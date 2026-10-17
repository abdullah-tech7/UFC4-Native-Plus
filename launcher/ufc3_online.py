"""UFC 3 launcher settings and owned server/game sessions (no GUI dependencies)."""
import ipaddress
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time

TCP_PORTS = (42230, 40443, 40080, 10041, 10744, 17502)
QOS_PORT = 17502
ROOT = Path(__file__).resolve().parent.parent
DEFAULTS = dict(game='', player='Player 1', language='1', mode='offline',
                join_address='', host_address='', fps='120', fullscreen=False,
                neo=False, skip_movies=True, present='Mailbox', resolution='1920x1080')
# Experimental scene patches (UFC-45/UFC-46): resolution -> (patches folder, patches config).
EXPERIMENTS = {'2560x1440': ('ufc46-1440p', 'patches-1440p.json'),
               '3840x2160': ('ufc45-4k', 'patches-4k.json')}


def data_dir():
    return Path(os.environ.get('BB_DATA_DIR') or
                Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'UFC3Native').resolve()


def read_legacy(path):
    values = {}
    try:
        for line in path.read_text().splitlines():
            key, sep, value = line.partition('=')
            if sep and key.startswith('BB_') and value:
                values[key] = value
    except OSError:
        pass
    return values


def load_settings(data):
    settings = dict(DEFAULTS)
    legacy = read_legacy(data / 'ufc3.conf')
    for key, env in [('player', 'BB_USER_NAME'), ('language', 'BB_LANGUAGE'),
                     ('fps', 'BB_FPS_LIMIT'), ('present', 'BB_PRESENT_MODE')]:
        if env in legacy:
            settings[key] = legacy[env]
    for key, env in [('fullscreen', 'BB_FULLSCREEN'), ('neo', 'BB_NEO'), ('skip_movies', 'BB_SKIP_MOVIES')]:
        if env in legacy:
            settings[key] = legacy[env] == '1'
    if legacy.get('BB_ONLINE_SERVER'):
        settings.update(mode='join', join_address=legacy['BB_ONLINE_SERVER'])
    try:
        settings['game'] = (data / 'game-folder').read_text().strip()
    except OSError:
        pass
    try:
        saved = json.loads((data / 'launcher.json').read_text())
        if isinstance(saved, dict):
            settings.update({k: v for k, v in saved.items() if k in DEFAULTS and type(v) is type(DEFAULTS[k])})
    except (OSError, ValueError):
        pass
    if settings['mode'] not in ('offline', 'join', 'host'):
        settings['mode'] = 'offline'
    return settings


def save_settings(data, settings):
    data.mkdir(parents=True, exist_ok=True)
    temp = data / 'launcher.json.tmp'
    temp.write_text(json.dumps({k: settings[k] for k in DEFAULTS}, indent=2) + '\n')
    temp.replace(data / 'launcher.json')


def local_addresses():
    """Return interface labels; socket bind below validates hand-entered addresses too."""
    try:
        result = subprocess.run(['ip', '-j', '-4', 'address', 'show', 'up'], capture_output=True,
                                text=True, timeout=3, check=True)
        return [(a['local'], device['ifname']) for device in json.loads(result.stdout)
                for a in device.get('addr_info', []) if a.get('family') == 'inet'
                and not ipaddress.IPv4Address(a['local']).is_loopback
                and not ipaddress.IPv4Address(a['local']).is_link_local]
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        return []


def validate_host(address):
    try:
        ip = ipaddress.IPv4Address(address)
    except ipaddress.AddressValueError as error:
        raise ValueError('Host address must be this PC\'s LAN or VPN IPv4 address.') from error
    if ip.is_loopback or ip.is_unspecified or ip.is_multicast or ip.is_link_local or ip.is_reserved:
        raise ValueError('Choose a reachable LAN or VPN IP, rather than a loopback or special address.')
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind((str(ip), 0))
    except OSError as error:
        raise ValueError(f'{address} is not an available local IP. Connect the network/VPN and rescan.') from error
    return str(ip)


def session_environment(settings, data, inherited=None):
    env = dict(os.environ if inherited is None else inherited)
    address = settings['host_address'].strip() if settings['mode'] == 'host' else settings['join_address'].strip()
    override = env.get('BB_ONLINE_SERVER')
    if settings['mode'] == 'host':
        address = validate_host(address)
        if override is not None and override != address:
            raise ValueError('BB_ONLINE_SERVER conflicts with the Host address. Remove it or select that same local IP.')
    if settings['mode'] == 'join' and not (override or address):
        raise ValueError('Enter the host\'s address to join a server.')
    if settings['mode'] != 'offline':
        env.setdefault('BB_ONLINE_SERVER', address)
    else:
        # Existing environment overrides keep their documented priority.
        if not override:
            env.pop('BB_ONLINE_SERVER', None)
    resolution = settings.get('resolution', '1920x1080')
    experiment = EXPERIMENTS.get(resolution)
    for key, value in dict(BB_USER_NAME=settings['player'], BB_LANGUAGE=settings['language'],
                           BB_FPS_LIMIT=settings['fps'], BB_PRESENT_MODE=settings['present'],
                           BB_FULLSCREEN=str(int(settings['fullscreen'])),
                           BB_NEO=str(int(settings['neo'] or experiment is not None)),
                           BB_SKIP_MOVIES=str(int(settings['skip_movies'])), BB_SAVE_LOG='1').items():
        env.setdefault(key, value)
    # 1080p is the game's default and needs no variables; 720p and the experiments set the
    # internal size, and the experiments also need PS4 Pro plus their scene patch (as in
    # tools/ufc45-4k-test.sh). Variables already exported by the user keep priority.
    if resolution == '1280x720':
        env.setdefault('BB_INTERNAL_WIDTH', '1280')
        env.setdefault('BB_INTERNAL_HEIGHT', '720')
    elif experiment:
        folder, config = experiment
        patches = ROOT / 'patches' / 'experiments' / folder
        width, height = resolution.split('x')
        env.setdefault('BB_INTERNAL_WIDTH', width)
        env.setdefault('BB_INTERNAL_HEIGHT', height)
        env.setdefault('BB_PATCHES_DIR', str(patches))
        env.setdefault('BB_PATCHES_CONFIG', str(patches / config))
    env['BB_DATA_DIR'] = str(data)
    return env


