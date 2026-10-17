from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'launcher'))
from ufc3_online import (DEFAULTS, HostServer, TCP_PORTS, QOS_PORT, load_settings,
                        local_addresses, run_session, save_settings, session_environment, validate_host)


class LauncherSettingsTests(unittest.TestCase):
    def test_legacy_config_is_data_and_migrates_online(self):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            (data / 'ufc3.conf').write_text('BB_ONLINE_SERVER=100.80.1.2\nBB_USER_NAME=$(exit 1)\nBB_FPS_LIMIT=90\n')
            settings = load_settings(data)
            self.assertEqual((settings['mode'], settings['join_address']), ('join', '100.80.1.2'))
            self.assertEqual(settings['player'], '$(exit 1)')
            settings['mode'] = 'offline'
            save_settings(data, settings)
            self.assertEqual(load_settings(data)['mode'], 'offline')

    def test_wrong_types_do_not_override_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            (data / 'launcher.json').write_text('{"fullscreen":"false","game":null,"mode":"bad"}')
            settings = load_settings(data)
            self.assertFalse(settings['fullscreen'])
            self.assertEqual(settings['game'], '')
            self.assertEqual(settings['mode'], 'offline')

    def test_join_requires_address_and_environment_override_wins(self):
        settings = dict(DEFAULTS, mode='join')
        with self.assertRaises(ValueError):
            session_environment(settings, Path('/tmp'), {})
        env = session_environment(settings, Path('/tmp'), {'BB_ONLINE_SERVER': '100.80.1.2', 'BB_FPS_LIMIT': '90'})
        self.assertEqual(env['BB_ONLINE_SERVER'], '100.80.1.2')
        self.assertEqual(env['BB_FPS_LIMIT'], '90')
        self.assertNotIn('BB_ONLINE_SERVER', session_environment(DEFAULTS, Path('/tmp'), {}))

    def test_host_rejects_special_or_nonlocal_addresses(self):
        for address in ('127.0.0.1', '0.0.0.0', '224.0.0.1', '169.254.1.1', 'bad', '192.0.2.123'):
            with self.subTest(address=address), self.assertRaises(ValueError):
                validate_host(address)


class ResolutionTests(unittest.TestCase):
    def test_4k_experiment_environment(self):
        env = session_environment(dict(DEFAULTS, resolution='3840x2160'), Path('/tmp'), {})
        self.assertEqual(env['BB_NEO'], '1')
        self.assertEqual((env['BB_INTERNAL_WIDTH'], env['BB_INTERNAL_HEIGHT']), ('3840', '2160'))
        self.assertTrue(env['BB_PATCHES_DIR'].endswith('patches/experiments/ufc45-4k'))
        self.assertTrue(env['BB_PATCHES_CONFIG'].endswith('ufc45-4k/patches-4k.json'))

    def test_2k_experiment_environment_and_user_exports_win(self):
        env = session_environment(dict(DEFAULTS, resolution='2560x1440'), Path('/tmp'), {})
        self.assertEqual(env['BB_NEO'], '1')
        self.assertEqual((env['BB_INTERNAL_WIDTH'], env['BB_INTERNAL_HEIGHT']), ('2560', '1440'))
        self.assertTrue(env['BB_PATCHES_DIR'].endswith('patches/experiments/ufc46-1440p'))
        self.assertTrue(env['BB_PATCHES_CONFIG'].endswith('ufc46-1440p/patches-1440p.json'))
        exported = session_environment(dict(DEFAULTS, resolution='2560x1440'), Path('/tmp'),
                                       {'BB_INTERNAL_WIDTH': '1920', 'BB_PATCHES_DIR': '/custom'})
        self.assertEqual(exported['BB_INTERNAL_WIDTH'], '1920')
        self.assertEqual(exported['BB_PATCHES_DIR'], '/custom')

    def test_1080p_is_untouched_and_resolution_persists(self):
        env = session_environment(DEFAULTS, Path('/tmp'), {})
        self.assertNotIn('BB_INTERNAL_WIDTH', env)
        self.assertNotIn('BB_PATCHES_DIR', env)
        self.assertEqual(env['BB_NEO'], '0')
        with tempfile.TemporaryDirectory() as folder:
            data = Path(folder)
            save_settings(data, dict(DEFAULTS, resolution='3840x2160'))
            self.assertEqual(load_settings(data)['resolution'], '3840x2160')


