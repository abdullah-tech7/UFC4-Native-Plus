"""Transport regressions for the local server; these do not prove in-game P2P works."""
import asyncio
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ufc3_server import read_http_request
from ufc3_auth import CLIENT_ID, SERVER_ID, IdentityStore, user_authenticated, post_auth, PS4_PLATFORM
from blaze_tdf import INT, STRING, STRUCT, TIME, Struct, decode_struct, Frame, read_frames, MESSAGE, REPLY, NOTIFICATION
from unittest.mock import patch
import json
import urllib.parse
import ufc3_server


class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.store = IdentityStore()

    def test_ufc_config_includes_easo_url(self):
        payload = ufc3_server.preauth_reply(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        conf = dict(fields['CONF'][1]['CONF'][1][2])
        self.assertEqual(conf['easo_auth_url'], 'https://accounts.ea.com/easo/auth')
        self.assertEqual(conf['projectId'], '0')

    def test_easo_login_returns_required_headers(self):
        request = (b'POST /easo/auth/nv2/auth HTTP/1.0\r\n'
                   b'X-UFC3-Online-Id: Host\r\nX-UFC3-Psn-Id: 1000000001\r\n\r\n')
        answer = ufc3_server.nucleus_answer(request)
        self.assertTrue(answer.startswith(b'HTTP/1.1 200 OK\r\n'))
        for header in (b'EASW-Token: ', b'EASW-Session: ', b'EASW-Nucleus-Display-Name: Host',
                       b'EASW-Token-Expiry-Time: '):
            self.assertIn(header, answer)

    def test_census_subscribe_reply_has_timeouts_and_empty_list(self):
        payload = ufc3_server.census_subscribe(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        self.assertEqual(fields['CNP'], (INT, 15_000_000))
        self.assertEqual(fields['RTMT'], (INT, 60_000_000))
        self.assertNotIn('TDFL', fields)
        self.assertEqual(payload[3], INT)

    def test_fighternet_reply_contains_dummy_card(self):
        payload = ufc3_server.ufc_notifications(None)
        fields, consumed = decode_struct(payload)
        self.assertEqual(consumed, len(payload))
        cards = fields['FTVS'][1][1]
        self.assertEqual(len(cards), 1)
        self.assertEqual(cards[0]['LTLI'], (STRING, 'Dummy'))
        self.assertEqual(cards[0]['NCTY'], (INT, 1))

    def test_friend_list_names_the_other_player(self):
        from ufc3_auth import Account
        linux = Account(1000000002, 11, 'Player_1')
        windows = Account(1000000001, 22, 'myzkW')
        ufc3_server.ONLINE.clear()
        ufc3_server.ONLINE[linux.id] = linux
        ufc3_server.ONLINE[windows.id] = windows
        try:
            payload = ufc3_server.friend_lists(linux)
            fields, consumed = decode_struct(payload)
            self.assertEqual(consumed, len(payload))
            info = fields['LSDT'][1][1][0]
            self.assertEqual(info['ALST'][1]['TYPE'], (INT, 1))
            self.assertEqual(info['BIDS'][1][1], [windows.id])
        finally:
            ufc3_server.ONLINE.clear()

    def test_identity_remains_the_same_through_oauth_and_blaze(self):
        first = self.store.account(123, 'Host')
        second = self.store.account(456, 'Guest')
        self.assertNotEqual(first.id, second.id)
        for account in (first, second):
            client_code = self.store.issue_code(account, CLIENT_ID)
            token = self.store.exchange_token(client_code, CLIENT_ID, account.psn_id)
            self.assertEqual(self.store.token_account(token, account.psn_id), (account, CLIENT_ID))
            self.assertIs(self.store.account(account.psn_id, 'Renamed'), account)
            server_code = self.store.issue_code(account, SERVER_ID)
            request = Struct(AUTH=(STRING, server_code), EXTI=(INT, account.psn_id))
            actual, key, response = self.store.login(request)
            self.assertIs(actual, account)
            fields, consumed = decode_struct(response)
            self.assertEqual(consumed, len(response))
            self.assertEqual(set(fields), {'ANON', 'SESS', 'SPAM', 'UNDR'})
            session = fields['SESS'][1]
            self.assertEqual(fields['SESS'][0], STRUCT)
            self.assertEqual(session['BUID'], (INT, account.id))
            self.assertEqual(session['UID'], (INT, account.id))
            self.assertEqual(session['KEY'], (STRING, key))
            self.assertEqual(session['PDTL'][1]['XREF'], (INT, account.psn_id))
            self.assertEqual(session['PDTL'][1]['DSNM'], (STRING, account.name))
            self.assertEqual(session['PDTL'][1]['PLAT'], (INT, PS4_PLATFORM))
            notice, _ = decode_struct(user_authenticated(account, key))
            self.assertEqual(notice['KEY'], session['KEY'])
            self.assertEqual(notice['BUID'], session['BUID'])
            self.assertEqual(notice['PLAT'], (INT, 20))
            post, _ = decode_struct(post_auth(account))
            self.assertEqual(set(post), {'TELE', 'TICK', 'UROP'})
            self.assertEqual(post['UROP'][1]['UID'], session['UID'])
            self.assertEqual(post['UROP'][1]['TMOP'], (INT, 0))
            self.assertEqual(post['TELE'][1]['PORT'], (INT, 0))
            with self.assertRaises(ValueError):
                self.store.login(request)  # one-use code

    def test_wrong_account_or_client_cannot_redeem_code(self):
        account = self.store.account(123, 'Host')
        code = self.store.issue_code(account, CLIENT_ID)
        for client, psn in ((SERVER_ID, 123), (CLIENT_ID, 456)):
            with self.assertRaises(ValueError):
                self.store.redeem_code(code, client, psn)
        self.assertIs(self.store.redeem_code(code, CLIENT_ID, 123), account)

    def test_expired_code_is_rejected(self):
        account = self.store.account(123, 'Host')
        with patch('ufc3_auth.time.monotonic', return_value=100):
            code = self.store.issue_code(account, SERVER_ID)
        with patch('ufc3_auth.time.monotonic', return_value=401):
            with self.assertRaises(ValueError):
                self.store.redeem_code(code, SERVER_ID, 123)

    def test_bogus_token_and_malformed_login_are_rejected(self):
        with self.assertRaises(ValueError):
            self.store.token_account('bogus', 123)
        with self.assertRaises(ValueError):
            self.store.login(Struct(AUTH=(STRING, 'bogus')))

    def test_game_empty_post_token_grant_and_session_chain(self):
        def call(path, params, body=b''):
            target = path + '?' + urllib.parse.urlencode(params)
            request = (f'POST {target} HTTP/1.0\r\nX-UFC3-Psn-Id: 123\r\n'
                       'X-UFC3-Online-Id: Host\r\nContent-Type: application/x-www-form-urlencoded\r\n'
                       f'Content-Length: {len(body)}\r\n\r\n').encode() + body
            return ufc3_server.nucleus_answer(request)
        def code(client):
            response = call('/connect/auth', {'client_id': client})
            self.assertTrue(response.startswith(b'HTTP/1.1 302'))
            location = next(line for line in response.split(b'\r\n') if line.startswith(b'Location:'))
            return urllib.parse.parse_qs(urllib.parse.urlsplit(location.decode().split(' ', 1)[1]).query)['code'][0]
        with patch.object(ufc3_server, 'IDENTITIES', self.store):
            client_code = code(CLIENT_ID)
            response = call('/connect/token', {'grant_type': 'authorization_code',
                            'client_id': CLIENT_ID, 'code': client_code})
            self.assertTrue(response.startswith(b'HTTP/1.1 200'))
            token = json.loads(response.split(b'\r\n\r\n', 1)[1])['access_token']
            info = json.loads(call('/connect/tokeninfo', {'access_token': token}).split(b'\r\n\r\n', 1)[1])
            account, _, _ = self.store.login(Struct(AUTH=(STRING, code(SERVER_ID)), EXTI=(INT, 123)))
            self.assertEqual(info['persona_id'], account.id)
            replay = call('/connect/token', {'grant_type': 'authorization_code',
                          'client_id': CLIENT_ID, 'code': client_code})
            self.assertTrue(replay.startswith(b'HTTP/1.1 400'))


class HttpTransportTests(unittest.IsolatedAsyncioTestCase):
    async def test_fragmented_headers_and_body(self):
        reader = asyncio.StreamReader()
        task = asyncio.create_task(read_http_request(reader))
        request = b'POST /connect/token HTTP/1.0\r\nContent-Length: 7\r\n\r\ncode=42'
        for byte in request:
            reader.feed_data(bytes([byte]))
            await asyncio.sleep(0)
        self.assertEqual(await task, request)

    async def test_does_not_consume_next_request(self):
        reader = asyncio.StreamReader()
        one = b'GET /connect/auth HTTP/1.0\r\n\r\n'
        reader.feed_data(one + one)
        self.assertEqual(await read_http_request(reader), one)
        self.assertEqual(await read_http_request(reader), one)

    async def test_truncated_body_fails(self):
        reader = asyncio.StreamReader()
        reader.feed_data(b'POST / HTTP/1.0\r\nContent-Length: 10\r\n\r\nx')
        reader.feed_eof()
        with self.assertRaises(asyncio.IncompleteReadError):
            await read_http_request(reader)

    async def test_rejects_ambiguous_or_oversized_lengths(self):
        for headers in (b'Content-Length: 1\r\nContent-Length: 2',
                        b'Content-Length: -1', b'Content-Length: 1048577',
                        b'Transfer-Encoding: chunked'):
            reader = asyncio.StreamReader()
            reader.feed_data(b'POST / HTTP/1.0\r\n' + headers + b'\r\n\r\n')
            with self.assertRaises(ValueError):
                await read_http_request(reader)


class BlazeSessionTests(unittest.IsolatedAsyncioTestCase):
    async def test_login_notifications_and_postauth_over_tcp(self):
        store = IdentityStore()
        account = store.account(123, 'Host')
        code = store.issue_code(account, SERVER_ID)
        async def read_frame(reader):
            head = await asyncio.wait_for(reader.readexactly(16), 2)
            length = int.from_bytes(head[:4], 'big') + int.from_bytes(head[4:6], 'big')
            body = await asyncio.wait_for(reader.readexactly(length), 2)
            return read_frames(bytearray(head + body))[0]
        completed = asyncio.Event()
        async def connection(reader, writer):
            try:
                await ufc3_server.blaze(reader, writer)
            finally:
                completed.set()
        with patch.object(ufc3_server, 'IDENTITIES', store):
            async with await asyncio.start_server(connection, '127.0.0.1', 0) as server:
                port = server.sockets[0].getsockname()[1]
                reader, writer = await asyncio.open_connection('127.0.0.1', port)
                try:
                    from blaze_tdf import encode_struct
                    login = encode_struct(Struct(AUTH=(STRING, code), EXTI=(INT, account.psn_id)))
                    writer.write(Frame(1, 10, 42, MESSAGE, login).encode())
                    await writer.drain()
                    reply = await read_frame(reader)
                    self.assertEqual((reply.msg_type, reply.component, reply.command, reply.msg_num), (REPLY, 1, 10, 42))
                    session = decode_struct(reply.payload)[0]['SESS'][1]
                    self.assertEqual(session['BUID'], (INT, account.id))
                    for command in (8, 2, 5):
                        notice = await read_frame(reader)
                        self.assertEqual((notice.msg_type, notice.component, notice.command), (NOTIFICATION, 0x7802, command))
                        fields = decode_struct(notice.payload)[0]
                        if command == 8:
                            self.assertEqual(fields['KEY'], session['KEY'])
                        elif command == 2:
                            self.assertEqual(fields['USER'][1]['EXID'], (INT, account.psn_id))
                        else:
                            self.assertEqual(fields['ID'], (INT, account.id))
                    writer.write(Frame(9, 8, 43, MESSAGE).encode())
                    await writer.drain()
                    post = await read_frame(reader)
                    self.assertEqual((post.msg_type, post.component, post.command, post.msg_num), (REPLY, 9, 8, 43))
                    self.assertEqual(decode_struct(post.payload)[0]['UROP'][1]['UID'], (INT, account.id))
                    # Failures use Fire2 metadata, preserve correlation, and keep TCP usable.
                    from blaze_tdf import ERROR_REPLY, MAP, GENERIC, encode_tag, encode_int
                    import tempfile
                    import struct
                    prefix = encode_struct(Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14'))),
                                                  SCNM=(STRING, 'ufcH2HUnranked')))
                    # A fully decoded criterion with an invalid generic value.
                    invalid = prefix + encode_tag('SCNA') + bytes([MAP, STRING, GENERIC]) + encode_int(1)
                    invalid += encode_int(len('UFC_WEIGHT_CLASS') + 1) + b'UFC_WEIGHT_CLASS\0' + b'\0'
                    # A truncated generic after otherwise valid fields must never enter the queue.
                    truncated = invalid[:-1] + b'\1'
                    with tempfile.TemporaryDirectory() as raw_dir, patch.object(ufc3_server, 'RAW_DIR', raw_dir):
                        for number, command, payload in ((44, 0x10, invalid), (45, 0x10, truncated),
                                                         (46, 0xD, b''), (47, 0xFFFF, b'')):
                            writer.write(Frame(4, command, number, MESSAGE, payload).encode())
                            await writer.drain()
                            failure = await read_frame(reader)
                            self.assertEqual((failure.msg_type, failure.component, failure.command, failure.msg_num),
                                             (ERROR_REPLY, 4, command, number))
                            self.assertEqual(failure.payload, b'')
                            expected = 0x40010000 if number < 46 else 0x40030000
                            self.assertEqual(decode_struct(failure.metadata)[0]['ERRC'], (INT, expected))
                            encoded = failure.encode()
                            self.assertEqual(struct.unpack('>IHHH', encoded[:10]),
                                             (0, len(failure.metadata), 4, command))
                            self.assertEqual(encoded[13], 0x60)  # Fire2 ERROR_REPLY, not an empty success.
                            self.assertFalse(ufc3_server.ufc3_games.SEARCHES)
                    writer.write(Frame(9, 8, 48, MESSAGE).encode())
                    await writer.drain()
                    self.assertEqual((await read_frame(reader)).msg_type, REPLY)

                finally:
                    await ufc3_server.close_writer(writer)
                await asyncio.wait_for(completed.wait(), 2)


class GameManagerTests(unittest.TestCase):
    def test_matchmaking_setup_has_schema_correct_two_player_roster(self):
        from ufc3_auth import Account
        from blaze_tdf import BLOB, UNION, Union
        import ufc3_games

        ufc3_games.GAMES.clear()
        ufc3_games.SEARCHES.clear()
        host = Account(1000000001, 111, 'WindowsHost')
        guest = Account(1000000002, 222, 'LinuxGuest')
        def network_address(ip):
            return Struct(IP=(INT, int.from_bytes(bytes(map(int, ip.split('.'))), 'big')),
                          MACI=(INT, 0), PORT=(INT, 3659))
        def search(account, peer_ip, ip):
            pnet = Union(2, 'VALU', STRUCT, Struct(INIP=(STRUCT, network_address(ip)),
                                                  EXIP=(STRUCT, network_address(ip)), MACI=(INT, 0)))
            request = Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14'), PNET=(UNION, pnet))),
                             SCNM=(STRING, 'ufcH2HUnranked'))
            return ufc3_games.matchmake(request, account, object(), peer_ip, lambda *_: None)

        try:
            search(host, '192.168.1.129', '192.168.1.129')
            _, notifications = search(guest, '192.168.1.2', '192.168.1.2')
            setups = [frame for _, frame in notifications
                      if frame.component == ufc3_games.GAME_MANAGER
                      and frame.command == ufc3_games.NOTIFY_GAME_SETUP]
            self.assertEqual(len(setups), 2)
            for frame in setups:
                fields, consumed = decode_struct(frame.payload)
                self.assertEqual(consumed, len(frame.payload))
                roster = fields['PROS'][1][1]
                self.assertEqual({p['NAME'][1] for p in roster}, {'WindowsHost', 'LinuxGuest'})
                for player in roster:
                    self.assertIn('EXBL', player)
                    self.assertIn('BLOB', player)
                    self.assertNotIn('EXBB', player)  # EXBB belongs to UserIdentification, not this class.
        finally:
            ufc3_games.GAMES.clear()
            ufc3_games.SEARCHES.clear()

    @staticmethod
    def _search(account, peer_ip, weight_classes=None, **criteria):
        """startMatchmakingScenario as UFC sends it (SCNA as blaze_tdf decodes it)."""
        from blaze_tdf import MAP, UNION, Union
        import ufc3_games

        def network_address():
            return Struct(IP=(INT, int.from_bytes(bytes(map(int, peer_ip.split('.'))), 'big')),
                          MACI=(INT, 0), PORT=(INT, 3659))
        def generic(values):
            return ('generic', 2806890242, (STRING, values))
        names = {'UFC_CLOCK_TYPE': ['abstain'], 'UFC_CUSTOM_FIGHTER': ['abstain'],
                 'UFC_FIGHT_RULES': ['abstain'], 'UFC_MAX_ROUNDS': ['abstain'],
                 'UFC_WEIGHT_CLASS': weight_classes or [str(i) for i in range(10)]}
        names.update(criteria)
        pnet = Union(2, 'VALU', STRUCT, Struct(INIP=(STRUCT, network_address()),
                                              EXIP=(STRUCT, network_address()), MACI=(INT, 0)))
        request = Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14'), PNET=(UNION, pnet))),
                         SCNA=(MAP, (STRING, 12, [(k, generic(v)) for k, v in sorted(names.items())])),
                         SCNM=(STRING, 'ufcH2HUnranked'))
        return ufc3_games.matchmake(request, account, object(), peer_ip, lambda *_: None)

    def _matched_games(self, host_classes, guest_classes, **guest_criteria):
        """Both players' NotifyGameSetup payloads, decoded (empty if they were not matched)."""
        from ufc3_auth import Account
        import ufc3_games

        ufc3_games.GAMES.clear()
        ufc3_games.SEARCHES.clear()
        self.addCleanup(ufc3_games.GAMES.clear)
        self.addCleanup(ufc3_games.SEARCHES.clear)
        host = Account(1000000001, 111, 'WindowsHost')
        guest = Account(1000000002, 222, 'LinuxGuest')
        self._search(host, '192.168.1.129', host_classes)
        _, notifications = self._search(guest, '192.168.1.2', guest_classes, **guest_criteria)
        out = []
        for _, frame in notifications:
            if frame.component == ufc3_games.GAME_MANAGER and frame.command == ufc3_games.NOTIFY_GAME_SETUP:
                fields, consumed = decode_struct(frame.payload)
                self.assertEqual(consumed, len(frame.payload))
                out.append(fields['GAME'][1])
        return out

    def test_matched_game_has_the_attributes_ufc_reads(self):
        games = self._matched_games(None, None)
        self.assertEqual(len(games), 2)
        for game in games:
            attributes = dict(game['ATTR'][1][2])
            # 0x358380 reads these six; all abstain except the weight class.
            self.assertEqual(set(attributes), {'GameMode', 'MaxRounds', 'ClockType',
                                               'CustomFighterOption', 'FightRules', 'WeightClass'})
            self.assertEqual(attributes['GameMode'], '4')
            self.assertIn(attributes['MaxRounds'], ('3', '5'))
            self.assertIn(attributes['ClockType'], ('0', '1'))
            self.assertIn(attributes['CustomFighterOption'], ('0', '1'))
            self.assertEqual(attributes['FightRules'], '0')
            self.assertIn(int(attributes['WeightClass']), range(10))

    def test_weight_class_is_one_both_players_accept(self):
        games = self._matched_games(['2', '5', '7'], ['5', '7', '9'])
        self.assertEqual(len(games), 2)
        for game in games:
            self.assertIn(dict(game['ATTR'][1][2])['WeightClass'], ('5', '7'))
        games = self._matched_games(['3'], None)
        self.assertEqual({dict(g['ATTR'][1][2])['WeightClass'] for g in games}, {'3'})

    def test_players_without_a_common_weight_class_are_not_matched(self):
        import ufc3_games
        self.assertEqual(self._matched_games(['0', '1'], ['8', '9']), [])
        self.assertEqual(len(ufc3_games.SEARCHES), 2)
        self.assertEqual(ufc3_games.GAMES, {})

    def test_concrete_criteria_are_honoured(self):
        games = self._matched_games(None, None, UFC_MAX_ROUNDS=['3'], UFC_CLOCK_TYPE=['0'],
                                    UFC_CUSTOM_FIGHTER=['1'])
        attributes = dict(games[0]['ATTR'][1][2])
        self.assertEqual((attributes['MaxRounds'], attributes['ClockType'], attributes['CustomFighterOption']),
                         ('3', '0', '1'))
        # A value the server has no default for still passes through when the other abstains.
        games = self._matched_games(None, None, UFC_FIGHT_RULES=['2'])
        self.assertEqual({dict(g['ATTR'][1][2])['FightRules'] for g in games}, {'2'})

    def test_game_data_has_no_tags_outside_ufcs_schema(self):
        for game in self._matched_games(None, None):
            for tag in ('GVER', 'HSES', 'IGNO', 'TCAP'):
                self.assertNotIn(tag, game)
            self.assertIn('HSES', game['PHST'][1])  # it belongs to HostInfo
            self.assertEqual(game['VSTR'], (STRING, '1.14'))


