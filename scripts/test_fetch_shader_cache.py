#!/usr/bin/env python3
"""Build/run the real fetch decoder tests with the existing macOS core build flags.

No emulator or game is started. --reference-revision additionally builds the previous
parser/decoder under distinct symbols for differential checks and an optional benchmark.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shlex
import subprocess


def main():
    source = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=source / 'Build/x64-Clang-Release')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference-revision')
    parser.add_argument('--benchmark', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    target = 'CMakeFiles/shadps4.dir/src/shader_recompiler/frontend/fetch_shader.cpp.o'
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', target], text=True)
    command = shlex.split(commands.splitlines()[-1])
    compiler = command[0]
    flags = []
    it = iter(command[1:])
    for arg in it:
        if arg in ('-MT', '-MF', '-o', '-c'):
            next(it)
        elif arg not in ('-MD', '-MMD'):
            flags.append(arg)
    if args.sanitize:
        flags += ['-fsanitize=address', '-fno-omit-frame-pointer', '-g', '-O1']
    inputs = [
        ('fetch', source / 'src/shader_recompiler/frontend/fetch_shader.cpp', []),
        ('decode', source / 'src/shader_recompiler/frontend/decode.cpp', []),
        ('format', source / 'src/shader_recompiler/frontend/format.cpp', []),
        ('tests', source / 'tests/gcn/fetch_shader_cache.cpp',
         ['-DFETCH_SHADER_CACHE_REFERENCE'] if args.reference_revision else []),
    ]
    reference_hash = None
    if args.reference_revision:
        reference_hash = subprocess.check_output(
            ['git', '-C', str(source), 'rev-parse', args.reference_revision], text=True).strip()
        include = output / 'reference-include'
        for filename in ('decode.h', 'fetch_shader.cpp', 'decode.cpp'):
            content = subprocess.check_output([
                'git', '-C', str(source), 'show',
                f'{reference_hash}:src/shader_recompiler/frontend/{filename}'])
            path = (include / 'shader_recompiler/frontend/decode.h'
                    if filename == 'decode.h' else output / f'reference-{filename}')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        renamed = ['ParseFetchShader', 'GetFetchShaderCode', 'GcnCodeSlice', 'GcnDecodeContext',
                   'GetInstructionEncoding', 'HasAdditionalLiteral', 'IsVop3BEncoding']
        ref_flags = [f'-I{include}'] + [f'-D{name}={name}Reference' for name in renamed]
        inputs += [('reference-fetch', output / 'reference-fetch_shader.cpp', ref_flags),
                   ('reference-decode', output / 'reference-decode.cpp', ref_flags)]

    def compile_one(item):
        name, path, extra = item
        cmd = [compiler, *extra, *flags, '-c', str(path), '-o', str(output / f'{name}.o')]
        result = subprocess.run(cmd, cwd=build, capture_output=True, text=True)
        (output / f'{name}.log').write_text(result.stdout + result.stderr)
        return name, cmd, result.returncode

    with ThreadPoolExecutor(max_workers=3) as executor:
        results = list(executor.map(compile_one, inputs))
    (output / 'compile-commands.json').write_text(json.dumps(results, indent=2) + '\n')
    for name, _, code in results:
        if code:
            raise RuntimeError(f'Compilation failed: {name}, see {output / (name + ".log")}')

    executable = output / 'fetch-shader-cache-test'
    arch = flags[flags.index('-arch') + 1]
    link = [compiler, '-arch', arch, '-mmacosx-version-min=26.0',
            *[str(output / f'{name}.o') for name, _, _ in inputs],
            str(build / 'externals/spdlog/libspdlog.a'), str(build / '_deps/fmt-build/libfmt.a'),
            '-pthread', '-o', str(executable)]
    if args.sanitize:
        link += ['-fsanitize=address']
    result = subprocess.run(link, capture_output=True, text=True)
    (output / 'link.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'Link failed, see {output / "link.log"}')
    cmd = [str(executable)] + (['--benchmark'] if args.benchmark else [])
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    (output / 'result.log').write_text(result.stdout + result.stderr)
    (output / 'result.json').write_text(json.dumps({
        'returncode': result.returncode, 'reference_revision': reference_hash,
        'sanitizer': 'address' if args.sanitize else None, 'arch': arch,
        'command': cmd, 'game_launched': False,
    }, indent=2) + '\n')
    print(result.stdout, end='')
    print(result.stderr, end='')
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
