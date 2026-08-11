# LBP3 shader collection and precompile TODO

## Implemented foundation

- LBP3 forces the shadPS4 SPIR-V/pipeline-key cache on even when the global option is disabled.
- Runtime data is stored under `cache/CUSA00063/runtime`; distributable data goes under
  `cache/CUSA00063/seed` or the app's `Contents/Resources/shader-cache-seed/CUSA00063` tree.
- Seed import is additive (`skip_existing`), and shutdown drains pending cache writes.
- SPIR-V buckets are namespaced by LBP3 app version, serialization schemas and exact
  `Shader::Profile` hash, so macOS and Windows use one layout without loading incompatible data.
- macOS KosmicKrisp/Mesa cache data is separated by Mac model and OS kernel version.
- Normal gameplay continues compiling unseen shaders and appending `.spv`, `.meta` and `.key`
  records to the writable runtime bucket.

## Bot (not implemented yet)

1. Drive every LBP3 story level and optional branch while recording newly discovered shaders and
   pipeline keys.
2. Make runs resumable and merge several corpora by content-addressed filename.
3. Export a manifest with title/update, build revision, schema versions, exact shader-profile ID,
   file hashes and shader/pipeline counts.
4. Add a precompile-only command that loads a compatible seed, creates all cached Vulkan pipelines
   without running a level, reports real progress, then saves the native driver cache.
5. Add a first-run stage similar to Steam:

   ```text
   Importing LBP3 shader cache
   Compiling SPIR-V shaders 1240/3500
   Compiling Vulkan pipelines 800/2100
   Saving driver cache
   Launching LittleBigPlanet 3
   ```

6. Store the original guest GCN compilation recipe. Current `.spv/.meta/.key` files are useful only
   for an exactly matching shader profile; they are not yet a universal cross-GPU seed.
7. Validate separate seeds on M4, M5, NVIDIA Windows and at least one AMD/Intel Vulkan driver.

Do not ship developer telemetry logs, save data, or a native cache from an unmatched GPU/driver.