class RoomLifecycleTests(unittest.TestCase):
    class Writer:
        def __init__(self):
            self.closed = False
            self.frames = []

        def is_closing(self):
            return self.closed

        def write(self, data):
            self.frames.extend(read_frames(bytearray(data)))

        def close(self):
            self.closed = True

    def setUp(self):
        import ufc3_games as gm
        from ufc3_auth import Account
        self.gm = gm
        gm.GAMES.clear()
        gm.SEARCHES.clear()
        self.addCleanup(gm.GAMES.clear)
        self.addCleanup(gm.SEARCHES.clear)
        self.accounts = [Account(100 + i, i + 1, f'Player{i}') for i in range(3)]
        self.writers = [self.Writer() for _ in self.accounts]

    def call(self, command, who=0, **fields):
        return self.gm.handle(command, Struct(fields), self.accounts[who], self.writers[who],
                              '192.168.1.10', lambda *_: None)

    def room(self, join=True):
        body, _ = self.call(self.gm.CREATE_GAME)
        gid = decode_struct(body)[0]['GID'][1]
        game = self.gm.GAMES[gid]
        if join:
            self.call(self.gm.JOIN_GAME, 1, GID=(INT, gid))
        return game

    def connect(self, game):
        from blaze_tdf import OBJID
        notes = []
        for who, target in ((0, 1), (1, 0)):
            _, out = self.call(self.gm.MESH_ENDPOINTS_CONNECTED, who, GID=(INT, game.gid),
                               TCG=(OBJID, (self.gm.USER_SESSIONS, 2, self.accounts[target].id)))
            notes += out
        return notes

    def search(self, who, version='1.14', scenario='ufcH2HUnranked', **extra):
        body, notes = self.call(self.gm.START_MATCHMAKING_SCENARIO, who,
                                CMGD=(STRUCT, Struct(GVER=(STRING, version))),
                                SCNM=(STRING, scenario), **extra)
        return decode_struct(body)[0]['MSID'][1], notes

    @staticmethod
    def bodies(notes, command):
        return [decode_struct(frame.payload)[0] for _, frame in notes if frame.command == command]

    def test_guest_leaves_without_tcp_close_and_can_join_again(self):
        from blaze_tdf import OBJID
        game = self.room()
        self.connect(game)
        _, notes = self.call(self.gm.LEAVE_GAME_BY_GROUP, 1, GID=(INT, game.gid),
                             PID=(INT, self.accounts[1].id), REAS=(INT, 8), CNTX=(INT, 42),
                             BTPL=(OBJID, (self.gm.USER_SESSIONS, 2, self.accounts[1].id)))
        self.assertEqual(game.players, [game.host])
        self.assertEqual(game.links, set())
        self.assertFalse(self.writers[1].closed)
        removed = self.bodies(notes, self.gm.NOTIFY_PLAYER_REMOVED)
        self.assertEqual(len(removed), 2)
        self.assertEqual(removed[0]['REAS'], (INT, 8))
        self.assertEqual(removed[0]['CNTX'], (INT, 42))
        self.call(self.gm.JOIN_GAME, 1, GID=(INT, game.gid))
        self.assertEqual(game.find(self.accounts[1].id).slot, 1)

    def test_host_leaves_notifies_both_and_both_can_search_again(self):
        game = self.room()
        self.connect(game)
        _, notes = self.call(self.gm.REMOVE_PLAYER, GID=(INT, game.gid), PID=(INT, self.accounts[0].id),
                             REAS=(INT, 7))
        self.assertEqual(self.gm.GAMES, {})
        self.assertEqual(game.players, [])
        self.assertEqual(game.links, set())
        removed = self.bodies(notes, self.gm.NOTIFY_GAME_REMOVED)
        self.assertEqual(len(removed), 2)
        self.assertEqual(removed[0]['REAS'], (INT, 4))  # UFC HOST_LEAVING, not Blaze3's 2.
        self.search(0)
        self.search(1)
        self.assertEqual(len(self.gm.GAMES), 1)

    def test_host_kick_removes_target_and_destroy_removes_room(self):
        game = self.room()
        self.connect(game)
        self.call(self.gm.REMOVE_PLAYER, GID=(INT, game.gid), PID=(INT, self.accounts[1].id), REAS=(INT, 9))
        self.assertEqual(game.players, [game.host])
        body, notes = self.call(self.gm.DESTROY_GAME, GID=(INT, game.gid), REAS=(INT, 0))
        self.assertEqual(decode_struct(body)[0]['GID'], (INT, game.gid))
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_GAME_REMOVED)[0]['REAS'], (INT, 0))
        self.assertFalse(self.gm.GAMES)

    def test_transport_disconnect_cleans_roster_and_old_socket_cannot_remove_new_room(self):
        game = self.room()
        self.connect(game)
        notes = self.gm.leave(self.accounts[1].id, lambda *_: None, writer=self.writers[1])
        self.assertEqual(game.players, [game.host])
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_PLAYER_REMOVED)[0]['REAS'], (INT, 3))
        old_writer = self.writers[1]
        self.writers[1] = self.Writer()
        self.call(self.gm.JOIN_GAME, 1, GID=(INT, game.gid))
        self.assertEqual(self.gm.leave(self.accounts[1].id, lambda *_: None, writer=old_writer), [])
        self.assertEqual(len(game.players), 2)
        self.writers[0].closed = True
        notes = self.gm.expire(lambda *_: None)
        self.assertFalse(self.gm.GAMES)
        self.assertTrue(self.bodies(notes, self.gm.NOTIFY_GAME_REMOVED))
        self.assertTrue(all(writer is self.writers[1] for writer, _ in notes))

    def test_mesh_loss_removes_target_and_links(self):
        from blaze_tdf import OBJID
        for command in (self.gm.MESH_ENDPOINTS_DISCONNECTED, self.gm.MESH_ENDPOINTS_CONNECTION_LOST):
            with self.subTest(command=command):
                game = self.room()
                self.connect(game)
                _, notes = self.call(command, GID=(INT, game.gid),
                                     TCG=(OBJID, (self.gm.USER_SESSIONS, 2, self.accounts[1].id)))
                self.assertEqual(game.players, [game.host])
                self.assertFalse(game.links)
                self.assertEqual(self.bodies(notes, self.gm.NOTIFY_PLAYER_REMOVED)[0]['REAS'], (INT, 2))
                self.gm.GAMES.clear()

    def test_invalid_gid_authority_and_transition_do_not_mutate(self):
        from blaze_tdf import MAP, OBJID
        game = self.room()
        cases = [
            (self.gm.ADVANCE_GAME_STATE, 0, {'GID': (INT, 9999), 'GSTA': (INT, self.gm.PRE_GAME)}),
            (self.gm.ADVANCE_GAME_STATE, 1, {'GSTA': (INT, self.gm.PRE_GAME)}),
            (self.gm.ADVANCE_GAME_STATE, 2, {'GSTA': (INT, self.gm.PRE_GAME)}),
            (self.gm.ADVANCE_GAME_STATE, 0, {'GID': (STRING, str(game.gid)), 'GSTA': (INT, self.gm.PRE_GAME)}),
            (self.gm.ADVANCE_GAME_STATE, 0, {'GSTA': (INT, self.gm.IN_GAME)}),
            (self.gm.ADVANCE_GAME_STATE, 0, {'GSTA': (INT, 0x999)}),
            (self.gm.FINALIZE_GAME_CREATION, 1, {}),
            (self.gm.DESTROY_GAME, 1, {}),
            (self.gm.SET_GAME_SETTINGS, 1, {'GSET': (INT, 42)}),
            (self.gm.SET_GAME_ATTRIBUTES, 1, {'ATTR': (MAP, (STRING, STRING, [('x', 'y')]))}),
            (self.gm.REMOVE_PLAYER, 1, {'PID': (INT, self.accounts[0].id)}),
            (self.gm.REMOVE_PLAYER, 0, {'PID': (INT, 9999)}),
            (self.gm.MESH_ENDPOINTS_CONNECTED, 0, {'TCG': (INT, self.accounts[1].id)}),
            (self.gm.MESH_ENDPOINTS_CONNECTED, 0, {'TCG': (OBJID, (4, 2, self.accounts[1].id))}),
            (self.gm.MESH_ENDPOINTS_CONNECTED, 0, {'TCG': (OBJID, (self.gm.USER_SESSIONS, 1, self.accounts[1].id))}),
            (self.gm.MESH_ENDPOINTS_CONNECTED, 0, {'TCG': (OBJID, (self.gm.USER_SESSIONS, 2, 9999))}),
        ]
        before = game.setup(game.host)
        for command, who, fields in cases:
            with self.subTest(command=command, who=who, fields=fields):
                request = {'GID': (INT, game.gid)} | fields
                with self.assertRaises(self.gm.GameError):
                    self.call(command, who, **request)
                self.assertEqual(game.setup(game.host), before)
                self.assertFalse(game.links)
                self.assertFalse(game.finalized)

    def test_states_timestamps_and_mesh_completion_are_consistent_and_once_only(self):
        game = self.room()
        self.assertEqual(game.host.state, self.gm.ACTIVE_CONNECTED)
        self.assertGreater(game.host.joined_at, 0)
        self.assertEqual(game.players[1].joined_at, 0)
        self.call(self.gm.ADVANCE_GAME_STATE, GID=(INT, game.gid), GSTA=(INT, self.gm.PRE_GAME))
        self.call(self.gm.FINALIZE_GAME_CREATION, GID=(INT, game.gid))
        with self.assertRaises(self.gm.GameError):
            self.call(self.gm.ADVANCE_GAME_STATE, GID=(INT, game.gid), GSTA=(INT, self.gm.IN_GAME))
        notes = self.connect(game)
        done = self.bodies(notes, self.gm.NOTIFY_PLAYER_JOIN_COMPLETED)
        self.assertEqual(len(done), 2)
        self.assertGreater(done[0]['TIME'][1], 0)
        self.assertEqual(done[0]['TIME'], game.players[1].replicated(game)['TIME'])
        self.assertEqual(self.connect(game), [])
        self.assertEqual(self.call(self.gm.FINALIZE_GAME_CREATION, GID=(INT, game.gid)), (b'', []))
        for state in (self.gm.IN_GAME, self.gm.POST_GAME, self.gm.PRE_GAME):
            self.call(self.gm.ADVANCE_GAME_STATE, GID=(INT, game.gid), GSTA=(INT, state))
            self.assertEqual(game.state, state)

    def test_no_duplicate_full_or_unknown_game_join_and_failed_join_preserves_membership(self):
        game = self.room()
        for who, gid in ((1, game.gid), (2, game.gid), (1, 9999)):
            with self.subTest(who=who, gid=gid), self.assertRaises(self.gm.GameError):
                self.call(self.gm.JOIN_GAME, who, GID=(INT, gid))
        self.assertEqual(len(game.players), 2)
        with self.assertRaises(self.gm.GameError):
            self.call(self.gm.CREATE_GAME, GMCD=(STRUCT, Struct(PMAX=(INT, 3))))
        self.assertIs(self.gm.game_of(self.accounts[0].id), game)

    def test_direct_join_cancels_waiting_search_only_after_join_is_validated(self):
        msid, _ = self.search(1)
        game = self.room(False)
        with self.assertRaises(self.gm.GameError):
            self.call(self.gm.JOIN_GAME, 1, GID=(INT, 9999))
        self.assertEqual(self.gm.SEARCHES[0].msid, msid)
        _, notes = self.call(self.gm.JOIN_GAME, 1, GID=(INT, game.gid))
        self.assertFalse(self.gm.SEARCHES)
        self.assertEqual(len(game.players), 2)
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_FAILED)[0]['MSID'], (INT, msid))
        self.search(2)
        self.assertEqual(len(self.gm.GAMES), 1)
        self.assertEqual(len(self.gm.SEARCHES), 1)

    def test_versions_are_isolated_and_unknown_scenario_rejected(self):
        self.search(0, version='1.14')
        self.search(1, version='1.15')
        self.assertFalse(self.gm.GAMES)
        self.assertEqual(len(self.gm.SEARCHES), 2)
        with self.assertRaises(self.gm.GameError):
            self.search(2, scenario='ranked')
        self.assertEqual(len(self.gm.SEARCHES), 2)
        self.search(2, version='1.14')
        self.assertEqual(len(self.gm.GAMES), 1)
        self.assertEqual(len(self.gm.SEARCHES), 1)

    def test_malformed_unknown_duplicate_and_conflicting_criteria_fail_closed(self):
        from blaze_tdf import MAP, GENERIC
        generic = lambda values: ('generic', 2806890242, (STRING, values))
        bad = [ (INT, 0), (MAP, (STRING, STRING, [])),
                (MAP, (STRING, GENERIC, [('UNKNOWN', generic(['0']))])),
                (MAP, (STRING, GENERIC, [('UFC_WEIGHT_CLASS', None)])),
                (MAP, (STRING, GENERIC, [('UFC_WEIGHT_CLASS', generic(['abstain', '0']))])),
                (MAP, (STRING, GENERIC, [('UFC_WEIGHT_CLASS', generic(['0'])),
                                       ('UFC_WEIGHT_CLASS', generic(['1']))])) ]
        game = self.room(False)
        for scna in bad:
            with self.subTest(scna=scna), self.assertRaises(self.gm.GameError):
                self.search(0, SCNA=scna)
            self.assertIs(self.gm.game_of(self.accounts[0].id), game)
            self.assertFalse(self.gm.SEARCHES)

    def test_cancel_is_owned_by_msid_and_account_and_does_not_cancel_replacement(self):
        first, _ = self.search(0)
        for who, msid in ((1, first), (0, first + 100)):
            with self.assertRaises(self.gm.GameError):
                self.call(self.gm.CANCEL_MATCHMAKING_SCENARIO, who, MSID=(INT, msid))
        _, notes = self.call(self.gm.CANCEL_MATCHMAKING_SCENARIO, MSID=(INT, first))
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_FAILED)[0]['RSLT'], (INT, 4))
        second, _ = self.search(0)
        with self.assertRaises(self.gm.GameError):
            self.call(self.gm.CANCEL_MATCHMAKING_SCENARIO, MSID=(INT, first))
        self.assertEqual(self.gm.SEARCHES[0].msid, second)

    def test_cancel_pending_match_cleans_both_but_validated_match_is_finished(self):
        for who in (0, 1):
            first, _ = self.search(0)
            second, _ = self.search(1)
            _, notes = self.call(self.gm.CANCEL_MATCHMAKING_SCENARIO, who, MSID=(INT, (first, second)[who]))
            self.assertFalse(self.gm.GAMES)
            self.assertEqual(len(self.bodies(notes, self.gm.NOTIFY_GAME_REMOVED)), 2)
            failures = self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_FAILED)
            self.assertEqual(sorted(f['RSLT'][1] for f in failures), [4, 5])
        first, _ = self.search(0)
        self.search(1)
        game = next(iter(self.gm.GAMES.values()))
        notes = self.connect(game)
        self.assertEqual(len(self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_CONNECTION_VALIDATED)), 2)
        self.assertEqual([p.search for p in game.players], [0, 0])
        with self.assertRaises(self.gm.GameError):
            self.call(self.gm.CANCEL_MATCHMAKING_SCENARIO, MSID=(INT, first))
        self.assertIn(game.gid, self.gm.GAMES)

    def test_idle_search_timeout_and_closed_writer_cleanup(self):
        with patch('ufc3_games.time.monotonic', return_value=100):
            msid, _ = self.search(0)
            self.search(1, version='other')
        self.writers[1].closed = True
        notes = self.gm.expire(lambda *_: None, now=161)
        self.assertFalse(self.gm.SEARCHES)
        self.assertEqual(len(notes), 1)
        failure = self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_FAILED)[0]
        self.assertEqual(failure['MSID'], (INT, msid))
        self.assertEqual(failure['RSLT'], (INT, 3))
        self.assertEqual(self.gm.expire(lambda *_: None, now=200), [])

    def test_matched_and_direct_join_timeouts(self):
        self.search(0)
        self.search(1)
        game = next(iter(self.gm.GAMES.values()))
        notes = self.gm.expire(lambda *_: None, now=max(p.deadline for p in game.players) + 1)
        self.assertFalse(self.gm.GAMES)
        self.assertEqual(len(self.bodies(notes, self.gm.NOTIFY_MATCHMAKING_FAILED)), 2)
        game = self.room()
        notes = self.gm.expire(lambda *_: None, now=game.players[1].deadline + 1)
        self.assertEqual(game.players, [game.host])
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_PLAYER_REMOVED)[0]['REAS'], (INT, 0))

    def test_real_wire_criteria_decode_completely_and_match(self):
        from blaze_tdf import MAP, GENERIC, encode_struct, encode_tag, encode_int
        request = Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14'))),
                         SCNM=(STRING, 'ufcH2HUnranked'))
        # UFC's untagged GenericTdfType contains a list<string> (tdf id 2806890242).
        values = ['2', '5']
        raw = encode_struct(request)
        raw += encode_tag('SCNA') + bytes([MAP, STRING, GENERIC]) + encode_int(1)
        name = b'UFC_WEIGHT_CLASS\0'
        raw += encode_int(len(name)) + name + b'\1' + encode_int(2806890242)
        raw += bytes([STRING]) + encode_int(len(values))
        for value in values:
            raw += encode_int(2) + value.encode() + b'\0'
        raw += b'\0'
        decoded, consumed = decode_struct(raw)
        self.assertEqual(consumed, len(raw))
        self.assertEqual(self.gm.scenario_criteria(decoded), {'UFC_WEIGHT_CLASS': values})
        for who in (0, 1):
            self.gm.handle(self.gm.START_MATCHMAKING_SCENARIO, decoded, self.accounts[who],
                           self.writers[who], '192.168.1.10', lambda *_: None)
        game = next(iter(self.gm.GAMES.values()))
        self.assertIn(dict(game.attributes)['WeightClass'], values)

    def test_replay_resets_postgame_to_pregame_with_new_reporting_id(self):
        game = self.room()
        self.connect(game)
        for state in (self.gm.PRE_GAME, self.gm.IN_GAME, self.gm.POST_GAME):
            if state == self.gm.IN_GAME:
                self.call(self.gm.FINALIZE_GAME_CREATION, GID=(INT, game.gid))
            self.call(self.gm.ADVANCE_GAME_STATE, GID=(INT, game.gid), GSTA=(INT, state))
        players, attributes = list(game.players), list(game.attributes)
        with self.assertRaises(self.gm.GameError):  # only the host replays
            self.call(self.gm.REPLAY_GAME, 1, GID=(INT, game.gid))
        self.assertEqual(game.state, self.gm.POST_GAME)
        body, notes = self.call(self.gm.REPLAY_GAME, GID=(INT, game.gid))
        self.assertEqual(body, b'')
        self.assertEqual(game.state, self.gm.PRE_GAME)
        self.assertEqual(game.players, players)  # same participants, slots and attributes
        self.assertEqual(game.attributes, attributes)
        self.assertNotEqual(game.gsid, game.gid)
        states = self.bodies(notes, self.gm.NOTIFY_GAME_STATE_CHANGE)
        self.assertEqual([s['GSTA'] for s in states], [(INT, self.gm.PRE_GAME)] * 2)
        reporting = self.bodies(notes, self.gm.NOTIFY_GAME_REPORTING_ID_CHANGE)
        self.assertEqual([r['GRID'] for r in reporting], [(INT, game.gsid)] * 2)
        self.assertEqual(len(notes), 4)  # both notifications reach both players
        # A duplicate replay is a no-op; the reused room starts the next fight without re-finalizing.
        self.assertEqual(self.call(self.gm.REPLAY_GAME, GID=(INT, game.gid)), (b'', []))
        self.call(self.gm.ADVANCE_GAME_STATE, GID=(INT, game.gid), GSTA=(INT, self.gm.IN_GAME))
        with self.assertRaises(self.gm.GameError):  # not from a fight in progress
            self.call(self.gm.REPLAY_GAME, GID=(INT, game.gid))

    def test_host_settings_and_attributes_keep_ufc_notification_shapes(self):
        from blaze_tdf import MAP
        game = self.room()
        _, notes = self.call(self.gm.SET_GAME_SETTINGS, GID=(INT, game.gid), GSET=(INT, 42))
        self.assertEqual(game.gset, 42)
        self.assertEqual(self.bodies(notes, self.gm.NOTIFY_GAME_SETTINGS_CHANGE)[0]['ATTR'], (INT, 42))
        self.call(self.gm.SET_GAME_ATTRIBUTES, GID=(INT, game.gid),
                  ATTR=(MAP, (STRING, STRING, [('WeightClass', '5')])))
        self.call(self.gm.SET_GAME_ATTRIBUTES, GID=(INT, game.gid),
                  ATTR=(MAP, (STRING, STRING, [('ClockType', '0')])))
        self.assertEqual(dict(game.attributes), {'WeightClass': '5', 'ClockType': '0'})

    def test_truncated_nested_tdf_duplicate_tags_and_bad_lengths_are_rejected(self):
        from blaze_tdf import encode_struct, encode_tag, encode_int, LIST, MAP
        struct = encode_struct(Struct(CMGD=(STRUCT, Struct(GVER=(STRING, '1.14')))))
        string = encode_struct(Struct(SCNM=(STRING, 'ufcH2HUnranked')))
        invalid = [struct[:-1], string[:-1], string + string,
                   encode_tag('SCNA') + bytes([MAP, STRING, INT]) + encode_int(-1),
                   encode_tag('PROS') + bytes([LIST, INT]) + encode_int(-1)]
        for raw in invalid:
            with self.subTest(raw=raw), self.assertRaises((ValueError, IndexError)):
                decode_struct(raw)

    def test_legacy_matchmaking_is_explicitly_rejected(self):
        with self.assertRaises(self.gm.GameError) as error:
            self.call(self.gm.START_MATCHMAKING)
        self.assertEqual(error.exception.code, self.gm.ERR_COMMAND_NOT_FOUND)
        self.assertFalse(self.gm.GAMES)
        self.assertFalse(self.gm.SEARCHES)


class MaintenanceTests(unittest.IsolatedAsyncioTestCase):
    async def test_maintenance_expires_without_client_requests(self):
        tick = asyncio.Event()
        def expire(log):
            tick.set()
            return []
        with patch.object(ufc3_server.ufc3_games, 'expire', side_effect=expire):
            worker = asyncio.create_task(ufc3_server.game_maintenance())
            try:
                await asyncio.wait_for(tick.wait(), 2)
            finally:
                worker.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await worker


if __name__ == '__main__':
    unittest.main()
