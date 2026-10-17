# SPDX-License-Identifier: GPL-2.0-or-later
"""GameManager (Blaze component 4) for a two-player LAN fight: one game, its host and a guest.

The fight itself is the game's own DirtySDK peer-to-peer traffic between the two PCs; the
server only creates the game, tells each player the other's address and relays state changes.

UFC's reflected TDF classes and command/notification switches define the wire contract.
Only ufcH2HUnranked scenario matchmaking is supported. Searches wait at most 60 seconds;
peer incorporation waits at most 30 seconds. With no host migration, host departure or an
incomplete matched pair closes the room for both clients.
"""
import itertools
import socket
import time
import uuid

from blaze_tdf import INT, STRING, STRUCT, LIST, MAP, UNION, BLOB, OBJID, GENERIC, NOTIFICATION, Frame, Struct, Union

GAME_MANAGER = 4
USER_SESSIONS = 0x7802

# Commands (client -> server).
CREATE_GAME, DESTROY_GAME, ADVANCE_GAME_STATE, SET_GAME_SETTINGS = 0x1, 0x2, 0x3, 0x4
SET_GAME_ATTRIBUTES, JOIN_GAME, REMOVE_PLAYER = 0x7, 0x9, 0xB
START_MATCHMAKING, CANCEL_MATCHMAKING, FINALIZE_GAME_CREATION = 0xD, 0xE, 0xF
START_MATCHMAKING_SCENARIO, CANCEL_MATCHMAKING_SCENARIO, LEAVE_GAME_BY_GROUP = 0x10, 0x11, 0x16
REPLAY_GAME = 0x13  # replayGame in UFC's command-name switch (table 0x1f2ae38, id 19)
MESH_ENDPOINTS_CONNECTED, MESH_ENDPOINTS_DISCONNECTED, MESH_ENDPOINTS_CONNECTION_LOST = 0x41, 0x42, 0x43
# (UFC's own command table: the eboot's GameManager command-name switch.)

# Notifications (server -> client).
# (UFC's notification-name switch: 0xB is NotifyMatchmakingSessionConnectionValidated; Blaze 15
# has no client NotifyMatchmakingFinished, NotifyGameSetup with a MatchmakingSetupContext is it.)
NOTIFY_MATCHMAKING_FAILED, NOTIFY_MATCHMAKING_CONNECTION_VALIDATED = 0xA, 0xB
NOTIFY_GAME_SETUP, NOTIFY_PLAYER_JOINING, NOTIFY_PLAYER_JOIN_COMPLETED = 0x14, 0x15, 0x1E
NOTIFY_PLAYER_REMOVED, NOTIFY_PLATFORM_HOST_INITIALIZED = 0x28, 0x47
NOTIFY_GAME_ATTRIB_CHANGE, NOTIFY_GAME_STATE_CHANGE, NOTIFY_GAME_SETTINGS_CHANGE = 0x50, 0x64, 0x6E
NOTIFY_GAME_REPORTING_ID_CHANGE = 0x71  # UFC's notification-name switch, id 113
NOTIFY_GAME_PLAYER_STATE_CHANGE = 0x74
NOTIFY_GAME_REMOVED = 0x10

# UFC live enum descriptors: PlayerRemovedReason 0x2ab61f8,
# GameDestructionReason 0x2ab5de8, MatchmakingResult 0x2ab7518 (guest-live.bin).
# The older Blaze3 values differ. Fire2Metadata reflects ERRC:int32.
# NotifyMatchmakingFailed (client notification 0x0a) carries matchmaking IDs/result.
PLAYER_JOIN_TIMEOUT, PLAYER_CONN_LOST, BLAZESERVER_CONN_LOST = 0, 2, 3
GAME_DESTROYED, PLAYER_LEFT, GROUP_LEFT, PLAYER_KICKED = 5, 7, 8, 9
SYS_GAME_ENDING, SYS_CONNECTION_FAILED, HOST_LEAVING = 0, 3, 4
SESSION_TIMED_OUT, SESSION_CANCELED, SESSION_TERMINATED = 3, 4, 5
SEARCH_TIMEOUT = 60.0
JOIN_TIMEOUT = 30.0

# UFC's error-name switch: 0xf828a4 -> ERR_SYSTEM; 0xf82ae4 -> ERR_COMMAND_NOT_FOUND.
ERR_SYSTEM, ERR_COMMAND_NOT_FOUND = 0x40010000, 0x40030000


UPDATE_PRIMARY_EXTERNAL_SESSION = 0xB1

# GameState.
INITIALIZING, PRE_GAME, IN_GAME, POST_GAME = 1, 0x82, 0x83, 4
# PlayerState.
RESERVED, QUEUED, ACTIVE_CONNECTING, ACTIVE_MIGRATING, ACTIVE_CONNECTED = 0, 1, 2, 3, 4

