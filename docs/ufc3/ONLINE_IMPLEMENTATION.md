# Online implementation — 2026-10-14

Work in progress, uncommitted. References and pinned upstream revisions:
[ONLINE_REFERENCES.md](ONLINE_REFERENCES.md).

## Confirmed progress

The real UFC executable reached `LOGIN_STATE_BLAZE_AUTHENTICATED` (state 10).
Evidence: `out/run-codex-usersession.log`, with the corresponding server traffic
in `out/server-codex-usersession.log`. This is authentication evidence, not a
verified online fight.

The previous blocker between receiving the Nucleus server code and sending a
Blaze login is resolved. UFC now sends Authentication component 1, command 10
with AUTH, CTYP, EXTB, EXTI, PRDT. It does not use BF4's Accounts 0x23 command.

### Runtime HTTP

- Nonblocking transfers run on a host worker. Guest callbacks stay on the guest
  caller thread and run when SendRequest is retried after transfer completion.
  This matches UFC's ProtoHttp retry path; a callback delivered later from the
  worker bypassed its header-cache update and caused the false Location error.
- Pending status returns BEFORE_SEND, which UFC explicitly retries.
- Response publication uses acquire/release atomics; request deletion joins its
  worker. Connect/send/receive are bounded, with partial-write handling.
- HTTP request logs omit query credentials. Response-header parsing is bounded
  and case insensitive. errno storage is per thread.

### Local identity and Blaze session

`tools/ufc3_auth.py` binds simulated PSN account -> OAuth code/token -> Blaze
account and persona. Codes expire, are single use, are bound to their client ID
and PSN account, and cannot be redeemed by another client/account. Repeated
tokeninfo requests retain the account ID. The runtime adds local identity headers
to HTTP requests; this is a development LAN identity scheme, not production auth.

UFC sends token grants as POST with parameters in the URL and an empty body.
The server accepts that and regular form bodies, rejecting conflicting values.

Schema differences verified in UFC's own reflection:

- LoginResponse: ANON, SESS, SPAM, UNDR.
- UserLoginInfo: 1CON, BUID, FRST, GEO, KEY, LLOG, MAIL, PDTL, UID.
- PersonaDetails: DSNM, LAST, PID, PLAT, STAS, XREF.
- ClientPlatformType: PC=4, PS4=20. Returning BF4's PC value would be wrong.
- UserSessionLoginInfo is sent as UserSessions notification 8; UserAdded and
  UserUpdated follow as notifications 2 and 5. Reply and notifications share
  the same account and session key. UserIdentification has AID, ALOC, EXBB,
  EXID, ID, NAME, NASP, ORIG, PIDI.
- PostAuthResponse (Util 9/8): TELE, TICK, UROP. UFC has no BF4 PSS field here.
  Telemetry/ticker endpoints are disabled; UROP identifies the local UID.

After state 10, UFC requested further service/configuration data and hit a null
numeric conversion at guest 0x402f0. The probe captured the caller 0x3b69f9 and
the configuration getter 0x34fe90, which reads **projectId** from UfcConfig.
The server now provides projectId="0" for the disabled local telemetry project.
The independent Util getTelemetryServer (9/5) route also returns explicit
disabled configuration. Do not infer that all post-login services are complete.

### Two-PC lobby (14 Oct, latest handoff)

The newer notes in `~/ufcCOntext/ONLINE-LOBBY-HANDOFF.md` supersede the earlier
status below: Linux and Windows both sign into the local backend, enter Quick
Fight → Search session, receive the GameManager roster, report mesh connection
in both directions, exchange DirtySDK UDP traffic on port 3659, and reach RED
vs BLUE. The server trace confirms `NotifyGameSetup`, UserSessions additions,
`NotifyGamePlayerStateChange`, join completion, and matchmaking validation.
This verifies a cross-platform lobby connection, not a playable fight.

The current hard blocker is that neither Select nor Back acts on RED vs BLUE.
The screen's relevant `network:pregame` service commands do not progress to
`send-pregame-data` or `get-gamesettings`; P2P continues with small keepalives.
The game creates its live match socket as ordinary UDP (`sceNetSocket` type 2),
binds it to port 3659, and uses that socket successfully in both directions.
The separate virtual-port type-6 implementation is not on this observed match
path. The 1055/1039-byte startup exchange completes symmetrically, followed by
one 52-byte request/reply; thereafter only 26/43-byte keepalives are visible.
The app payload is ProtoTunnel encrypted, so those lengths alone do not identify
the missing state message.
The UI probe now writes command traces and caller stacks to
`out/ufc3-ui-probe.log` on the next launch. Tests requiring menu input remain
manual; no input is injected.

For the next run, `BB_NET_TRACE_SIZE=52` records the guest caller chain for the
single 52-byte UDP exchange. This should identify which UFC code builds and
consumes the pregame packet without flooding logs with every epoll poll.

