# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the installed launcher with a fake child; never runs a game/GPU."""
from pathlib import Path
import contextlib
import importlib.util
import io
import os
import sys
from types import SimpleNamespace
sys.dont_write_bytecode=True
package=Path(sys.argv[1]).resolve()
spec=importlib.util.spec_from_file_location('ufc4_packaged_launcher',package/'launcher.py')
launcher=importlib.util.module_from_spec(spec);spec.loader.exec_module(launcher)
cache=next(p.parent for p in (package/'cache').glob('*/ready.json'))
launcher.prepare_game=lambda game:(cache,{'title':'CUSA14209','version':'28.00'})
captured=[]
backups=[]
launcher.launch_guard=lambda user:contextlib.nullcontext()
launcher.create_backup=lambda source,destination:backups.append((source,destination))
class FakeChild:
    def __init__(self,command,*,cwd,env,stdout,stderr):
        captured.append((command,cwd,env))
    def wait(self,timeout):assert timeout is None;return 0
original=launcher.subprocess.Popen
os.environ['BB_ONLINE_SERVER']='must-not-be-used.invalid'
os.environ['BB_ALPHA_TO_MASK']='0';os.environ['SHADPS4_BC_MODE']='0'
launcher.subprocess.Popen=FakeChild
try:
    with contextlib.redirect_stdout(io.StringIO()):
        assert launcher.launch(SimpleNamespace(game=None,prepare_only=False,timeout=0))==0
finally:launcher.subprocess.Popen=original
assert len(captured)==1
assert backups==[(package/'user/savedata',package/'user/save_backups')]
command,cwd,env=captured[0]
assert Path(command[0])==package/'bin/ufc4-runtime.exe' and cwd==package
assert command[-2:]==['--timeout','0']
assert 'BB_ONLINE_SERVER' not in env
for key,value in {'BB_ALPHA_TO_MASK':'1','SHADPS4_BC_MODE':'3','BB_FPS_LIMIT':'100',
    'BB_LANGUAGE':'20','BB_INTERNAL_WIDTH':'1920','BB_INTERNAL_HEIGHT':'1080',
    'BB_FULLSCREEN':'1','BB_READBACKS':'2','BB_READBACK_LINEAR':'1','BB_GPU_WRITE_TWINS':'0',
    'BB_PREUPLOAD':'1','BB_WRITE_WINDOW':'1','BB_WRITE_STEP':'1','BB_NEO':'0',
    'BB_PATCH_SHADERS':'0','BB_HEAP_HOOKS':'0','BB_HONEST_LABELS':'1'}.items():
    assert env[key]==value,(key,env.get(key))
assert env['BB_USER_DIR']==env['BB_GPU_USER_DIR']==str(package/'user')
assert 'BB_DRAW_PIPE' not in env and 'BB_PREP_WORKERS' not in env
assert 'BB_UFC4_TEXTURE_PROBE_DIR' not in env
captured.clear();backups.clear()
with contextlib.redirect_stdout(io.StringIO()):
    assert launcher.launch(SimpleNamespace(game=None,prepare_only=True,timeout=0))==0
assert not captured and not backups
def failed_backup(*args):raise OSError('simulated save backup failure')
launcher.create_backup=failed_backup
try:
    with contextlib.redirect_stdout(io.StringIO()):
        launcher.launch(SimpleNamespace(game=None,prepare_only=False,timeout=0))
    raise AssertionError('backup failure must prevent launching the child')
except OSError as error:assert str(error)=='simulated save backup failure'
assert not captured
print('PASS: installed launcher selects new renderer, BC/alpha/coherency settings, same save profile, Spanish/1080p/100 FPS, no timeout or inherited online server; fake child only')