def terminate_owned(process):
    if process is None:
        return
    # Only this Popen's process group, created with start_new_session=True.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=4)


class HostServer:
    def __init__(self, root, data, address, ports=TCP_PORTS, qos_port=QOS_PORT):
        self.root, self.data, self.address = Path(root), Path(data), address
        self.ports, self.qos_port = tuple(ports), qos_port
        self.process = None
        self.log = None
        self.log_path = None

    def start(self, cancel=None, timeout=15):
        validate_host(self.address)
        # Bind before spawning to report occupied ports; never attach to someone else's server.
        probes = []
        try:
            for kind, port in [(socket.SOCK_STREAM, p) for p in self.ports] + [(socket.SOCK_DGRAM, self.qos_port)]:
                probe = socket.socket(socket.AF_INET, kind)
                probes.append(probe)
                if kind == socket.SOCK_STREAM:
                    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    probe.bind(('0.0.0.0', port))
                except OSError as error:
                    raise RuntimeError(f'Port {port}/{"TCP" if kind == socket.SOCK_STREAM else "UDP"} is unavailable. Close the other host or choose Join server.') from error
        finally:
            for probe in probes:
                probe.close()
        logs = self.data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        self.log_path = logs / f'server-{time.time_ns()}.log'
        self.log = self.log_path.open('wb')
        try:
            self.process = subprocess.Popen([sys.executable, '-u', str(self.root / 'tools/ufc3_server.py'),
                                             '--bind', '0.0.0.0', '--ports', ','.join(map(str, self.ports))],
                                            cwd=self.data, stdout=self.log, stderr=subprocess.STDOUT,
                                            stdin=subprocess.DEVNULL, start_new_session=True)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if cancel and cancel.is_set():
                    raise InterruptedError('Launch cancelled.')
                if self.process.poll() is not None:
                    raise RuntimeError(f'Server exited during startup. See {self.log_path}')
                ready = True
                for port in self.ports:
                    try:
                        with socket.create_connection((self.address, port), timeout=.15):
                            pass
                    except OSError:
                        ready = False
                        break
                if ready:
                    try:
                        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                            udp.settimeout(.2)
                            udp.sendto(b'UFC3 host check', (self.address, self.qos_port))
                            reply, sender = udp.recvfrom(128)
                            ready = reply == b'UFC3 host check' and sender[0] == self.address and sender[1] == self.qos_port
                    except OSError:
                        ready = False
                if ready and self.process.poll() is None:
                    return
                if cancel:
                    cancel.wait(.1)
                else:
                    time.sleep(.1)
            raise RuntimeError(f'Server did not become ready at {self.address}. Check the firewall and {self.log_path}')
        except BaseException:
            self.stop()
            raise

    def stop(self):
        terminate_owned(self.process)
        self.process = None
        if self.log:
            self.log.close()
            self.log = None


def run_session(root, data, settings, cancel, notify):
    """Run off the GTK thread. All owned children are reaped on every exit path."""
    root, data = Path(root).resolve(), Path(data).resolve()
    server = None
    game = None
    try:
        folder = Path(settings['game']).expanduser().resolve()
        if not (folder / 'eboot.bin').is_file():
            raise ValueError('Choose the UFC 3 game folder containing eboot.bin (CUSA06534 + update 1.14).')
        env = session_environment(settings, data)
        if cancel.is_set():
            return
        if settings['mode'] == 'host':
            server = HostServer(root, data, settings['host_address'].strip())
            notify('Starting server…')
            server.start(cancel)
            notify(f'Server ready. Share {server.address} with your friend. Log: {server.log_path}')
        if cancel.is_set():
            return
        if (root / 'bin/sacredfire').is_file():
            command = ['bash', str(root / 'ufc3.sh'), '--play', str(folder)]
        else:
            merge = subprocess.run([sys.executable, str(root / 'scripts/merge_update.py'), str(folder)],
                                   capture_output=True, text=True, check=True, env=env)
            env['BB_GAME_DIR'] = merge.stdout.strip().splitlines()[-1]
            command = ['bash', str(root / 'run.sh')]
        if cancel.is_set():
            return
        logs = data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        with (logs / f'launcher-run-{time.time_ns()}.log').open('wb') as log:
            game = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, start_new_session=True)
            notify('Game running' + (f' · Hosting at {server.address}' if server else ''))
            while game.poll() is None:
                if cancel.wait(.2):
                    break
                if server and server.process.poll() is not None:
                    raise RuntimeError(f'The hosted server stopped unexpectedly. See {server.log_path}')
            if not cancel.is_set():
                notify(f'Game exited with code {game.returncode}. Logs: {logs}')
    finally:
        terminate_owned(game)
        if server:
            server.stop()