_game_ids = itertools.count(1)
_scenario_ids = itertools.count(1)
# Reporting ids handed out on replay: outside the game-id space the first fight uses.
_reporting_ids = itertools.count(1 << 32)

class GameError(ValueError):
    def __init__(self, message, code=ERR_SYSTEM):
        super().__init__(message)
        self.code = code


def field(request, tag, kind, default=None):
    value = request.get(tag)
    if value is None and default is not None:
        return default
    if not isinstance(value, tuple) or len(value) != 2 or value[0] != kind:
        raise GameError(f'invalid or missing {tag}')
    return value[1]


def string_map(request, tag, default=None):
    attr = field(request, tag, MAP, default)
    if (not isinstance(attr, tuple) or len(attr) != 3 or attr[:2] != (STRING, STRING)
            or any(not isinstance(k, str) or not isinstance(v, str) for k, v in attr[2])):
        raise GameError(f'{tag} must be a string map')
    return list(attr[2])


def live(writer):
    return not getattr(writer, 'is_closing', lambda: False)()


def notice(players, command, body):
    from blaze_tdf import encode_struct
    frame = Frame(GAME_MANAGER, command, 0, NOTIFICATION, encode_struct(body))
    return [(p.writer, frame) for p in players if live(p.writer)]


def failed(player, result, msid=None):
    return notice([player], NOTIFY_MATCHMAKING_FAILED, Struct(
        MAXF=(INT, 100), MSCD=(INT, msid or player.search),
        MSID=(INT, msid or player.search), RSLT=(INT, result), USID=(INT, player.account.id)))

def ip_int(text: str) -> int:
    return int.from_bytes(socket.inet_aton(text), 'big')


