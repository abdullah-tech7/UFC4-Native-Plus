"""Check bounded/cancellable local diagnostics without a runtime or real waits."""
from pathlib import Path
import sys
import tempfile
sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parent.parent/'tools'))
from ufc4_material_capture import MaterialCapture

class FastEvent:
    def __init__(self):self.done=False;self.waits=[]
    def wait(self,seconds):self.waits.append(seconds);return self.done
    def set(self):self.done=True

class Process:
    def __init__(self,code=None):self.code=code
    def poll(self):return self.code

class Trigger:
    def __init__(self):self.count=0
    def touch(self,exist_ok):assert exist_ok;self.count+=1
    def unlink(self,missing_ok):assert missing_ok

with tempfile.TemporaryDirectory(prefix='materials-test-',dir=Path(sys.argv[1])) as folder:
    root=Path(folder)
    for case,code,stopped,large,expected in (
        ('normal',None,False,False,180),('exited',0,False,False,0),
        ('cancelled',None,True,False,0),('size-limit',None,False,True,180)):
        env={};capture=MaterialCapture(root/case,env)
        assert env['BB_CAPTURE_ON_FLIP']=='1'
        assert env['BB_UFC4_TEXTURE_PROBE_PS']=='7388baf7'
        assert Path(env['BB_CAPTURE_DIR']).is_relative_to(root)
        capture.stopped=FastEvent();capture.trigger=Trigger()
        if stopped:capture.stopped.set()
        if large:
            with (capture.directory/'frame_0.txt').open('wb') as file:file.truncate(32*1024*1024)
        capture.start(Process(code));capture.worker.join(timeout=1)
        assert not capture.worker.is_alive()
        assert capture.trigger.count==expected
        assert all(t==30 for t in capture.stopped.waits)
        capture.close()
    env={};capture=MaterialCapture(root/'targeted',env)
    for i in range(12):(capture.directory/f'frame_{i:02}.txt').write_text('CAF 000000007f438824')
    assert capture._trim_and_count()[0]==0
    assert len(list(capture.directory.glob('frame_*.txt')))==8
    for i in (20,21):(capture.directory/f'frame_{i}.txt').write_text('NPC 000000007388baf7')
    assert capture._trim_and_count()[0]==2
    capture.stopped=FastEvent();capture.trigger=Trigger()
    capture.start(Process());capture.worker.join(timeout=1)
    assert capture.trigger.count==0
    capture.close()
print('PASS: 8-frame retention, opponent targeting, exit, cancellation, 90-minute ceiling; no runtime/game started')
