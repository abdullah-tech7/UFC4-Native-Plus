# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded local draw/material diagnostics using the renderer's frame recorder."""
import os
import threading
import time


class MaterialCapture:
    def __init__(self,logs,env):
        self.directory=logs/('materials-'+time.strftime('%Y%m%d-%H%M%S')+f'-{os.getpid()}')
        self.directory.mkdir(parents=True,exist_ok=True)
        self.gpu_directory=self.directory/'gpu-textures'
        self.gpu_directory.mkdir()
        self.trigger=self.directory/'capture.request'
        self.stopped=threading.Event()
        self.worker=None
        env.update(BB_CAPTURE_DIR=str(self.directory),BB_CAPTURE_TRIGGER=str(self.trigger),
                   BB_CAPTURE_ON_FLIP='1',BB_IMAGE_SYNC_LOG='1',
                   BB_UFC4_TEXTURE_PROBE_DIR=str(self.gpu_directory),
                   BB_UFC4_TEXTURE_PROBE_PS='7388baf7')

    def _trim_and_count(self):
        # The first eight startup frames previously caught only the avatar
        # editor and spent the GPU budget before the fight. Wait for NPC skin.
        frames=sorted(self.directory.glob('frame_*.txt'))
        targets=[]
        for path in frames:
            if path.stat().st_size<=32*1024*1024 and '000000007388baf7' in path.read_text(errors='replace'):
                targets.append(path)
        keep=set(targets[-2:])
        remaining=[p for p in frames if p not in keep]
        total=sum(p.stat().st_size for p in frames)
        while remaining and (len(frames)>8 or total>=24*1024*1024):
            old=remaining.pop(0)
            if old.resolve().parent!=self.directory.resolve():raise OSError('Capture escaped its directory')
            total-=old.stat().st_size;old.unlink();frames.remove(old)
        return len(keep),total

    def start(self,process):
        def record():
            try:
                # Wait at most 90 minutes, retaining eight frames / ~32 MiB.
                # Stop after two frames containing the actual opponent shader.
                for _ in range(180):
                    if self.stopped.wait(30) or process.poll() is not None:return
                    opponents,total=self._trim_and_count()
                    if opponents>=2 or total>=32*1024*1024:return
                    self.trigger.touch(exist_ok=True)
            except OSError as error:
                print(f'Material capture stopped: {error}',flush=True)
        self.worker=threading.Thread(target=record,name='ufc4-material-capture',daemon=True)
        self.worker.start()
        print(f'Material diagnostics: {self.directory} (wait for opponent skin, retain 8 frames; GPU maps up to 48 images/128 MiB).',flush=True)

    def close(self):
        self.stopped.set()
        if self.worker:self.worker.join(timeout=1)
        try:self.trigger.unlink(missing_ok=True)
        except OSError:pass