def lan_address() -> str:
    """This PC's LAN address (the server and the Linux player share it)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(('192.168.1.1', 9))  # sends nothing
            return s.getsockname()[0]
        except OSError:
            return '127.0.0.1'


def reachable_pnet(pnet, peer_ip: str):
    """The player's network address as the other PC must use it.

    The game reports the address netctl gave it. A player on the server's PC reports loopback,
    which the other PC cannot reach; use the server's LAN address for it instead.
    """
    if not isinstance(pnet, Union) or pnet.member == 0x7f or not isinstance(pnet.value, dict):
        return pnet
    pair = Struct(pnet.value)
    seen = lan_address() if peer_ip.startswith('127.') else peer_ip
    for tag in ('INIP', 'EXIP'):
        kind, addr = pair.get(tag, (STRUCT, Struct()))
        addr = Struct(addr)
        port = addr.get('PORT', (INT, 0))[1]
        ip = addr.get('IP', (INT, 0))[1]
        if not ip or (ip >> 24) == 127:
            ip = ip_int(seen)
        if not port:
            port = Struct(pair.get('INIP', (STRUCT, Struct()))[1]).get('PORT', (INT, 0))[1]
        addr['IP'] = (INT, ip)
        addr['PORT'] = (INT, port)
        addr.setdefault('MACI', (INT, 0))
        pair[tag] = (STRUCT, addr)
    return Union(pnet.member, pnet.tag, pnet.type, pair)


class Player:
    def __init__(self, account, writer, pnet, slot):
        self.account, self.writer, self.pnet, self.slot = account, writer, pnet, slot
        self.state = ACTIVE_CONNECTING
        self.matched = False  # joinedViaMatchmaking
        self.search = 0  # nonzero only until validation/cancellation
        self.deadline = time.monotonic() + JOIN_TIMEOUT
        self.joined_at = 0

    def replicated(self, game) -> Struct:
        """ReplicatedGamePlayer."""
        a = self.account
        return Struct(
            BLOB=(BLOB, b''), CONG=(INT, a.id), CSID=(INT, self.slot), DSUI=(INT, 0),
            EXBL=(BLOB, b''), EXID=(INT, getattr(a, 'psn_id', 0) or 0),
            GID=(INT, game.gid), JFPS=(INT, 0), JVMM=(INT, int(self.matched)), LOC=(INT, 1701729619), NAME=(STRING, a.name),
            NASP=(STRING, 'cem_ea_id'), PATT=(MAP, (STRING, STRING, [])), PID=(INT, a.id),
            PNET=(UNION, self.pnet), SID=(INT, self.slot), SLOT=(INT, 0), STAT=(INT, self.state),
            TIDX=(INT, 0), TIME=(INT, self.joined_at), UID=(INT, a.id))


class Game:
    def __init__(self, request: Struct, host: Player):
        self.gid = next(_game_ids)
        self.uuid = str(uuid.uuid4())
        self.request = request
        settings = Struct(field(request, 'GMCD', STRUCT, Struct()))
        self.settings = settings
        self.attributes = string_map(settings, 'ATTR', (STRING, STRING, []))
        self.gset = field(settings, 'GSET', INT, 0)
        self.capacity = field(settings, 'PMAX', INT, 2)
        if self.capacity != 2:
            raise GameError('only two-player games are supported')
        self.topology = field(settings, 'NTOP', INT, 130)
        self.presence = field(settings, 'PRES', INT, 1)
        self.voip = field(settings, 'VOIP', INT, 0)
        self.gtype = field(request, 'GTYP', STRING, '')
        common = Struct(field(request, 'CMGD', STRUCT, Struct()))
        self.version = field(common, 'GVER', STRING, '')
        self.state = INITIALIZING
        self.finalized = False
        self.gsid = self.gid  # gameReportingId; replayGame rotates it for the next fight
        host.state = ACTIVE_CONNECTED
        host.joined_at = int(time.time() * 1_000_000)
        self.host = host
        self.players = [host]
        self.links = set()  # (reporting player id, connected player id) from meshEndpointsConnected

    def data(self) -> Struct:
        """ReplicatedGameData.

        Only members of UFC's ReplicatedGameData: GVER, HSES, IGNO and TCAP belong to other
        classes or older schemas (CommonGameRequestData, HostInfo) and are not sent.
        """
        # HostInfo (UFC reflection): CONG connection group, CSID connection slot, HPID, HSES, HSLT.
        h = self.host.account.id
        host = Struct(CONG=(INT, h), CSID=(INT, self.host.slot), HPID=(INT, h), HSES=(INT, h),
                      HSLT=(INT, self.host.slot))
        pcap = [self.capacity, 0, 0, 0]
        return Struct(
            ADMN=(LIST, (INT, [self.host.account.id])),
            ATTR=(MAP, (STRING, STRING, self.attributes)),
            CAP=(LIST, (INT, pcap)), CRIT=(MAP, (STRING, STRING, [])),
            DSTR=(MAP, (STRING, STRING, [])), ESNM=(STRING, ''),
            GID=(INT, self.gid), GMRG=(INT, 0), GNAM=(STRING, self.host.account.name),
            GPVH=(INT, 0x1A2B3C4D), GSET=(INT, self.gset), GSID=(INT, self.gsid),
            GSTA=(INT, self.state), GTYP=(STRING, self.gtype), GURL=(STRING, ''),
            HNET=(LIST, (UNION, [self.host.pnet])), MCAP=(INT, self.capacity),
            NQOS=(STRUCT, Struct(DBPS=(INT, 0), NATT=(INT, 0), UBPS=(INT, 0))),
            NRES=(INT, 0), NTOP=(INT, self.topology), PGID=(STRING, self.uuid),
            PGSR=(BLOB, b''), PHST=(STRUCT, host), PRES=(INT, self.presence),
            PSAS=(STRING, 'local'), QCAP=(INT, 0), SEED=(INT, self.gid * 7919),
            THST=(STRUCT, host),
            TIDS=(LIST, (INT, [65534])), UUID=(STRING, self.uuid),
            VOIP=(INT, self.voip), VSTR=(STRING, self.version))

    def setup(self, player: Player, context: int = 0, reason=None) -> bytes:
        """NotifyGameSetup for `player`: the game, its roster and why the player is in it.

        GameSetupReason member 0 (DatalessSetupContext): DCTX 0 = created, 1 = joined.
        """
        from blaze_tdf import encode_struct
        # GameSetupReason: each member has its own tag (DLSC 0, IJGS 1, IMSC 2, MMSC 3, RDSC 4);
        # a member under another tag does not decode and the client gets an empty context.
        reason = reason or Union(0, 'DLSC', STRUCT, Struct(DCTX=(INT, context)))
        return encode_struct(Struct(
            GAME=(STRUCT, self.data()),
            PROS=(LIST, (STRUCT, [p.replicated(self) for p in self.players])),
            MNAM=(STRING, 'GameMode'), QOSV=(INT, 0), REAS=(UNION, reason),
            TELM=(INT, 30_000_000)))  # qosTelemetryInterval (µs); unset, clients report every frame

    def find(self, account_id):
        return next((p for p in self.players if p.account.id == account_id), None)


GAMES = {}  # gid -> Game


def game_of(account_id):
    return next((g for g in GAMES.values() if g.find(account_id)), None)


def destroy(game, log, reason=HOST_LEAVING, result=SESSION_TERMINATED):
    notes = []
    for player in game.players:
        if player.search:
            notes += failed(player, result)
            player.search = 0
    notes += notice(game.players, NOTIFY_GAME_REMOVED,
                    Struct(GID=(INT, game.gid), REAS=(INT, reason)))
    GAMES.pop(game.gid, None)
    game.links.clear()
    game.players.clear()
    log(f'game {game.gid} destroyed (reason {reason})')
    return notes


def remove(game, player, log, reason=PLAYER_LEFT, context=0):
    notes = notice(game.players, NOTIFY_PLAYER_REMOVED, Struct(
        CNTX=(INT, context), GID=(INT, game.gid), PID=(INT, player.account.id), REAS=(INT, reason)))
    if player is game.host or any(p.search for p in game.players):
        return notes + destroy(game, log)
    game.players.remove(player)
    game.links = {link for link in game.links if player.account.id not in link}
    return notes


def leave(account_id, log, reason=BLAZESERVER_CONN_LOST, writer=None):
    """Transport cleanup; an old socket must not remove a replacement session."""
    notes = drop_search(account_id, log, writer=writer)
    game = game_of(account_id)
    if not game:
        return notes
    player = game.find(account_id)
    if writer is not None and player.writer is not writer:
        return notes
    return notes + remove(game, player, log, reason)


def expire(log, now=None):
    """Bound waiting searches and incomplete joins, even when no new requests arrive."""
    now = time.monotonic() if now is None else now
    notes = []
    for search in list(SEARCHES):
        if not live(search.writer) or now >= search.deadline:
            SEARCHES.remove(search)
            if live(search.writer):
                notes += failed(search, SESSION_TIMED_OUT, search.msid)
    for game in list(GAMES.values()):
        dead = next((p for p in game.players if not live(p.writer)), None)
        if dead:
            notes += remove(game, dead, log, BLAZESERVER_CONN_LOST)
        elif any((p.search or p.state == ACTIVE_CONNECTING) and now >= p.deadline
                 for p in game.players):
            if any(p.search for p in game.players):
                notes += destroy(game, log, reason=SYS_CONNECTION_FAILED, result=SESSION_TIMED_OUT)
            else:
                for p in list(game.players):
                    if p.state == ACTIVE_CONNECTING and now >= p.deadline:
                        notes += remove(game, p, log, PLAYER_JOIN_TIMEOUT)
    return notes


def cancel(request, account, writer, log):
    msid = field(request, 'MSID', INT)
    for search in SEARCHES:
        if search.msid == msid and search.account.id == account.id and search.writer is writer:
            SEARCHES.remove(search)
            return failed(search, SESSION_CANCELED, msid)
    game = game_of(account.id)
    player = game.find(account.id) if game else None
    if player and player.writer is writer and player.search == msid and msid:
        notes = failed(player, SESSION_CANCELED)
        player.search = 0
        return notes + remove(game, player, log)
    raise GameError('unknown matchmaking session for this account')


def handle(command, request, account, writer, peer_ip, log):
    """Return (reply, notifications), None for an unknown command, or raise GameError.

    Validate before mutation. The transport returns failures via Fire2Metadata.ERRC.
    """
    from blaze_tdf import encode_struct
    request = request or Struct()
    notes = []
    if command == CREATE_GAME:
        common = Struct(field(request, 'CMGD', STRUCT, Struct()))
        pnet = reachable_pnet(field(common, 'PNET', UNION, Union()), peer_ip)
        host = Player(account, writer, pnet, 0)
        game = Game(request, host)
        notes += leave(account.id, log, PLAYER_LEFT)
        GAMES[game.gid] = game
        log(f'game {game.gid} created by {account.name} ({game.gtype}, {game.capacity} seats, '
            f'attributes {dict(game.attributes)}, address {describe_pnet(pnet)})')
        notes.append((writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION, game.setup(host, 0))))
        return encode_struct(Struct(GID=(INT, game.gid))), notes  # CreateGameResponse

    if command == START_MATCHMAKING_SCENARIO:
        return matchmake(request, account, writer, peer_ip, log)

    if command in (CANCEL_MATCHMAKING_SCENARIO, CANCEL_MATCHMAKING):
        return b'', cancel(request, account, writer, log)

    if command == START_MATCHMAKING:
        raise GameError('legacy matchmaking is not supported', ERR_COMMAND_NOT_FOUND)

    if command == JOIN_GAME:
        game = GAMES.get(field(request, 'GID', INT))
        if not game:
            raise GameError('unknown game')
        if game_of(account.id):
            raise GameError('player is already in a game')
        if len(game.players) >= game.capacity:
            raise GameError('game is full')
        if game.state not in (INITIALIZING, PRE_GAME):
            raise GameError('game is not accepting joins')
        common = Struct(field(request, 'CMGD', STRUCT, Struct()))
        pnet = reachable_pnet(field(common, 'PNET', UNION, field(request, 'PNET', UNION, Union())), peer_ip)
        slot = next(i for i in range(game.capacity) if all(p.slot != i for p in game.players))
        player = Player(account, writer, pnet, slot)
        notes += drop_search(account.id, log)
        game.players.append(player)
        log(f'{account.name} joins game {game.gid} of {game.host.account.name} '
            f'(address {describe_pnet(pnet)})')
        # Each side must know the other user (UserSessions UserAdded + UserUpdated, with the
        # network address) before the roster names them; BF4's join flow does the same.
        from ufc3_auth import user_added, user_updated
        for other in game.players:
            if other is not player:
                for to, who in ((other, player), (player, other)):
                    notes.append((to.writer, Frame(USER_SESSIONS, 2, 0, NOTIFICATION, user_added(who.account, who.pnet))))
                    notes.append((to.writer, Frame(USER_SESSIONS, 5, 0, NOTIFICATION, user_updated(who.account))))
        joining = encode_struct(Struct(GID=(INT, game.gid), PDAT=(STRUCT, player.replicated(game))))
        for other in game.players:
            if other is not player:
                notes.append((other.writer, Frame(GAME_MANAGER, NOTIFY_PLAYER_JOINING, 0, NOTIFICATION, joining)))
        notes.append((writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION, game.setup(player, 1))))
        return encode_struct(Struct(GID=(INT, game.gid), JGS=(INT, 0))), notes

    supported = {ADVANCE_GAME_STATE, FINALIZE_GAME_CREATION, SET_GAME_ATTRIBUTES, SET_GAME_SETTINGS,
                 MESH_ENDPOINTS_CONNECTED, MESH_ENDPOINTS_DISCONNECTED, MESH_ENDPOINTS_CONNECTION_LOST,
                 LEAVE_GAME_BY_GROUP, DESTROY_GAME, REMOVE_PLAYER, UPDATE_PRIMARY_EXTERNAL_SESSION,
                 REPLAY_GAME}
    if command not in supported:
        return None
    # updatePrimaryExternalSessionForUser uses CGID, not GID, in UFC.
    game = GAMES.get(field(request, 'CGID' if command == UPDATE_PRIMARY_EXTERNAL_SESSION else 'GID', INT))
    if not game or not game.find(account.id):
        raise GameError('unknown game or nonmember')
    if game.find(account.id).writer is not writer:
        raise GameError('request from an obsolete connection')
    if command in {ADVANCE_GAME_STATE, FINALIZE_GAME_CREATION, SET_GAME_ATTRIBUTES,
                   SET_GAME_SETTINGS, DESTROY_GAME, REPLAY_GAME} and game.host.account.id != account.id:
        raise GameError('host authority required')

    if command == ADVANCE_GAME_STATE:
        state = field(request, 'GSTA', INT)
        transitions = {INITIALIZING: {PRE_GAME}, PRE_GAME: {IN_GAME},
                       IN_GAME: {POST_GAME}, POST_GAME: {PRE_GAME}}
        if state == game.state:
            return b'', []
        if state not in transitions.get(game.state, set()):
            raise GameError('illegal game state transition')
        if state == IN_GAME and (not game.finalized or len(game.players) != game.capacity
                                 or any(p.state != ACTIVE_CONNECTED for p in game.players)):
            raise GameError('game is not ready')
        game.state = state
        log(f'game {game.gid} state -> {game.state:#x}')
        change = encode_struct(Struct(GID=(INT, game.gid), GSTA=(INT, game.state)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_STATE_CHANGE, 0, NOTIFICATION, change)))
        return b'', notes

    if command == REPLAY_GAME:
        # replayGame: request is only GID and the reply is empty (PocketRelay's ReplayGame route).
        # The host takes a finished room back to PRE_GAME for the rematch; players, slots and
        # attributes stay, and the reporting id rotates so the next fight reports under its own id
        # (Blaze's NotifyGameReportingIdChange, in UFC's notification switch as id 0x71).
        if game.state == PRE_GAME:
            return b'', []  # duplicate/retry of a replay that already applied
        if game.state != POST_GAME:
            raise GameError('replay requires a finished game')
        game.state = PRE_GAME
        game.gsid = next(_reporting_ids)
        log(f'game {game.gid} replay requested by {account.name}: state -> {PRE_GAME:#x}, '
            f'reporting id {game.gsid}')
        change = encode_struct(Struct(GID=(INT, game.gid), GSTA=(INT, game.state)))
        reporting = encode_struct(Struct(GID=(INT, game.gid), GRID=(INT, game.gsid)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_STATE_CHANGE, 0, NOTIFICATION, change)))
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_REPORTING_ID_CHANGE, 0,
                                          NOTIFICATION, reporting)))
        return b'', notes

    if command == FINALIZE_GAME_CREATION:
        if game.finalized:
            return b'', []
        game.finalized = True
        log(f'game {game.gid} finalized by {account.name}')
        init = encode_struct(Struct(GID=(INT, game.gid), PHID=(INT, game.host.account.id), PHST=(INT, game.host.slot)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_PLATFORM_HOST_INITIALIZED, 0, NOTIFICATION, init)))
        return b'', notes

    if command == SET_GAME_ATTRIBUTES:
        changed = dict(string_map(request, 'ATTR'))
        merged = dict(game.attributes); merged.update(changed)
        game.attributes = sorted(merged.items())
        body = encode_struct(Struct(ATTR=(MAP, (STRING, STRING, sorted(changed.items()))), GID=(INT, game.gid)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_ATTRIB_CHANGE, 0, NOTIFICATION, body)))
        return b'', notes

    if command == SET_GAME_SETTINGS:
        game.gset = field(request, 'GSET', INT)
        body = encode_struct(Struct(ATTR=(INT, game.gset), GID=(INT, game.gid)))
        for p in game.players:
            notes.append((p.writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETTINGS_CHANGE, 0, NOTIFICATION, body)))
        return b'', notes

    if command == MESH_ENDPOINTS_CONNECTED:
        # A member reports its peer connection (TCG: the other end's connection group). Once both
        # ends of a pair report it, the joining player is in: ACTIVE_CONNECTED + join completed.
        target = mesh_target(request, game)
        other = target[2]
        log(f'mesh: {account.name} connected to connection group {target}')
        if other and other != account.id:
            game.links.add((account.id, other))
        for p in game.players:
            if p is game.host or p.state == ACTIVE_CONNECTED:
                continue
            h = game.host.account.id
            if (p.account.id, h) in game.links and (h, p.account.id) in game.links:
                p.state = ACTIVE_CONNECTED
                p.joined_at = int(time.time() * 1_000_000)
                log(f'game {game.gid}: {p.account.name} joined (peer connection up both ways)')
                for who in (game.host, p):
                    state = encode_struct(Struct(GID=(INT, game.gid), PID=(INT, who.account.id),
                                                 STAT=(INT, ACTIVE_CONNECTED)))
                    for q in game.players:
                        notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_GAME_PLAYER_STATE_CHANGE, 0,
                                                      NOTIFICATION, state)))
                done = encode_struct(Struct(GID=(INT, game.gid), PID=(INT, p.account.id), TIME=(INT, p.joined_at)))
                for q in game.players:
                    notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_PLAYER_JOIN_COMPLETED, 0, NOTIFICATION, done)))
                # Matchmaking ends for its members once their connections are validated; until
                # then the client keeps its scenario open and the lobby does not start.
                for q in game.players:
                    if not q.search:
                        continue
                    results = Struct(ALST=(LIST, (INT, [])), FCNT=(INT, 0), GIDL=(LIST, (INT, [])),
                                     NTOP=(INT, game.topology), TIER=(INT, 0))
                    valid = encode_struct(Struct(CONV=(STRUCT, results), DISP=(INT, 1), GID=(INT, game.gid),
                                                 MSCD=(INT, q.search), MSID=(INT, q.search), QSVR=(INT, 0),
                                                 USID=(INT, q.account.id)))
                    notes.append((q.writer, Frame(GAME_MANAGER, NOTIFY_MATCHMAKING_CONNECTION_VALIDATED, 0,
                                                  NOTIFICATION, valid)))
                    log(f'matchmaking {q.search}: {q.account.name} connection validated, search finished')
                    q.search = 0
        return b'', notes

    if command == UPDATE_PRIMARY_EXTERNAL_SESSION:
        return encode_struct(Struct(GID=(INT, game.gid), USEI=(INT, account.id))), notes

    if command in (MESH_ENDPOINTS_DISCONNECTED, MESH_ENDPOINTS_CONNECTION_LOST):
        target = mesh_target(request, game)
        return b'', remove(game, game.find(target[2]), log, PLAYER_CONN_LOST)

    if command in (LEAVE_GAME_BY_GROUP, REMOVE_PLAYER):
        pid = field(request, 'PID', INT)
        player = game.find(pid)
        if not player or (pid != account.id and game.host.account.id != account.id):
            raise GameError('invalid removal target or authority')
        if command == LEAVE_GAME_BY_GROUP and 'BTPL' in request:
            group = field(request, 'BTPL', OBJID)
            if group != (USER_SESSIONS, 2, pid):
                raise GameError('invalid leave group')
        default_reason = GROUP_LEFT if command == LEAVE_GAME_BY_GROUP else (PLAYER_LEFT if pid == account.id else PLAYER_KICKED)
        reason = field(request, 'REAS', INT, default_reason)
        if reason not in range(24):
            raise GameError('invalid player removal reason')
        return b'', remove(game, player, log, reason, field(request, 'CNTX', INT, 0))

    if command == DESTROY_GAME:
        reason = field(request, 'REAS', INT, SYS_GAME_ENDING)
        if reason not in range(9):
            raise GameError('invalid destruction reason')
        return encode_struct(Struct(GID=(INT, game.gid))), destroy(game, log, reason)

    return None


def mesh_target(request, game):
    target = field(request, 'TCG', OBJID)
    if (not isinstance(target, tuple) or len(target) != 3 or target[:2] != (USER_SESSIONS, 2)
            or not game.find(target[2])):
        raise GameError('invalid mesh connection group')
    return target


SEARCHES = []  # players waiting in startMatchmakingScenario, oldest first

# Game settings for a Quick Fight made by matchmaking, as UFC's own Quick Fight session creates
# them (createGame from the host: gameType4, GameMode 4, peer-to-peer full mesh, 2 seats).
# Its attributes are filled per match by match_attributes().
MATCH_GAME = Struct(
    CMGD=(STRUCT, Struct(GGTY=(INT, 0), GVER=(STRING, '1.14'))),
    GMCD=(STRUCT, Struct(ATTR=(MAP, (STRING, STRING, [])),
                         GSET=(INT, 295940), NTOP=(INT, 130), PMAX=(INT, 2), PMIN=(INT, 1),
                         PRES=(INT, 1), VOIP=(INT, 0))),  # VOIP_DISABLED: no voice mesh to wait for
    GTYP=(STRING, 'gameType4'))

# Attributes of a matched game. The game reads all six when the fight is set up (0x358380:
# GameMode, MaxRounds, ClockType, CustomFighterOption, FightRules, WeightClass); a missing one
# stays at its "any" value (-1) and the pregame screen gets no weight class. The search sends a
# criterion per setting (scenario attributes UFC_*), either the values the player accepts or
# "abstain" (the UI's "any", -1; 0x381f00). Values are the UI's enums (OnlineChallengeHelper in
# inxp_ui_40.big, "values need to match onlinegamedefines.h"):
#   WeightClass   0 fly, 1 bantam, 2 feather, 3 light, 4 welter, 5 middle, 6 light heavy,
#                 7 heavy, 8 women's bantam, 9 women's straw (sent as the bits of a mask)
#   MaxRounds     3 or 5            ClockType   0 real time, 1 accelerated
#   CustomFighterOption  0 not allowed, 1 allowed
#   FightRules    no UI enum; 0 is the game's own default (0x3540a0 and the pregame model)
# A value both players accept is used (the first player's first); when both abstain, the first
# entry of the list below: MaxRounds 5 as UFC's own Quick Fight session creates it, ClockType 1
# (the game's default), no created fighters (the other PC does not have them).
MATCH_CRITERIA = (
    ('WeightClass', 'UFC_WEIGHT_CLASS', [str(i) for i in range(10)]),
    ('MaxRounds', 'UFC_MAX_ROUNDS', ['5', '3']),
    ('ClockType', 'UFC_CLOCK_TYPE', ['1', '0']),
    ('CustomFighterOption', 'UFC_CUSTOM_FIGHTER', ['0', '1']),
    ('FightRules', 'UFC_FIGHT_RULES', ['0']),
)


def scenario_criteria(request) -> dict:
    """The search's criteria: UFC_* name -> list of accepted values (None: abstain)."""
    out = {}
    if 'SCAT' in request:
        raise GameError('SCAT is not UFC scenarioAttributes (SCNA)')
    scna = field(request, 'SCNA', MAP, (STRING, GENERIC, []))
    if not isinstance(scna, tuple) or len(scna) != 3 or scna[:2] != (STRING, GENERIC):
        raise GameError('invalid scenario attributes map')
    supported = {name for _, name, _ in MATCH_CRITERIA}
    for name, value in scna[2]:
        if name not in supported or name in out:
            raise GameError('unknown or duplicate scenario criterion')
        if (not isinstance(value, tuple) or len(value) != 3 or value[:2] != ('generic', 2806890242)
                or not isinstance(value[2], tuple) or len(value[2]) != 2 or value[2][0] != STRING):
            raise GameError('criterion must be a string list')
        values = value[2][1]
        if not values or any(not isinstance(v, str) or (v != 'abstain' and not v.isdecimal()) for v in values):
            raise GameError('unsupported criterion value')
        if 'abstain' in values and len(values) != 1:
            raise GameError('abstain must be the only criterion value')
        out[name] = None if values == ['abstain'] else values
    return out


