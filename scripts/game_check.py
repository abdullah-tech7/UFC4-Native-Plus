# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact executable and library checks for the measured UFC 4 dumps.

Profiles establish file identity only; they do not imply successful game execution.
Unknown executables are rejected. No address-specific UFC 3 build is selected.
"""
import hashlib
import json
from pathlib import Path
import sys

SUPPORTED = json.loads((Path(__file__).with_name('ufc4_profiles.json')).read_text(encoding='utf-8'))
BY_TITLE = {p['title']: p for p in SUPPORTED.values()}
SUPPORTED_TITLE = 'CUSA14209'
SUPPORTED_VERSION = '28.00'
SUPPORTED_IMAGE = next(k for k, p in SUPPORTED.items() if p['version'] == SUPPORTED_VERSION)
OLD_IMAGES = {}
KNOWN_672 = set()


def image_sha256(game):
    from prepare import parse_self, span
    elf, _, ph, _, _ = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    size = max(p['vaddr'] + p['memsz'] for p in loads)
    if size > 512 * 1024 * 1024:
        raise ValueError('executable image exceeds 512 MiB')
    image = bytearray(size)
    for p in loads:
        if p['filesz'] > p['memsz']:
            raise ValueError('segment file size exceeds memory size')
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def sfo_info(game):
    from prepare import sfo
    return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())


def problem(game, image_hash=None):
    try:
        info = sfo_info(game)
        image_hash = image_hash or image_sha256(game)
    except (OSError, ValueError, KeyError, IndexError, StopIteration):
        return 'unreadable', '?', '?'
    title, version = info.get('TITLE_ID', '?'), info.get('APP_VER', '?')
    p = SUPPORTED.get(image_hash)
    if not p:
        return ('other_title' if title not in BY_TITLE else 'wrong_eboot'), title, version
    if title != p['title'] or version != p['version']:
        return 'wrong_eboot', title, version
    for filename, expected in p['module_sha256'].items():
        path = Path(game) / 'sce_module' / filename
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            return 'wrong_modules', title, version
    return None


def profile(game, image_hash=None):
    image_hash = image_hash or image_sha256(game)
    if problem(game, image_hash):
        return None
    return dict(SUPPORTED[image_hash], variant='ufc4-known')


def explain(kind, title, version):
    return f'Found {title} {version}. ' + {
        'unreadable': 'Expected an extracted plaintext PS4 executable and param.sfo.',
        'other_title': 'This development profile is for UFC 4 CUSA14209 only.',
        'wrong_eboot': 'Executable or metadata differs from the measured 01.00/28.00 profiles.',
        'wrong_modules': 'The game libraries differ from the measured UFC 4 libraries.',
    }[kind]


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--variant']
    game = Path(args[0]) if args else Path('.')
    error = problem(game)
    if error:
        sys.exit(explain(*error))
    print('ufc4-known' if '--variant' in sys.argv else 'ufc4')