Live Windows + Linux run (2026-10-15, `logsUFC/20261015_003631.log` and
`20261015_003702.log`): Linux logged a 52-byte UDP send to
`192.168.1.129:3659`; Windows logged a 52-byte UDP receive from
`192.168.1.2:3659`. This also proves `BB_NET_TRACE_SIZE=52` reached the Windows
game process. The Linux UI trace still ends at `request-initial-sync`; there is
no observed `send-pregame-data` or `get-gamesettings`. With `BB_NET_LOG=1`, the
logs do not include other UDP packet sizes or payloads, so the lack of another
52-byte trace does not establish that Windows sent no response. Next capture
both endpoints with `BB_NET_LOG=3` (logs the first 96 bytes of each UDP packet)
and correlate the decrypted/ProtoTunnel payload with the pregame UI commands.

Follow-up live run (Linux `logsUFC/20261015_004916.log`, Windows
`logsUFC/20261015_004822.log`, input capture `logsUFC/windows-pad.txt`): the
Windows input recorder captured button masks `16384` (Cross) and `8192`
(Circle), matching `runtime_pad.c`; the keyboard reaches `scePadReadState` even
though Windows reports no physical gamepad. Linux's UI probe still records only
the initial transition/listener/sync commands. Across the two logs the match
socket exchanged 1055/1039-byte startup packets, the 52-byte sync exchange,
and recurring 26/43-byte datagrams; there is no larger follow-up packet carrying
the complete pregame settings. Thus the keyboard mapping is not the observed
blocker on Windows, and the missing pregame state remains the leading issue.
The datagram bodies after the tunnel setup are encrypted, so packet sizes do
not establish their application meaning.
Follow-up live run (Linux `logsUFC/20261015_010405.log`, Windows
`logsUFC/20261015_010407.log`, Linux input `logsUFC/linux-pad.txt`): the Linux
input recording captured masks `16384` (Cross, Space) and `8192` (Circle), so
the attempted keyboard input reaches the pad state. The GDB trace again stops
at the initial transition, data-change listener, and `request-initial-sync`;
the candidate pregame-state writer and send path at `0x2f5100` and `0x2fe860`
were not reached. The Linux and Windows packet-size histograms match exactly
(262 UDP events each, including 1055/1039-byte startup traffic and recurring
26/43-byte packets), confirming bidirectional transport during this run. The
button input and basic UDP path are therefore no longer the leading suspects;
the remaining gap is the game's pregame state/message flow after initial sync.
Datagrams are ProtoTunnel-encrypted, so matching lengths do not identify their
application payloads. The input capture starts/stops with F9 and is enabled by
`BB_PAD_RECORD`.

The next trace at `logsUFC/20261015_011052.log` reached both
`request-initial-sync` and its callback at guest offsets `0x2e6361` and
`0x2e63c5`. Disassembly shows the callback target (`0x5ec0b0`) is a generic
completion forwarder: it checks two nested objects and, when ready, dispatches
the status byte and result data through a second virtual callback. It does not
itself encode or send a pregame packet. The session continued exchanging small
encrypted tunnel datagrams, with no larger settings/state packet visible. The
UI probe now records the nested callback object and its virtual target on the
next run, which should identify the actual completion handler.

For future probes, launch through `run.sh` so UFC 3's GPU/vblank settings are
applied. Directly invoking `out/sacredfire` omitted them (60 Hz instead of the
configured 480 Hz) and produced repeated FlipManager timeouts and a frozen
screen. With the existing build, set `BB_PREBUILT=1` and
`BB_PROBE=tools/ufc3-ui-probe.sh` when using `run.sh`; set `BB_NET_LOG=0` to
avoid flooding the terminal with per-call network logs.

Live connection check (`logsUFC/20261015_012140.log`): Linux started with
`BB_ONLINE_SERVER=192.168.1.2`. While both games were at RED/BLUE, `ss -tnp`
showed an established Linux game connection from `192.168.1.2` to server port
`10041`, and a second established port-10041 connection from Windows
`192.168.1.129`. The redirector on `192.168.1.2:42230` returned Blaze at
`192.168.1.2:10041`; both endpoints are therefore connected to the local
server at the TCP layer.

The temporary GDB setting `handle SIGTRAP ... nopass` froze UFC 3 with
`BB_WRITE_STEP` enabled: the runtime's own Linux single-step handler uses
`SIGTRAP`, so GDB swallowed the runtime's step notifications. That rule has
been removed. Recovery launch `logsUFC/20261015_012652.log` is running normally
with `BB_ALLOW_PTRACE=1`, the correct 480 Hz vblank, active frame rendering,
and an established Linux-to-Blaze TCP connection. Do not use the probe until
its signal handling is validated with this write-step path.

