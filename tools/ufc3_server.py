#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""UFC 3 online server for playing with a friend (run by the player who hosts).

Provides the redirector and local Nucleus endpoints. Blaze login and matchmaking are under
development; successful HTTP authentication alone does not establish an online game.

    python3 tools/ufc3_server.py [--ports 42230,443,80,10041]
"""
import argparse
import asyncio
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from blaze_tdf import *  # noqa: E402,F403
import ufc3_games  # noqa: E402
from ufc3_auth import CLIENT_ID, SERVER_ID, IdentityStore, user_authenticated, post_auth, user_added, user_updated, telemetry_config

IDENTITIES = IdentityStore()
ONLINE = {}  # blaze id -> account, one entry per connected game
CONNECTIONS = {}  # blaze id -> current writer


def stamp():
    return datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]


def describe(data: bytes) -> str:
    if data[:1] == b'\x16' and data[1:2] == b'\x03':
        return f'TLS handshake (version 3.{data[2]})'
    head = data.split(b'\r\n', 1)[0]
    if head[:4] in (b'GET ', b'POST', b'HEAD', b'PUT ') :
        return 'HTTP ' + head.decode('latin-1')
    return 'binary'


REDIRECTOR_PORT = 42230
BLAZE_PORT = 10041
MAX_HTTP_BODY = 1024 * 1024
RAW_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'blaze-raw')


async def close_writer(writer):
    writer.close()
    try:
        await writer.wait_closed()
    except ConnectionError:
        pass  # a client crash/reset must not raise a second server exception


async def read_http_request(reader: asyncio.StreamReader) -> bytes:
    """TCP may split headers and the body into arbitrary packets on either host platform."""
    head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), timeout=15)
    lengths = []
    for line in head.split(b'\r\n')[1:]:
        key, sep, value = line.partition(b':')
        if sep and key.strip().lower() == b'content-length':
            lengths.append(int(value.strip()))
        if sep and key.strip().lower() == b'transfer-encoding':
            raise ValueError('transfer encoding is not supported')
    if len(set(lengths)) > 1:
        raise ValueError('conflicting content lengths')
    length = lengths[0] if lengths else 0
    if not 0 <= length <= MAX_HTTP_BODY:
        raise ValueError('HTTP body exceeds limit')
    body = await asyncio.wait_for(reader.readexactly(length), timeout=15) if length else b''
    return head + body


def server_instance_xml(host_ip: str) -> bytes:
    """The redirector's answer: the Blaze server, without TLS (secure 0)."""
    ip_number = sum(int(part) << (8 * (3 - i)) for i, part in enumerate(host_ip.split('.')))
    return f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<serverinstanceinfo>
  <address member="0">
    <valu>
      <hostname>{host_ip}</hostname>
      <ip>{ip_number}</ip>
      <port>{BLAZE_PORT}</port>
    </valu>
  </address>
  <secure>0</secure>
  <trialservicename></trialservicename>
  <defaultdnsaddress>0</defaultdnsaddress>
