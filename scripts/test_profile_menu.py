#!/usr/bin/env python3
"""Headless regression using production objects and SDL virtual controllers; no live settings."""
import argparse, pathlib, re, shlex, subprocess
p=argparse.ArgumentParser(); p.add_argument('--build',type=pathlib.Path,required=True); p.add_argument('--output',type=pathlib.Path,required=True); a=p.parse_args()
b=a.build.resolve(); out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True); src=pathlib.Path(__file__).resolve().parents[1]
n=(b/'build.ninja').read_text(); block=n[n.index('build CMakeFiles/shadps4.dir/src/input/controller.cpp.o:'):].split('\nbuild ',1)[0]
f={k:shlex.split(v) for k,v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$',block,re.M)}
cc=re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$',(b/'CMakeCache.txt').read_text(),re.M)[1]
objects=[b/('CMakeFiles/shadps4.dir/src/'+s+'.cpp.o') for s in ['input/controller','input/profile_menu','core/user_manager']]
libs=[b/s for s in ['externals/sdl3/libSDL3.a','externals/libDear_ImGui.a','externals/freetype/libfreetype.a','externals/libpng/libpng16.a','_deps/zlib-build/libz.a','externals/spdlog/libspdlog.a','_deps/fmt-build/libfmt.a']]
libs=[x for x in libs if x.exists()]
frameworks=['-framework','CoreHaptics','-framework','UniformTypeIdentifiers']
for name in dict.fromkeys(re.findall(r'-framework ([A-Za-z0-9]+)',n)): frameworks+=['-framework',name]
cmd=[cc,*f['DEFINES'],*f['FLAGS'],*f['INCLUDES'],str(src/'tests/profile_menu.cpp'),*map(str,objects),*map(str,libs),*frameworks,'-Wl,-dead_strip','-o',str(out/'profile-menu-test')]
with (out/'compile.log').open('w') as log: subprocess.run(cmd,cwd=b,stdout=log,stderr=subprocess.STDOUT,check=True)
r=subprocess.run([str(out/'profile-menu-test')],capture_output=True,text=True,timeout=30); (out/'result.log').write_text(r.stdout+r.stderr); print(r.stdout+r.stderr); r.check_returncode()
