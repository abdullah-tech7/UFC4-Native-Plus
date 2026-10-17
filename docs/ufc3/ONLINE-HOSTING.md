# Hosting a private UFC 3 match (Windows and Linux)

Both players need the same native-port build and the supported UFC 3 dump (CUSA06534, update 1.14). One player runs the server on their own PC. Either Windows or Linux can host; the two players may use different operating systems.

## On the same LAN

1. Open **UFC3.exe** on Windows, or **UFC3.sh** on Linux. The Linux GUI requires Python 3, PyGObject and GTK 4. Windows includes its Python runtime.
2. The host chooses **Host server** under **Online play**, selects their own LAN IP under **Host address**, and shares that address. **Rescan** refreshes the list; **Copy IP** copies the selected address.
3. The other player chooses **Join server** and enters that IP in **Join address**.
4. Both choose their game folder and press **Play**. The host launcher starts the server, verifies its TCP/UDP listeners, and starts the game. Both enter Online → Quick Fight, choose compatible matchmaking settings and search at nearly the same time.

The host launcher keeps its server running for that game session. Exiting the game, pressing Stop, or closing the host launcher ends the session and its server. It never takes over a server started elsewhere. For a separately managed server, both players use Join server instead.

## A friend outside your LAN

For this first implementation, install a VPN such as [Tailscale](https://tailscale.com/docs/use-cases/personal-or-at-home-use/share-private-game-server) separately on both PCs and arrange access between them. Neither launcher installs or configures the VPN.

Connect the VPN before opening the launcher (or press Rescan). The host selects **their VPN IPv4 address**, and the friend enters that same address in Join. The host's own game also connects to that address automatically. Do not use `127.0.0.1` or the LAN address for this test: the runtime uses the route to the server to select the address it announces to the other player.

Confirm that the PCs can reach each other through the VPN and allow the game and Python server through the firewall on that interface. A direct VPN connection is preferable for latency; VPN relay connections can add latency. See [Tailscale connection types](https://tailscale.com/docs/reference/connection-types).

LAN fights and the rematch patch have been validated by the user. Hosting with these launchers has passed local server checks, including the Windows executable under Wine. A gameplay test with a Windows host and a separate test across two Internet connections are still needed. This is private hosting, with no public matchmaking service, invitation system, built-in Internet NAT traversal, or automatic router setup. Forwarding only the server ports does not by itself establish the peer-to-peer fight connection.

## Logs and troubleshooting

- **Windows:** `%LOCALAPPDATA%\UFC3Native\logs\`; server logs are `server-*.log`.
- **Linux:** `~/.local/share/UFC3Native/logs/`; use Open logs in the launcher. `BB_DATA_DIR` changes the data location on either system.
- **Port occupied:** stop your previous host session, or use Join for a server you deliberately run separately. The launcher never kills an unrelated process.
- **Address missing or rejected:** connect the LAN/VPN, rescan, and select an IPv4 address actually assigned to the host PC. Do not enter the router's public IP in the Host field.
- **Friend cannot connect:** check the selected interface, VPN access rules, both firewalls, and matching versions. Read the host server log first.
- **Unexpected online address:** an inherited `BB_ONLINE_SERVER` overrides GUI Offline/Join settings. Remove it to control the mode from the GUI. Host requires any override to match the selected local IP.

Server listeners: TCP **42230, 40443, 40080, 10041, 10744, 17502**, plus UDP **17502** for QoS. Fight packets use a separate UDP peer-to-peer transport; its physical port is supplied by the game, not a fixed launcher setting. Allow the game on the private LAN/VPN interface rather than assuming the server-port list covers the fight.

The Windows diagnostic `UFC3.exe --check-host <local-IPv4>` starts and checks a server, then closes it without running the game. It writes `logs\host-check.log`. Linux command-line play remains available through `./ufc3.sh --play /path/to/CUSA06534` (or the previous positional-folder command). Opening `./ufc3.sh` without arguments now opens the GUI.