class ServerLifecycleTests(unittest.TestCase):
    def setUp(self):
        addresses = local_addresses()
        if not addresses:
            self.skipTest('Needs an available LAN/VPN IPv4 address')
        self.address = addresses[0][0]

    def test_host_conflicting_environment_is_rejected(self):
        with self.assertRaises(ValueError):
            session_environment(dict(DEFAULTS, mode='host', host_address=self.address), Path('/tmp'),
                                {'BB_ONLINE_SERVER': '127.0.0.1'})

    def test_occupied_port_does_not_start_or_kill_another_server(self):
        with tempfile.TemporaryDirectory() as folder, socket.socket() as other:
            other.bind(('0.0.0.0', 0))
            other.listen()
            port = other.getsockname()[1]
            server = HostServer(ROOT, folder, self.address, ports=(port,))
            with self.assertRaisesRegex(RuntimeError, f'Port {port}/TCP'):
                server.start()
            self.assertIsNone(server.process)
            with socket.create_connection((self.address, port), timeout=1):
                pass

    def test_partial_startup_is_terminated_and_reaped(self):
        with tempfile.TemporaryDirectory() as folder, socket.socket() as reserve:
            reserve.bind(('0.0.0.0', 0))
            port = reserve.getsockname()[1]
            reserve.close()
            root = Path(folder)
            (root / 'tools').mkdir()
            (root / 'tools/ufc3_server.py').write_text('import time\ntime.sleep(30)\n')
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.bind(('0.0.0.0', 0))
                qos = udp.getsockname()[1]
            server = HostServer(root, root, self.address, ports=(port,), qos_port=qos)
            with self.assertRaisesRegex(RuntimeError, 'did not become ready'):
                server.start(timeout=.3)
            self.assertIsNone(server.process)
            self.assertIsNone(server.log)

    def test_real_server_ready_on_all_listeners_and_releases_ports(self):
        held = []
        try:
            for kind, port in [(socket.SOCK_STREAM, p) for p in TCP_PORTS] + [(socket.SOCK_DGRAM, QOS_PORT)]:
                sock = socket.socket(socket.AF_INET, kind)
                held.append(sock)
                if kind == socket.SOCK_STREAM:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind(('0.0.0.0', port))
        except OSError:
            self.skipTest('A server already owns the standard UFC 3 ports')
        finally:
            for sock in held:
                sock.close()
        with tempfile.TemporaryDirectory() as folder:
            server = HostServer(ROOT, folder, self.address)
            try:
                server.start(timeout=5)
                process = server.process
                self.assertIsNone(process.poll())
                # Actual protocol request in addition to the TCP/UDP readiness checks.
                with socket.create_connection((self.address, 42230), timeout=1) as connection:
                    connection.sendall(b'GET / HTTP/1.1\r\nHost: test\r\n\r\n')
                    self.assertIn(b'200 OK', connection.recv(4096))
            finally:
                server.stop()
            self.assertIsNotNone(process.poll())
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.bind(('0.0.0.0', QOS_PORT))

    def test_host_can_restart_after_a_completed_protocol_connection(self):
        with tempfile.TemporaryDirectory() as folder:
            for _ in range(2):
                server = HostServer(ROOT, folder, self.address)
                try:
                    server.start(timeout=5)
                    with socket.create_connection((self.address, 42230), timeout=1) as connection:
                        connection.sendall(b'GET / HTTP/1.1\r\nHost: test\r\n\r\n')
                        while connection.recv(4096):
                            pass
                finally:
                    server.stop()

    def test_game_session_stop_reaps_process_group(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'bin').mkdir()
            (root / 'bin/sacredfire').touch()  # package layout; this is not a game executable
            (root / 'dump').mkdir()
            (root / 'dump/eboot.bin').touch()
            (root / 'ufc3.sh').write_text('sleep 30 &\necho "$!" > child.pid\nwait\n')
            messages = []
            cancel = threading.Event()
            worker = threading.Thread(target=run_session, args=(root, root / 'data',
                                      dict(DEFAULTS, game=str(root / 'dump')), cancel, messages.append))
            worker.start()
            deadline = time.monotonic() + 3
            while not (root / 'child.pid').exists() and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertTrue((root / 'child.pid').exists())
            cancel.set()
            worker.join(5)
            self.assertFalse(worker.is_alive())


if __name__ == '__main__':
    unittest.main()
