# mesa-kosmickrisp

Provides CMake build logic for compiling KosmicKrisp and the Vulkan loader on macOS, for either ARM64 or x86_64, on an ARM64 host.

## Building

Install the required dependencies, e.g. with brew and pip:
```
brew install cmake meson ninja pkg-config llvm spirv-tools spirv-llvm-translator
pip3 install --break-system-packages mako packaging pyyaml
```

Then build using CMake, replacing the architecture with your desired target:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --parallel$(sysctl -n hw.ncpu)
```

## LBP3 upstream baseline

Mesa is pinned to `169cbbf12a68779483c8a02e1f97f35b5e2f18c5` (2026-09-04).
The CMake configure step applies the following patches in order:

- `lbp3-metal-fixes.patch`: retain render-only Metal command buffers between
  passes, preserve ordering when compute intervenes, release allocator backing
  on explicit Vulkan resource-release resets, and request resource-alias
  visibility only when occlusion queries require it.
- `lbp3-native-effects.patch`: retain the linear triangle-fan restart expansion
  used by LBP3. Upstream now supplies provoking-vertex support and the common
  topology output-order helpers, so those changes are no longer duplicated.
- `lbp3-host-import.patch`: reject failed Metal host-pointer imports before
  dereferencing the result. Imported-buffer offsets are already fixed upstream.
- `lbp3-metal-library-cache.patch`: reuse identical Metal stage libraries within
  each compiler/device. The key includes source, language version, math mode and
  floating-point options. Retention is bounded by an NSCache (4096 entries and
  64 MiB of source text); callers keep their owned library references. This is
  an in-process cache, not a replacement for Metal's persistent shader cache.

- `lbp3-command-pool-cache.patch`: initialize the free command-BO count, reset
  it when trimming the pool, and retain at most 32 BOs. A poisoned Vulkan
  allocator regression test reproduces the old uninitialized count; GPU buffer
  updates exercise reuse and trimming without launching the game.

- `lbp3-precise-timestamps.patch`: opt-in precise compute encoder boundaries
  for bounded GPU timestamp diagnostics. Normal launches retain the default
  encoder behavior; enable only with `MESA_KK_PRECISE_COMPUTE_TIMESTAMPS=1`.

These patches retain the upstream barrier/framebuffer-fetch, shader constant
storage, triangle-merge workaround, render-pass resolve, timestamp, and debug
label improvements. Changing the Mesa revision requires rebasing the patch set;
do not silently discard a patch that fails to apply.

## Command-pool regression

Run `tests/run_command_pool_regression.py` with `--mesa-build` pointing to
`build/externals/mesa/build-target`, `--loader-dir` to the bundled Vulkan loader's
directory, `--icd` to the ICD JSON for the library under test, and `--output` to
the test artifact directory. It uses the Mesa compilation flags and a real
Vulkan device. A poisoned allocation must create an empty pool with count zero;
three update/readback cycles check the cache bound, reuse, trim, and reuse after
trim. The previous library fails the initial-count check.