def match_attributes(first: dict, second: dict):
    """The matched game's attributes for two searches' criteria, or None if they do not overlap."""
    attributes = [('GameMode', '4')]
    for attribute, criterion, defaults in MATCH_CRITERIA:
        wanted = [c[criterion] for c in (first, second) if c.get(criterion) is not None]
        common = [v for v in (wanted[0] if wanted else defaults) if all(v in w for w in wanted)]
        if not common:
            return None
        attributes.append((attribute, common[0]))
    return sorted(attributes)


class Search:
    def __init__(self, account, writer, pnet, msid, scenario, criteria=None, version='1.14'):
        self.account, self.writer, self.pnet, self.msid, self.scenario = account, writer, pnet, msid, scenario
        self.criteria = criteria or {}
        self.version = version
        self.deadline = time.monotonic() + SEARCH_TIMEOUT


def drop_search(account_id, log, writer=None):
    notes = []
    for search in [s for s in SEARCHES if s.account.id == account_id
                   and (writer is None or s.writer is writer)]:
        SEARCHES.remove(search)
        notes += failed(search, SESSION_CANCELED, search.msid)
        log(f'matchmaking {search.msid}: {search.account.name} stopped searching')
    return notes


def matchmake(request, account, writer, peer_ip, log):
    """startMatchmakingScenario: pair two searching players in a new game.

    The first searcher waits. When a second one arrives the server creates the game with the
    first as its host (MatchmakingSetupContext RSLT 0, SUCCESS_CREATED_GAME) and the second
    joining it (RSLT 1, SUCCESS_JOINED_NEW_GAME); both get NotifyGameSetup.
    """
    from blaze_tdf import encode_struct
    from ufc3_auth import user_added, user_updated
    notes = []
    request = request or Struct()
    common = Struct(field(request, 'CMGD', STRUCT, Struct()))
    version = field(common, 'GVER', STRING)
    if not version:
        raise GameError('empty protocol version')
    pnet = reachable_pnet(field(common, 'PNET', UNION, Union()), peer_ip)
    scenario = field(request, 'SCNM', STRING)
    if scenario != 'ufcH2HUnranked':
        raise GameError('unsupported scenario')
    criteria = scenario_criteria(request)
    notes += expire(log)
    msid = next(_scenario_ids)
    if game_of(account.id):
        notes += leave(account.id, log, PLAYER_LEFT)  # Quick Fight's own private session
    notes += drop_search(account.id, log)
    other, attributes = None, None
    for search in SEARCHES:
        if search.account.id != account.id and search.scenario == scenario and search.version == version:
            attributes = match_attributes(search.criteria, criteria)
            if attributes:
                other = search
                break
    if not other:
        SEARCHES.append(Search(account, writer, pnet, msid, scenario, criteria, version))
        log(f'matchmaking {msid}: {account.name} searching {scenario or "(scenario)"} '
            f'(address {describe_pnet(pnet)}, criteria {criteria}); waiting for an opponent')
        return encode_struct(Struct(MSID=(INT, msid))), notes
    SEARCHES.remove(other)
    settings = Struct(MATCH_GAME)
    settings['CMGD'] = (STRUCT, Struct(GGTY=(INT, 0), GVER=(STRING, version)))
    gmcd = Struct(MATCH_GAME['GMCD'][1])
    gmcd['ATTR'] = (MAP, (STRING, STRING, attributes))
    settings['GMCD'] = (STRUCT, gmcd)
    host = Player(other.account, other.writer, other.pnet, 0)
    host.state = ACTIVE_CONNECTED
    game = Game(settings, host)
    guest = Player(account, writer, pnet, 1)
    host.matched = guest.matched = True
    game.players.append(guest)
    GAMES[game.gid] = game
    log(f'matchmaking: {other.account.name} (host, {describe_pnet(other.pnet)}) vs {account.name} '
        f'({describe_pnet(pnet)}) in new game {game.gid}, attributes {dict(attributes)}')
    for to, who in ((host, guest), (guest, host)):
        notes.append((to.writer, Frame(USER_SESSIONS, 2, 0, NOTIFICATION, user_added(who.account, who.pnet))))
        notes.append((to.writer, Frame(USER_SESSIONS, 5, 0, NOTIFICATION, user_updated(who.account))))
    host.search, guest.search = other.msid, msid
    for player, search_id, result in ((host, other.msid, 0), (guest, msid, 1)):
        # Blaze 15 carries the scenario id (MSCD) besides the session id (MSID); the client
        # finds its own scenario by it and leaves a game set up for an unknown one.
        found = Struct(FIT=(INT, 100), MAXF=(INT, 100), MSCD=(INT, search_id), MSID=(INT, search_id),
                       RSLT=(INT, result), USID=(INT, player.account.id))
        reason = Union(3, 'MMSC', STRUCT, found)
        notes.append((player.writer, Frame(GAME_MANAGER, NOTIFY_GAME_SETUP, 0, NOTIFICATION,
                                           game.setup(player, reason=reason))))
    return encode_struct(Struct(MSID=(INT, msid))), notes


def describe_pnet(pnet) -> str:
    try:
        pair = pnet.value
        out = []
        for tag in ('INIP', 'EXIP'):
            addr = pair[tag][1]
            ip = addr['IP'][1]
            out.append(f'{tag} {socket.inet_ntoa(ip.to_bytes(4, "big"))}:{addr["PORT"][1]}')
        return ', '.join(out)
    except Exception:
        return repr(pnet)
