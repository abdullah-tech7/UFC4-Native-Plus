"""Opt-in packaged launcher checks under Wine (never runs the game).

UFC3_WINDOWS_PACKAGE=/path/to/extracted/UFC3Native UFC3_HOST_IP=<local-IP>
WINEPREFIX=/path/to/existing/prefix xvfb-run -a python3 -m unittest tests.test_windows_host_smoke
"""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get('UFC3_WINDOWS_PACKAGE') and os.environ.get('UFC3_HOST_IP'),
                     'Set UFC3_WINDOWS_PACKAGE and UFC3_HOST_IP for the Wine smoke check')
class PackagedWindowsHost(unittest.TestCase):
    def check_host(self, expected):
        with tempfile.TemporaryDirectory(prefix='ufc3-host-data-') as folder:
            env = dict(os.environ, BB_DATA_DIR='Z:' + folder.replace('/', '\\'), BB_NO_DIALOGS='1', WINEDEBUG='-all')
            command = ['wine', str(Path(env['UFC3_WINDOWS_PACKAGE']) / 'UFC3.exe'),
                       '--check-host', env['UFC3_HOST_IP']]
            result = subprocess.run(command, env=env, capture_output=True, timeout=45)
            log = (Path(folder) / 'logs/host-check.log').read_text(errors='replace')
            self.assertEqual(result.returncode, expected, log)
            return log

    def test_real_server_readiness_and_cleanup(self):
        log = self.check_host(0)
        self.assertIn('Host server: ready on', log)
        self.assertIn('stopped (the host check finished)', log)
        for port in (42230, 40443, 40080, 10041, 10744, 17502):
            with socket.socket() as probe:
                probe.settimeout(.2)
                self.assertNotEqual(probe.connect_ex((os.environ['UFC3_HOST_IP'], port)), 0)

    def test_occupied_port_is_reported_without_taking_over(self):
        with socket.socket() as other:
            other.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            other.bind(('0.0.0.0', 42230))
            other.listen()
            log = self.check_host(1)
            self.assertIn('42230/TCP already in use', log)
            with socket.create_connection((os.environ['UFC3_HOST_IP'], 42230), timeout=1):
                pass


if __name__ == '__main__':
    unittest.main()
