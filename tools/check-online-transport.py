#!/usr/bin/env python3
"""Run UDP ABI probes between Linux and Windows binaries. Wine is a compatibility check,
not verification on physical Windows machines or proof of in-game matchmaking."""
import argparse
import os
from pathlib import Path
import queue
import socket
import subprocess
import threading


def run_pair(listener_command, peer_command, p2p=False):
    mode = 'P2P' if p2p else 'UDP'
    listen_flag, peer_flag = ('--p2p-listen', '--p2p-peer') if p2p else ('--listen', '--peer')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    environment = dict(os.environ, WINEDEBUG='-all')
    listener = subprocess.Popen(listener_command + [listen_flag, str(port)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, env=environment)
    lines = queue.Queue()
    output = []
    def drain():
        for line in listener.stdout:
            output.append(line)
            lines.put(line.strip())
        lines.put(None)
    pump = threading.Thread(target=drain, daemon=True)
    pump.start()
    try:
        while True:
            line = lines.get(timeout=20)
            if line is None:
                raise RuntimeError('listener exited before readiness: ' + ''.join(output))
            if line == 'READY':
                break
        peer = subprocess.run(peer_command + [peer_flag, '127.0.0.1', str(port)],
                              capture_output=True, text=True, env=environment, timeout=15)
        if peer.returncode or f'Peer {mode} roundtrip OK' not in peer.stdout:
            raise RuntimeError('peer failed: ' + peer.stdout + peer.stderr)
        if listener.wait(timeout=15):
            raise RuntimeError('listener failed: ' + ''.join(output))
        pump.join(timeout=1)
        if f'Peer {mode} roundtrip OK' not in ''.join(output):
            raise RuntimeError('listener did not verify the packet')
    finally:
        if listener.poll() is None:
            listener.terminate()
            try:
                listener.wait(timeout=5)
            except subprocess.TimeoutExpired:
                listener.kill()
                listener.wait()
        listener.stdout.close()


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine', action='store_true', help='also test the Windows PE via Wine')
    args = parser.parse_args()
    linux = [str(repo / 'out/online-net-test')]
    pairs = [('Linux / Linux', linux, linux)]
    if args.wine:
        windows = ['wine', str(repo / 'out/windows/bin/online-net-test.exe')]
        pairs += [('Linux / Windows (Wine)', linux, windows),
                  ('Windows (Wine) / Linux', windows, linux),
                  ('Windows / Windows (both Wine)', windows, windows)]
    for label, listener, peer in pairs:
        for p2p in (False, True):
            run_pair(listener, peer, p2p)
            print(label + ': ' + ('P2P virtual-port' if p2p else 'UDP') + ' roundtrip verified', flush=True)


if __name__ == '__main__':
    main()
