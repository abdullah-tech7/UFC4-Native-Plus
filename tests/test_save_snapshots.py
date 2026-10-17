# SPDX-License-Identifier: GPL-2.0-or-later
"""Filesystem and process-lock regressions; uses temporary fake saves, never a game."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import save_snapshots as saves


class Snapshots(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ufc-save-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'user/savedata'
        self.backups = self.root / 'user/save_backups'
        self.slot = self.source / '1/CUSA14209/CAREERSAVE0/DATA'
        self.slot.parent.mkdir(parents=True)
        self.slot.write_bytes(b'career progress 1')
        (self.source / 'profile').write_bytes(b'profile data')

    def test_verified_copy_dedup_and_changed_progress(self):
        original = saves.inventory(self.source)
        first = saves.create_backup(self.source, self.backups)
        manifest = json.loads((first / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['files'], original)
        self.assertEqual(saves.inventory(first / 'files'), original)
        self.assertIsNone(saves.create_backup(self.source, self.backups))
        self.slot.write_bytes(b'career progress 2')
        second = saves.create_backup(self.source, self.backups)
        self.assertNotEqual(first, second)
        self.assertEqual((first / 'files' / self.slot.relative_to(self.source)).read_bytes(), b'career progress 1')
        self.assertEqual(saves.inventory(second / 'files'), saves.inventory(self.source))
        self.assertEqual(len(list(self.backups.glob('snapshot-*'))), 2)

    def test_incomplete_copy_is_never_published_and_prior_copy_survives(self):
        first = saves.create_backup(self.source, self.backups)
        before = saves.inventory(first)
        self.slot.write_bytes(b'career progress 2')
        copier = saves.shutil.copytree

        def changing_copy(*args, **kwargs):
            result = copier(*args, **kwargs)
            self.slot.write_bytes(b'changed while copying')
            return result

        with patch.object(saves.shutil, 'copytree', side_effect=changing_copy):
            with self.assertRaises(RuntimeError):
                saves.create_backup(self.source, self.backups)
        self.assertEqual(saves.inventory(first), before)
        self.assertEqual(len(list(self.backups.glob('snapshot-*'))), 1)
        self.assertEqual(len(list(self.backups.glob('.pending-*'))), 1)

    def test_corrupt_previous_copy_does_not_suppress_new_backup(self):
        first = saves.create_backup(self.source, self.backups)
        (first / 'files/profile').write_bytes(b'damaged')
        self.assertIsNotNone(saves.create_backup(self.source, self.backups))
        self.assertEqual(len(list(self.backups.glob('snapshot-*'))), 2)

    def test_rejects_recursive_or_overlapping_destinations(self):
        for destination in [self.source, self.source / 'backups', self.source.parent]:
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                saves.create_backup(self.source, destination)

    def test_empty_saves_do_not_create_backup(self):
        missing = self.root / 'missing'
        self.assertIsNone(saves.create_backup(missing, self.backups))
        self.assertFalse(self.backups.exists())

    def test_session_lock_blocks_second_process_and_releases_after_error(self):
        code = """import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from save_snapshots import launch_guard
try:
    with launch_guard(Path(sys.argv[2])): pass
except RuntimeError:
    sys.exit(23)
"""
        command = [sys.executable, '-B', '-c', code, str(Path(saves.__file__).parent), str(self.root / 'user')]
        with self.assertRaisesRegex(ValueError, 'test error'):
            with saves.launch_guard(self.root / 'user'):
                child = subprocess.run(command, capture_output=True, timeout=10)
                self.assertEqual(child.returncode, 23, child.stderr)
                raise ValueError('test error')
        child = subprocess.run(command, capture_output=True, timeout=10)
        self.assertEqual(child.returncode, 0, child.stderr)


if __name__ == '__main__':
    unittest.main()