</serverinstanceinfo>
""".encode()


async def redirector(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info('peername')
    local = writer.get_extra_info('sockname')[0]
    data = await read_http_request(reader)
    head, _, body = data.partition(b'\r\n\r\n')
    print(f'{stamp()} redirector: {head.split(b"\r\n", 1)[0].decode("latin-1")} from {peer[0]}', flush=True)
    answer = server_instance_xml(local)
    writer.write(b'HTTP/1.1 200 OK\r\nContent-Type: application/xml\r\nContent-Length: '
                 + str(len(answer)).encode() + b'\r\nConnection: close\r\n\r\n' + answer)
    await writer.drain()
    await close_writer(writer)
    print(f'{stamp()} redirector: Blaze at {local}:{BLAZE_PORT} (no TLS)', flush=True)


# Blaze components (core only: Ultimate Team, live events and the rest are not offered).
AUTHENTICATION, GAME_MANAGER, STATS, UTIL, MESSAGING, ASSOCIATION, GAME_REPORTING, USER_SESSIONS = (
    0x1, 0x4, 0x7, 0x9, 0xF, 0x19, 0x1C, 0x7802)
OFFERED = [AUTHENTICATION, GAME_MANAGER, STATS, UTIL, MESSAGING, ASSOCIATION, GAME_REPORTING, USER_SESSIONS]
CLIENT_CONFIG = {
    'pingPeriod': '15s', 'connIdleTimeout': '90s', 'defaultRequestTimeout': '60s',
    'voipHeadsetUpdateRate': '1000', 'xlspConnectionIdleTimeout': '300',
    # Nucleus OAuth: the client builds its login request with it (a missing key crashed the game).
    'redirect_uri': 'http://127.0.0.1/success',
    # Nucleus (EA accounts) endpoints: *.ea.com resolves to this server (443 at +40000).
    'nucleusConnect': 'https://accounts.ea.com',
    'nucleusProxy': 'https://gateway.ea.com',
    'nucleusPortal': 'https://signin.ea.com',
    'consoleClientId': 'UFC2018_PS4_CLIENT',
    # UFC getter 0x34fe90 reads projectId; telemetry initialization converts it
    # with strtol without a null check (caller 0x3b69f4). Local/disabled project.
    'projectId': '0',
    # Login state 12 reads these and strlen's the result. A missing key is a
    # null pointer (caller 0x34ffc0, easo_auth_url). Values stay on this server.
    'easo_auth_url': 'https://accounts.ea.com/easo/auth',
    'easo_sku': 'UFC2018_PS4',
    'easo_game_secret': 'local',
    'easo_xmshd_apikey': 'local',
    'easo_xmshd_base_url': 'https://accounts.ea.com/xmshd',
    'gameface_base_url': 'https://accounts.ea.com/gameface',
    'gameface_sku': 'UFC2018',
    'pulse_nps_survey_url': 'https://accounts.ea.com/nps',
    'pulse_nps_gameid': '0',
    'aruba_url': 'https://accounts.ea.com/aruba',
    'pinServerEnv': 'prod',
    'pinEventFilterCommaDelimitList': '',
}


def client_config_reply(request) -> bytes:
    """Util::fetchClientConfig: the same map for every config id (BlazeSDK, UfcConfig)."""
    return encode_struct([('CONF', MAP, (STRING, STRING, sorted(CLIENT_CONFIG.items())))])


QOS_PORT = 17502


def qos_config(host_ip: str):
    """QosConfigInfo (UFC reflection: BWPS, LNP, LTPS; QosPingSiteInfo: PSA, PSP).

    Matchmaking needs the client's QoS/network info (UserSessions updateNetworkInfo), which it
    only measures against the ping sites announced here. The sites are this server.
    """
    site = Struct(PSA=(STRING, host_ip), PSP=(INT, QOS_PORT))
    return Struct(BWPS=(STRUCT, site), LNP=(INT, 10), LTPS=(MAP, (STRING, STRUCT, [('local', site)])))


def preauth_reply(request, host_ip: str = '127.0.0.1') -> bytes:
    return encode_struct([
        ('ANON', INT, 0),
        ('ASRC', STRING, '303107'),
        ('CIDS', LIST, (INT, OFFERED)),
        ('CLID', STRING, 'UFC2018_PS4_SERVER'),  # the server's Nucleus client id (2nd /connect/auth)
        ('CNGN', STRING, ''),
        ('CONF', STRUCT, Struct(CONF=(MAP, (STRING, STRING, sorted(CLIENT_CONFIG.items()))))),
        ('ESRC', STRING, '303107'),
        ('INST', STRING, 'ufc-2018-ps4'),
        ('MINR', STRING, '0'),
        ('NASP', STRING, 'cem_ea_id'),
        ('PILD', STRING, ''),
        ('PLAT', STRING, 'ps4'),
        ('PTAG', STRING, ''),
        ('QOSS', STRUCT, qos_config(host_ip)),
        ('RSRC', STRING, '303107'),
        ('SVER', STRING, 'Blaze 15.1.1.6.5 (UFC 3 native port server)'),
    ])


CENSUS = 0xA
NOTIFICATIONS = 0x84F  # UFCNotifications, GetUFCNotifications is command 1
H2H_SEASONS = 0x815  # H2HSeasonsComponent; getH2HSeasonInfo is command 3 (the eboot's component table)


def h2h_season_info(request) -> bytes:
    """H2HSeasons getH2HSeasonInfo -> H2HSeasonInfo (UFC reflection).

    Online head-to-head (Quick Fight's ufcH2HUnranked) reads the season config: divisions with
    their max rounds, the player's division and stats. An empty reply leaves no divisions, and the
    online fight lobby never fills in. Ten divisions, a season running for a year, no history.
    """
    import time
    divisions = [Struct(MXRD=(INT, 5 if n == 1 else 3), NAME=(STRING, f'Division {n}'),
                        PTTS=(INT, (10 - n) * 100)) for n in range(1, 11)]
    config = Struct(BTDV=(INT, 10), DFMR=(INT, 3), DVCC=(LIST, (STRUCT, divisions)), NPDM=(INT, 0),
                    OHVT=(INT, 10), PLDV=(INT, 10), TCMR=(INT, 5), TCPT=(INT, 3), UFGS=(INT, 0))
    stats = Struct(CSET=(INT, int(time.time()) + 365 * 86400), CSID=(INT, 1), CURD=(INT, 10),
                   LOSS=(INT, 0), PNTS=(INT, 0), TCS=(STRUCT, Struct(ISCP=(INT, 0), TCP=(INT, 0))),
                   WINS=(INT, 0))
    featured = Struct(FFFL=(LIST, (STRUCT, [])), PPF=(INT, 0), TMST=(INT, 0))
    return encode_struct(Struct(CLCF=(STRUCT, config), FFIN=(STRUCT, featured),
                                GHRL=(LIST, (STRUCT, [])), STAT=(STRUCT, stats)))


def census_subscribe(request) -> bytes:
    """CensusData command 5 replies with SubscribeToCensusDataUpdatesResponse.

    The three periods are TimeValues. This eboot visits them as plain integers
    (wire type 0) holding microseconds. Type 0xB, type 0xE, and the census list
    were rejected or expired immediately. 60s here is 60_000_000, not 60.
    """
    return encode_struct([
        ('CNP', INT, 15_000_000),   # censusNotificationPeriod, 15s
        ('NTMT', INT, 60_000_000),  # notificationTimeout, 60s
        ('RTMT', INT, 60_000_000),  # resubscribeTimeout, 60s
    ])


def ufc_notifications(request) -> bytes:
    """FighterNet reads GetUFCNotifications. An empty reply is the unavailable screen.

    One accolade card titled Dummy is enough to show the feed is alive.
    """
    card = [
        ('LCCI', STRING, 'Dummy is online'),       # localizedContentInfo
        ('LTLI', STRING, 'Dummy'),                 # localizedTitleInfo
        ('NCID', INT, 1),                          # contentId
        ('NCTY', INT, 1),                          # contentType, accolades
    ]
    return encode_struct([
        ('FTVS', LIST, (STRUCT, [card])),          # uFCNotificationList
    ])


HANDLERS = {
    (UTIL, 0x7): preauth_reply,
    (UTIL, 0x1): client_config_reply,
    (UTIL, 0x5): lambda request: encode_struct(telemetry_config()),
    (CENSUS, 0x5): census_subscribe,
    (NOTIFICATIONS, 0x1): ufc_notifications,
    (H2H_SEASONS, 0x3): h2h_season_info,
}


# AssociationLists commands in the client's table, in the order of the command
# names: 4 getListForUser, 5 getLists, 10 subscribeToLists.
FRIEND_COMMANDS = {4, 5, 10}


def friend_lists(viewer) -> bytes:
    """One friend list containing every other player connected to this server."""
    others = [account.id for account in ONLINE.values() if account.id != viewer.id]
    info = [
        ('ALST', STRUCT, [          # list identification
            ('LNM', STRING, 'friend'),
            ('TYPE', INT, 1),
        ]),
        ('BIDS', LIST, (INT, others)),
    ]
    return encode_struct([
        ('LSDT', LIST, (STRUCT, [info])),  # listsInfo
    ])


async def blaze(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    peer = writer.get_extra_info('peername')
    print(f'{stamp()} blaze: client {peer[0]}:{peer[1]}', flush=True)
    buffer = bytearray()
    session = None
    online_id = None
    notes = []
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            buffer += data
            for frame in read_frames(buffer):
                if frame.msg_type == PING:
                    writer.write(Frame(frame.component, frame.command, frame.msg_num, PING_REPLY).encode())
                    continue
                decoded = True
                try:
                    request, consumed = decode_struct(frame.payload)
                    if consumed != len(frame.payload):
                        raise ValueError('trailing or truncated TDF data')
                    logged = Struct({tag: (kind, '<redacted>' if tag in ('AUTH', 'KEY') else value)
                                     for tag, (kind, value) in request.items()})
                    text = dump(logged)
                except Exception as error:  # keep serving; the raw bytes are saved
                    decoded = False
                    request, _ = decode_partial(frame.payload)
                    raw = os.path.join(RAW_DIR, f'{frame.component:x}-{frame.command:x}-{frame.msg_num}.bin')
                    os.makedirs(RAW_DIR, exist_ok=True)
                    with open(raw, 'wb') as f:
                        f.write(frame.payload)
                    text = (f'(partly decoded: {error}; raw payload in {raw})\n' + dump(request))
                handler = HANDLERS.get((frame.component, frame.command))
                known = handler or frame.component == ufc3_games.GAME_MANAGER or (frame.component, frame.command) in ((AUTHENTICATION, 0xa), (UTIL, 8))
                print(f'{stamp()} blaze: {frame}{"" if known else "  (no handler: empty reply)"}', flush=True)
                if text:
                    print('  ' + text.replace('\n', '\n  '), flush=True)
                if not session and frame.component not in (UTIL, AUTHENTICATION):
                    # A client that reconnected after a server restart resumes without logging in.
                    # Close it: the game then reports the lost connection and logs in again,
                    # instead of waiting forever on requests this server cannot attribute.
                    print(f'{stamp()} blaze: request before login from {peer[0]}; closing so the game '
                          f'logs in again', flush=True)
                    return
                if (frame.component, frame.command) == (AUTHENTICATION, 0xa):
                    try:
                        account, key, payload = IDENTITIES.login(request)
                    except ValueError as error:
                        # Never grant a session on a bogus/replayed code. Close until
                        # Fire2's error metadata contract is implemented and verified.
                        print(f'{stamp()} blaze: login rejected ({error})', flush=True)
                        return
                    session = (account, key)
                    previous = CONNECTIONS.get(account.id)
                    if previous is not None and previous is not writer:
                        send_game_notes(ufc3_games.leave(account.id, game_log, writer=previous))
                        previous.close()
                    CONNECTIONS[account.id] = writer
                    ONLINE[account.id] = account
                    online_id = account.id
                    print(f'{stamp()} blaze: authenticated account {account.id} ({account.name}), '
                          f'{len(ONLINE)} online', flush=True)
                elif (frame.component, frame.command) == (UTIL, 8):
                    if not session:
                        print(f'{stamp()} blaze: postAuth before login rejected', flush=True)
                        return
                    payload = post_auth(session[0])
                elif frame.component == ASSOCIATION and frame.command in FRIEND_COMMANDS and session:
                    payload = friend_lists(session[0])
                    print(f'{stamp()} friends: {session[0].name} sees '
                          f'{len(ONLINE) - 1} other player(s)', flush=True)
                elif (frame.component, frame.command) == (UTIL, 0x7):
                    payload = preauth_reply(request, writer.get_extra_info('sockname')[0])
                elif frame.component == ufc3_games.GAME_MANAGER and session:
                    try:
                        if CONNECTIONS.get(session[0].id) is not writer:
                            raise ufc3_games.GameError('request from an obsolete connection')
                        if not decoded:
                            raise ufc3_games.GameError('GameManager requires a fully decoded request')
                        result = ufc3_games.handle(frame.command, request, session[0], writer, peer[0], game_log)
                        if result is None:
                            # Explicitly accepted telemetry. No metrics consumer yet.
                            if frame.command != 0xAB:
                                raise ufc3_games.GameError('unsupported GameManager command',
                                                          ufc3_games.ERR_COMMAND_NOT_FOUND)
                            result = (b'', [])
                        payload, notes = result
                    except (ValueError, TypeError, IndexError, KeyError) as error:
                        code = getattr(error, 'code', ufc3_games.ERR_SYSTEM)
                        writer.write(Frame(frame.component, frame.command, frame.msg_num, ERROR_REPLY,
                                           metadata=encode_struct(Struct(ERRC=(INT, code)))).encode())
                        game_log(f'rejected command {frame.command:#x} for {session[0].name}: {error}')
                        continue
                else:
                    payload = handler(request) if handler else b''
                writer.write(Frame(frame.component, frame.command, frame.msg_num, REPLY, payload).encode())
                send_game_notes(notes)
                notes = []
                if (frame.component, frame.command) == (AUTHENTICATION, 0xa):
                    writer.write(Frame(USER_SESSIONS, 8, 0, NOTIFICATION,
                                       user_authenticated(*session)).encode())
                    writer.write(Frame(USER_SESSIONS, 2, 0, NOTIFICATION, user_added(session[0])).encode())
                    writer.write(Frame(USER_SESSIONS, 5, 0, NOTIFICATION, user_updated(session[0])).encode())
            await writer.drain()
    except ConnectionError:
        pass
    finally:
        if online_id is not None:
            if CONNECTIONS.get(online_id) is writer:
                CONNECTIONS.pop(online_id, None)
                ONLINE.pop(online_id, None)
            send_game_notes(ufc3_games.leave(online_id, game_log, writer=writer))
        await close_writer(writer)
    print(f'{stamp()} blaze: client {peer[0]}:{peer[1]} left', flush=True)
    writer.close()


def send_game_notes(notes):
    for target, note in notes:
        if not ufc3_games.live(target):
            continue
        try:
            target.write(note.encode())
        except ConnectionError:
            target.close()
        else:
            print(f'{stamp()} game: -> {note}', flush=True)


async def game_maintenance():
    while True:
        await asyncio.sleep(1)
        send_game_notes(ufc3_games.expire(game_log))


def game_log(text):
    print(f'{stamp()} game: {text}', flush=True)


def easo_login_answer(headers) -> bytes:
    """SportsLoginService::Login reads these response headers and rejects an empty one."""
    name = headers.get('X-UFC3-Online-Id', 'Player').replace('\r', '').replace('\n', '') or 'Player'
    try:
        user_id = int(headers.get('X-UFC3-Psn-Id', '1000000001'))
    except ValueError:
        user_id = 1000000001
    fields = {
        'EASW-Token': 'local-token',
        'EASW-Token-Version': '1',
        'EASW-Token-Expiry-Time': '1999999999',
        'EASW-Nucleus-Persona': str(user_id),
        'EASW-Nucleus-User': str(user_id),
        'EASW-Nucleus-Display-Name': name,
        'EASW-Userid': str(user_id),
        'EASW-Session': 'local-session',
    }
    head = ''.join(f'{key}: {value}\r\n' for key, value in fields.items())
    return (f'HTTP/1.1 200 OK\r\n{head}Content-Length: 0\r\nConnection: close\r\n\r\n').encode()


def json_answer(status, body):
    import json
    encoded = json.dumps(body).encode()
    return (f'HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {len(encoded)}\r\n'
            'Connection: close\r\n\r\n').encode() + encoded


def nucleus_answer(request: bytes) -> bytes:
    import urllib.parse
    from email.parser import BytesHeaderParser
    head, _, body = request.partition(b'\r\n\r\n')
    line, _, raw_headers = head.partition(b'\r\n')
    headers = BytesHeaderParser().parsebytes(raw_headers + b'\r\n\r\n')
    target = line.decode('latin-1').split(' ')[1]
    url = urllib.parse.urlsplit(target)
    query = dict(urllib.parse.parse_qsl(url.query))
    form = dict(urllib.parse.parse_qsl(body.decode('utf-8')))
    try:
        psn_id = int(headers.get('X-UFC3-Psn-Id', '0'))
        if url.path == '/connect/auth':
            client_id = query.get('client_id', '')
            account = IDENTITIES.account(psn_id, headers.get('X-UFC3-Online-Id', 'Player'))
            code = IDENTITIES.issue_code(account, client_id)
            redirect = query.get('redirect_uri', 'http://127.0.0.1/success')
            # The client consumes this Location; it does not fetch an external URI.
            if '\r' in redirect or '\n' in redirect:
                raise ValueError('invalid redirect URI')
            location = redirect + ('&' if '?' in redirect else '?') + 'code=' + code
            print(f'{stamp()} nucleus: issued code for {client_id}, account {account.id}', flush=True)
            return (f'HTTP/1.1 302 Found\r\nLocation: {location}\r\nContent-Length: 0\r\n'
                    'Connection: close\r\n\r\n').encode()
        if url.path == '/connect/token':
            # UFC sends POST with the grant in the URL and an empty body.
            # Also accept form bodies for other clients, rejecting conflicts.
            if any(key in query and query[key] != value for key, value in form.items()):
                raise ValueError('conflicting token parameters')
            grant = query | form
            if grant.get('grant_type') != 'authorization_code':
                raise ValueError('unsupported token grant')
            token = IDENTITIES.exchange_token(grant.get('code', ''), grant.get('client_id', ''), psn_id)
            return json_answer('200 OK', {'access_token': token, 'token_type': 'Bearer',
                               'expires_in': 86400, 'id_token': ''})
        if url.path == '/easo/auth/nv2/auth':
            print(f'{stamp()} easo: login for {headers.get("X-UFC3-Online-Id", "Player")}', flush=True)
            return easo_login_answer(headers)
        if url.path.startswith('/aruba/'):
            return json_answer('200 OK', {'messages': []})
        if url.path == '/xmshd/search':
            preview = body[:180].decode('utf-8', 'replace').replace('\n', ' ')
            print(f'{stamp()} xmshd search {len(body)} bytes: {preview}', flush=True)
            return json_answer('200 OK', {'results': [], 'count': 0, 'totalCount': 0})
        if url.path == '/connect/tokeninfo':
            account, client_id = IDENTITIES.token_account(query.get('access_token', ''), psn_id)
            return json_answer('200 OK', {'client_id': client_id, 'scope': 'basic.identity basic.persona signin offline',
                'expires_in': 86400, 'pid_id': str(account.id), 'pid_type': 'NUCLEUS',
                'user_id': str(account.id), 'persona_id': account.id, 'is_underage': False,
                'stopProcess': 'OFF', 'telemetry_id': str(account.id)})
    except ValueError as error:
        print(f'{stamp()} nucleus: rejected {url.path}: {error}; '
              f'form fields={sorted(form)}, query fields={sorted(query)}, '
              f'content type={headers.get("Content-Type", "")}', flush=True)
        return json_answer('400 Bad Request', {'error': 'invalid_grant'})
    return b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, port: int):
    if port == BLAZE_PORT:
        return await blaze(reader, writer)
    try:
        if port == REDIRECTOR_PORT:
            return await redirector(reader, writer)
        data = await read_http_request(reader)
        # Log the route rather than embedded OAuth credentials and tokens.
        target = data.split(b'\r\n', 1)[0].split(b' ')
        route = target[1].split(b'?', 1)[0] if len(target) >= 2 else b'?'
        print(f'{stamp()} port {port}: HTTP {route.decode("latin-1")}', flush=True)
        writer.write(nucleus_answer(data))
        await writer.drain()
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError,
            ConnectionError, ValueError) as error:
        print(f'{stamp()} port {port}: request rejected ({type(error).__name__})', flush=True)
    finally:
        await close_writer(writer)


class QosProbe(asyncio.DatagramProtocol):
    """QoS ping site (UDP): logs each probe and echoes it back unchanged (protocol under study)."""

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        print(f'{stamp()} qos: {len(data)} B from {addr[0]}:{addr[1]}: {data[:64].hex(" ")}', flush=True)
        self.transport.sendto(data, addr)


async def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--ports', default='42230,40443,40080,10041,10744,17502')  # 443/80 arrive at +40000
    p.add_argument('--bind', default='0.0.0.0')
    args = p.parse_args()
    servers = []
    for port in (int(x) for x in args.ports.split(',') if x):
        try:
            servers.append(await asyncio.start_server(
                lambda r, w, port=port: handle(r, w, port), args.bind, port))
            print(f'{stamp()} listening on {args.bind}:{port}', flush=True)
        except OSError as error:
            for server in servers:
                server.close()
                await server.wait_closed()
            raise SystemExit(f'Cannot listen on {args.bind}:{port}: {error}') from error
    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(QosProbe, local_addr=(args.bind, QOS_PORT))
    print(f'{stamp()} qos ping site on udp {args.bind}:{QOS_PORT}', flush=True)
    await asyncio.gather(game_maintenance(), *(s.serve_forever() for s in servers))


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
