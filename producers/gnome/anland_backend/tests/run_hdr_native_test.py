#!/usr/bin/env python3
"""Use a configured, patched Mutter 51 tree; no desktop/device is started."""
import json, os, pathlib, shlex, subprocess, sys, tempfile
if len(sys.argv)!=2:
    raise SystemExit('Usage: run_hdr_native_test.py /absolute/path/to/mutter/build')
build=pathlib.Path(sys.argv[1]).resolve()
tests=pathlib.Path(__file__).resolve().parent
commands=json.loads((build/'compile_commands.json').read_text())
entry=next(x for x in commands if x['file'].endswith('/meta-anland-color.c'))
args=shlex.split(entry['command'])
if 'ANLAND_MUTTER_51' not in entry['command']:
    raise SystemExit('Native GPU harness requires Mutter 51; other APIs need matching build verification.')
output=pathlib.Path(tempfile.mkdtemp(prefix='anland-mutter-hdr-test-'))/'hdr-native-test'
args=[x for x in args if x not in ('-c','-MD')]
for flag in ('-MQ','-MF'):
    if flag in args:
        i=args.index(flag); del args[i:i+2]
i=args.index('-o'); del args[i:i+2]
args.remove(entry['file']) if entry['file'] in args else None
# The compdb may spell the source relative to its build directory.
args=[x for x in args if not x.endswith('/meta-anland-color.c')]
args += [str(tests/'hdr_native_test.c'),str(pathlib.Path(entry['directory'])/entry['file']),'-o',str(output),'-lEGL','-lm']
for path in [build/'clutter/clutter',build/'cogl/cogl',build/'mtk/mtk']:
    lib=next(path.glob('lib*.so.0.0.0'))
    args += [str(lib),'-Wl,-rpath,'+str(path)]
args += shlex.split(subprocess.check_output(['pkg-config','--libs','gio-2.0','graphene-gobject-1.0'],text=True))
env=os.environ.copy(); env['CCACHE_DISABLE']='1'; env['CCACHE_DIR']=str(output.parent/'ccache')
subprocess.run(args,cwd=entry['directory'],env=env,check=True)
subprocess.run([str(output)],env=env,check=True)
