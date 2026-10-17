# SPDX-License-Identifier: GPL-2.0-or-later
"""Verified save snapshots. Inspired by FelipePrado0/gow3_pc's prelaunch backups.

Unlike that helper, completed snapshots are hash-checked, published atomically and
never automatically deleted. No automatic restore and no cache/game data copies.
"""
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import uuid


def _plain(path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError(f'No se respaldan enlaces ni puntos de reanálisis: {path}')


def inventory(folder):
    folder = Path(folder)
    if not folder.exists():
        return {}
    _plain(folder)
    files = {}
    for directory, folders, names in os.walk(folder, followlinks=False):
        for name in folders:
            _plain(Path(directory) / name)
        for name in sorted(names):
            path = Path(directory) / name
            _plain(path)
            if not path.is_file():
                raise ValueError(f'Archivo de guardado no regular: {path}')
            digest = hashlib.sha256()
            size = 0
            with path.open('rb') as stream:
                while block := stream.read(1024 * 1024):
                    digest.update(block)
                    size += len(block)
            files[path.relative_to(folder).as_posix()] = {'size': size, 'sha256': digest.hexdigest()}
    return dict(sorted(files.items()))


def create_backup(savedata, destination):
    savedata, destination = Path(savedata), Path(destination)
    if savedata.exists():
        _plain(savedata)
    if destination.exists():
        _plain(destination)
    savedata, destination = savedata.resolve(), destination.resolve()
    if savedata == destination or savedata in destination.parents or destination in savedata.parents:
        raise ValueError('El respaldo y los guardados deben estar en carpetas separadas.')
    before = inventory(savedata)
    if not before:
        return None
    destination.mkdir(parents=True, exist_ok=True)
    candidates = sorted(destination.glob('snapshot-*/manifest.json'), reverse=True)
    for candidate in candidates:
        _plain(candidate.parent)
        _plain(candidate)
        try:
            previous = json.loads(candidate.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if previous.get('source') != str(savedata) or previous.get('schema') != 1:
            continue
        if previous.get('files') == before and inventory(candidate.parent / 'files') == before:
            return None
        break
    stamp = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    identity = stamp + '-' + uuid.uuid4().hex[:8]
    staging = destination / ('.pending-' + identity)
    staging.mkdir()
    shutil.copytree(savedata, staging / 'files')
    if inventory(staging / 'files') != before or inventory(savedata) != before:
        raise RuntimeError('Los guardados cambiaron durante el respaldo; se conserva la copia incompleta para revisar.')
    manifest = {'schema': 1, 'created_utc': stamp, 'source': str(savedata), 'files': before}
    with (staging / 'manifest.json').open('w', encoding='utf-8') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    completed = destination / ('snapshot-' + identity)
    staging.rename(completed)
    return completed


@contextmanager
def launch_guard(user_folder):
    """Hold a process lock through snapshotting and the entire child lifetime."""
    user_folder = Path(user_folder)
    user_folder.mkdir(parents=True, exist_ok=True)
    path = user_folder / 'launcher-session.lock'
    if path.exists():
        _plain(path)
    stream = path.open('a+b')
    locked = False
    try:
        if path.stat().st_size == 0:
            stream.write(b'\0')
            stream.flush()
        stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked = True
        except OSError as error:
            raise RuntimeError('Ya hay una sesión abierta con este perfil. Cerrala antes de volver a iniciar.') from error
        yield
    finally:
        if locked:
            stream.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        stream.close()