Role-reversal run (2026-10-15 01:35, Linux host): Linux (`Player_1`) searched
first and Windows (`myzkW`) joined. `out/server-linux-host-test.log` confirms
that Player_1 was assigned host, both mesh endpoints connected, both
matchmaking searches validated, and the search completed. Linux displayed its
local fight-rules bar; Windows remained on the RED/BLUE screen without those
settings. The Windows log (`logsUFC/20261015_013057.log`) shows a 52-byte UDP
request to Linux and a 52-byte reply, followed by only 26/43-byte tunnel
keepalives; no larger pregame-settings datagram is visible. The contents are
ProtoTunnel-encrypted, so packet lengths do not reveal the message type. This
reproduces the guest-side stall with the host OS reversed and rules out a
matchmaking/P2P-connectivity failure. It does not yet distinguish a shared
guest UI/state-handler problem from a host-side trigger that fails to publish
the full pregame state. The Linux run used `BB_NET_LOG=0`, so its own packet
trace was not captured; next run should capture `BB_NET_LOG=3` on Linux too
and probe `network:pregame` commands on the Linux host while it shows the rules.

Schema review of the live UFC reflection found one concrete GameManager roster
mistag: `ReplicatedGamePlayer` calls its external blob `EXBL`, while `EXBB`
belongs to `UserIdentification`. The roster now sends `EXBL` and the class's
`BLOB` custom-data field. A regression test decodes both matchmaking setup
notifications and checks the two player names and these exact tags. This
corrects the wire schema, but is not yet proven to unlock the UI.

### Datagram P2P

`src/runtime_net_p2p.h` implements type-6 sockets with a shared physical UDP
socket and independent virtual ports. Header format/architecture were compared
with bbhost; the implementation is C for this runtime, without importing its
Bloodborne backend.

- Encodes/decodes byte and 16-bit virtual ports; accepts optional incoming comid.
- Demultiplexes into bounded queues (128 packets / 256 KiB per virtual socket).
- Preserves sender IP, physical port and virtual port; supports peek and NBIO.
- Epoll readiness comes from the correct virtual queue; packets for other ports
  do not make every virtual socket readable.
- Closing or shutting down one virtual socket leaves its siblings alive.
- Physical transport remains nonblocking; recv/epoll pump queues. Blocking recv
  checks every millisecond, so this is not bbhost's reader-thread/eventfd design.

## Validation

- Sixteen Python tests pass, covering HTTP framing, the OAuth identity chain,
  expiry/replay, wrong-account/client rejection, login + notifications +
  postAuth over TCP, and the schema-correct two-player GameManager roster.
- C ABI tests cover UDP, errno per thread, response headers, HTTP redirects,
  actual P2P wire packets from a plain UDP peer, virtual-port isolation,
  epoll readiness, peek, malformed headers, close/shutdown and physical rebind.
- C checks passed with AddressSanitizer and UndefinedBehaviorSanitizer.
- Two separate Linux processes passed both UDP and type-6 P2P round trips.
- Windows runtime build passed: `out/build-windows-online-session.log`.
  The Windows client has also run natively on the user's Omen and joined the
  Linux client through the local backend, reaching the shared RED vs BLUE
  lobby. Earlier Wine checks are not used as cross-platform proof.

Commands from the repository root:

```sh
python3 -m unittest discover -s tests -p test_online_server.py -v
cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_online_net.c src/runtime_net.c -o out/online-net-test
out/online-net-test --http  # requires the local server
python3 tools/check-online-transport.py
```

`tools/ufc3-login-probe.sh` / `.gdb` trace login states. The null-number probe
stops before dereferencing null and prints a frame-pointer chain and getter.
Do not inject game input. Stop the debugger/game before rebuilding the runtime;
the game's internal timeout did not reliably end debugger runs.

## Remaining work

- Trace why RED vs BLUE does not dispatch/apply pregame input and state. Verify
  the corrected roster tag in a fresh match, compare both endpoints' UI command
  traces, and reverse the 52-byte state-sync packet via its guest caller chain
  before changing behavior.
- Unsupported Blaze commands still get empty success replies and need a proper
  Fire2 error/allowlist policy.
- STUN, hole punching and relay are not implemented. Current P2P proof is direct
  same-LAN UDP; it does not establish NAT traversal or a synchronized fight.
- Type 10 is still ordinary TCP, not a PS4 reliable P2P transport implementation.
- Default simulated PSN IDs still hash the display name; two instances need
  different BB_USER_NAME values until persistent per-install identity is added.
- DNS rewriting does not filter direct-IP egress. PSN invitations remain stubs.
- No fight has been started or validated. Two-PC Linux/Windows lobby setup is
  verified; Linux/Linux and Windows/Windows have not yet been tested. No online
  release has been published.
