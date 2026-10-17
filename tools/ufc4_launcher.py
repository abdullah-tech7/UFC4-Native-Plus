# SPDX-License-Identifier: GPL-2.0-or-later
"""UFC4 development launcher. Game data stays in the user's extracted folder."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parent
sys.path[:0]=[str(ROOT/'scripts'),str(ROOT/'tools')]
from save_snapshots import create_backup, launch_guard

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def prepare_game(game):
    import game_check
    error=game_check.problem(game)
    if error:raise ValueError(game_check.explain(*error))
    image=game_check.image_sha256(game)
    profile=game_check.profile(game,image)
    cache=ROOT/'cache'/image[:16]
    marker=cache/'ready.json'
    expected={'image':image,'preparation':'ufc4-dev-1'}
    if marker.is_file():
        ready=json.loads(marker.read_text())
        valid=all(ready.get(k)==v for k,v in expected.items())
        valid=valid and all((cache/name).is_file() and digest(cache/name)==sha
                            for name,sha in ready.get('files',{}).items())
        if valid and len(ready.get('files',{}))==4:
            return cache,profile
    cache.mkdir(parents=True,exist_ok=True)
    from prepare import prepare
    from link_modules import link
    from content_profile import prepare as content
    from gen_import_names import aerolib_names,generate
    prepare(game,cache)
    link(game,cache,profile['modules'])
    content(game,cache)
    info=json.loads((cache/'link.json').read_text())
    lines=generate(info['unresolved_imports'],aerolib_names(ROOT/'share/aerolib.inl'))
    (cache/'import_names.txt').write_text('# UFC4 import names\n'+'\n'.join(lines)+'\n')
    expected['files']={name:digest(cache/name) for name in ('boot-linked.bin','link.json','import_names.txt','content.bin')}
    marker.write_text(json.dumps(expected,indent=2)+'\n')
    return cache,profile

def launch(args):
    for key in list(os.environ):
        if key.startswith('BB_'):del os.environ[key]
    game=args.game
    if game is None:
        config=ROOT/'game_path.txt'
        if not config.is_file():raise ValueError('Falta game_path.txt con la ruta de la carpeta extraída de UFC 4.')
        game=Path(config.read_text(encoding='utf-8-sig').strip())
    game=game.resolve()
    version=ROOT/'VERSION'
    print('Launcher version: '+(version.read_text(encoding='utf-8').strip() if version.is_file() else 'development'),flush=True)
    cache,profile=prepare_game(game)
    print(f"UFC4 prototype: {profile['title']} {profile['version']}; game={game}",flush=True)
    runtime=ROOT/'bin/ufc4-runtime.exe'
    measured=json.loads(runtime.with_suffix('.link.json').read_text())
    if digest(runtime)!=measured['output_sha256']:raise ValueError('El runtime difiere de su manifiesto de compilación.')
    if args.prepare_only:
        print('Preparation verified; game was not executed.',flush=True);return 0
    env=os.environ.copy()
    env.update({
        'BB_GAME_PROFILE':'ufc4','BB_GAME_VARIANT':'ufc4-known',
        'BB_CONFIG':str(ROOT/'ufc4.ini'),'BB_USER_DIR':str(ROOT/'user'),
        'BB_GPU_USER_DIR':str(ROOT/'user'),'BB_NEO':'0',
        'BB_HEAP_HOOKS':'0','BB_HEAP_SITES':'0','BB_FREE_CHECK':'0',
        'BB_DISCARD_HOOK':'0','BB_GNM_OBSERVE':'0','BB_PATCH_SHADERS':'0',
        'BB_READBACKS':'2','BB_READBACK_LINEAR':'1','BB_GPU_WRITE_TWINS':'0',
        # End-of-pipe/end-of-shader labels acknowledge completed GPU work.
        # The donor's default acknowledges recording, before execution finishes.
        'BB_HONEST_LABELS':'1',
        # Retain submitted commands until decoding, as the donor launcher does.
        'BB_COPY_GPU_BUFFERS':'1',
        # v0.1.5's coherent write windows and upload path, as its Windows
        # launcher uses. Native shader dither restores hair-card coverage.
        'BB_WRITE_WINDOW':'1','BB_WRITE_STEP':'1','BB_PREUPLOAD':'1',
        'BB_Z16S8_AS_D16':'1','BB_ALPHA_TO_MASK':'1','SHADPS4_BC_MODE':'3',
        'BB_VBLANK_HZ':'480','BB_FLIP_RATE':'0','BB_FPS_LIMIT':'100','BB_PRESENT_MODE':'Mailbox',
        'BB_INTERNAL_WIDTH':'1920','BB_INTERNAL_HEIGHT':'1080',
        'BB_WINDOW_SIZE':'1920x1080','BB_FULLSCREEN':'1',
        'BB_SKIP_MOVIES':'1','BB_SKIP_MOVIE_FILE':str(ROOT/'share/skip_movie.mkv'),
        'BB_FRAME_STATS':'1','BB_USER_NAME':'Player 1','BB_LANGUAGE':'20',
        'VK_LOADER_LAYERS_DISABLE':'*',
    })
    info=json.loads((cache/'link.json').read_text())
    command=[str(runtime),str(cache/'boot-linked.bin'),'--app0',str(game),
        '--user',env['BB_USER_DIR'],'--import-names',str(cache/'import_names.txt'),
        '--content-profile',str(cache/'content.bin'),'--tls-loads',str(info['fs_loads_patched'])]
    # The donor runtime defaults to a ten-second watchdog when this is omitted.
    # Pass zero explicitly for normal play, which disables that watchdog.
    command+=['--timeout',str(args.timeout)]
    print(f'Runtime timeout: {args.timeout} seconds (0 = disabled).',flush=True)
    print('Starting offline runtime.',flush=True)
    print('Renderer: UFC3 v0.1.5; skin and hair correction confirmed by the user. Graphics settings preserved.',flush=True)
    with launch_guard(ROOT/'user'):
        backup=create_backup(ROOT/'user/savedata',ROOT/'user/save_backups')
        print(f'Save backup: {backup if backup else "unchanged or no saves yet"}',flush=True)
        process=subprocess.Popen(command,cwd=ROOT,env=env,stdout=sys.stdout,stderr=sys.stderr)
        try:return process.wait(timeout=args.timeout+15 if args.timeout else None)
        except subprocess.TimeoutExpired:
            process.kill();process.wait();raise RuntimeError('La prueba excedió su límite de tiempo.')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path)
    parser.add_argument('--prepare-only',action='store_true')
    parser.add_argument('--timeout',type=int,default=0)
    parser.add_argument('--no-dialog',action='store_true')
    args=parser.parse_args()
    if not 0<=args.timeout<=180:parser.error('timeout debe estar entre 0 y 180 segundos')
    logs=ROOT/'logs';logs.mkdir(exist_ok=True)
    logfile=logs/(time.strftime('%Y%m%d-%H%M%S')+'-launcher.log')
    with logfile.open('w',encoding='utf-8',buffering=1) as stream:
        sys.stdout=sys.stderr=stream
        try:
            code=launch(args)
            if code and not (args.timeout and code==142):raise RuntimeError(f'El runtime terminó con código {code}.')
            print(f'Runtime exit: {code}',flush=True)
            return code
        except Exception as error:
            import traceback
            traceback.print_exc()
            if not args.no_dialog:
                ctypes.windll.user32.MessageBoxW(None,f'{error}\n\nRegistro: {logfile}',
                    'UFC 4 — prototipo de desarrollo',0x10)
            return 1

if __name__=='__main__':sys.exit(main())
