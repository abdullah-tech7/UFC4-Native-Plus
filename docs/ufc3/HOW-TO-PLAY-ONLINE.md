# How to play online

This is private hosting: one player runs the server on their own PC. There is no public matchmaking. Full technical details live in [ONLINE-HOSTING.md](ONLINE-HOSTING.md).

## Before you start

1. Both players need the **same version of the port** and the **same game dump/update**.

## Same network (LAN)

2. The host opens the launcher, picks **Host server**, and presses **Play**. The launcher shows the IP to share — use **Copy IP** to send it to your friend.
3. The friend picks **Join server**, pastes that IP, and presses **Play**.

## Different networks (over the Internet)

4. Both players install [Tailscale](https://tailscale.com) (or another VPN) and sign in to the same tailnet.
5. The host selects **their VPN IP** (a `100.x.y.z` address) as the host address — never `127.0.0.1` — and shares it.
6. The friend joins using that VPN IP.

## In game

7. Both players open the online menu and enter **Quick Fight** (or ranked) search at about the same time.
8. After the fight, **rematch works** — you can run it back without re-hosting.

## Troubleshooting

- **Friend cannot connect:** allow the game and the server through the firewall on the private LAN/VPN interface (the fight uses its own peer-to-peer UDP traffic, not only the listed server ports).
- **Host must stay in the game:** exiting the game, pressing Stop, or closing the host launcher ends the session and its server.
- **Check versions:** both players must run the same port build and game update; a version mismatch fails to connect.
- **Address missing:** connect the LAN/VPN first, then press Rescan in the launcher.
