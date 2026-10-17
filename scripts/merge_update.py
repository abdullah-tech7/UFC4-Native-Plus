#!/usr/bin/env python3
"""Merge a PS4 update folder over its base game folder, as the console does.

PKG extractors usually leave the base game and its update side by side:
    <dir>/CUSA06534          (base 1.00)
    <dir>/CUSA06534-patch    (update 1.14; also -PATCH, -update, -UPDATE)
This makes <dir>/CUSA06534-merged next to them: every base file hard-linked (no extra disk
space; the base folder is never written), then every update file linked over it. On a volume
without hard links the files are copied. A marker records the source folders and sizes, so later
launches reuse the merged folder. Prints the folder to play: the merged one, or the given
folder unchanged when it has no update beside it (already merged).
"""
import json
import os
import shutil
import sys
from pathlib import Path

SUFFIXES = ('-patch', '-update', '_patch', '_update', ' patch', ' update')
MARKER = '.merged_update.json'


def split(folder: Path):
    """(base, update) for a base or an update folder given by the user; update may be None."""
    name = folder.name
    for suffix in SUFFIXES:
        if name.lower().endswith(suffix):
            base = folder.with_name(name[:-len(suffix)])
            return (base, folder) if (base / 'eboot.bin').is_file() else (folder, None)
    if name.lower().endswith('-merged'):
        return folder, None
    for entry in folder.parent.iterdir() if folder.parent.is_dir() else ():
        if entry.is_dir() and entry != folder and entry.name.lower() in (name.lower() + s for s in SUFFIXES):
            if (entry / 'eboot.bin').is_file():
                return folder, entry
    return folder, None


def files(root: Path):
    for dirpath, _, names in os.walk(root):
        for n in names:
            p = Path(dirpath) / n
            yield p.relative_to(root), p


def signature(base: Path, update: Path):
    total = lambda root: sum(p.stat().st_size for _, p in files(root))
    return {'base': str(base.resolve()), 'update': str(update.resolve()),
            'base_bytes': total(base), 'update_bytes': total(update)}


def place(source: Path, target: Path, stats):
    """target becomes a link to (or a copy of) source; an existing target is replaced, never
    written through (it may be a link to a base file)."""
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + '.merge-tmp')
    if temp.exists():
        temp.unlink()
    try:
        os.link(source, temp)
        stats['linked'] += 1
    except OSError:
        shutil.copy2(source, temp)
        stats['copied'] += 1
    os.replace(temp, target)


def merge(base: Path, update: Path) -> Path:
    merged = base.with_name(base.name + '-merged')
    sig = signature(base, update)
    marker = merged / MARKER
    try:
        if json.loads(marker.read_text(encoding='utf-8')) == sig:
            return merged
    except (OSError, ValueError):
        pass
    if merged.exists():
        shutil.rmtree(merged)
    stats = {'linked': 0, 'copied': 0}
    for rel, path in files(base):
        place(path, merged / rel, stats)
    for rel, path in files(update):
        place(path, merged / rel, stats)
    marker.write_text(json.dumps(sig, indent=1), encoding='utf-8')
    print(f'merge_update: {update.name} over {base.name} -> {merged} '
          f'({stats["linked"]} hard links, {stats["copied"]} copies)', file=sys.stderr)
    if stats['copied'] and not stats['linked']:
        print('merge_update: this drive has no hard links, so the game was copied '
              '(it uses as much space again)', file=sys.stderr)
    return merged


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: merge_update.py <game folder>')
    folder = Path(sys.argv[1])
    if not (folder / 'eboot.bin').is_file():
        raise SystemExit(f'No eboot.bin in {folder}')
    base, update = split(folder)
    print(merge(base, update) if update else folder)


if __name__ == '__main__':
    main()
