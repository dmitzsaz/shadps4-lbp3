#!/usr/bin/env python3
"""Test production profile UI/SDL ownership with virtual pads; never read live settings."""
import argparse
import json
import pathlib
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
parser.add_argument('--backend-source', type=pathlib.Path,
                    help='Optional previous backend source for regression reproduction')
args = parser.parse_args()
build, out = args.build.resolve(), args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = pathlib.Path(__file__).resolve().parents[1]
ninja = (build / 'build.ninja').read_text()
block = ninja[ninja.index('build CMakeFiles/shadps4.dir/src/input/controller.cpp.o:'):].split('\nbuild ', 1)[0]
flags = {k: shlex.split(v) for k, v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M)[1]
objects = [build / ('CMakeFiles/shadps4.dir/src/' + s + '.cpp.o') for s in (
    'input/controller', 'input/profile_menu', 'core/user_manager',
    'imgui/renderer/font_stack', 'imgui/renderer/font_data')]
libraries = [build / s for s in (
    'externals/sdl3/libSDL3.a', 'externals/libDear_ImGui.a', 'externals/freetype/libfreetype.a',
    'externals/libpng/libpng16.a', '_deps/zlib-build/libz.a',
    'externals/spdlog/libspdlog.a', '_deps/fmt-build/libfmt.a')]
frameworks = ['-framework', 'CoreHaptics', '-framework', 'UniformTypeIdentifiers']
for name in dict.fromkeys(re.findall(r'-framework ([A-Za-z0-9]+)', ninja)):
    frameworks += ['-framework', name]
extra = []
if args.backend_source:
    extra = [f'-DSHADPS4_SDL_BACKEND_SOURCE="{args.backend_source.resolve()}"',
             '-I' + str(source / 'src/imgui/renderer')]
command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'], *extra,
           str(source / 'tests/profile_menu.cpp'), str(source / 'tests/sdl_gamepad_ownership.cpp'),
           *map(str, objects), *map(str, libraries), *frameworks, '-Wl,-dead_strip',
           '-o', str(out / 'profile-menu-test')]
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
with (out / 'compile.log').open('w') as log:
    subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
result = subprocess.run([str(out / 'profile-menu-test'), str(out / 'profile-menu.png')],
                        capture_output=True, text=True, timeout=30)
(out / 'result.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr)
result.check_returncode()
