# LBP3 shader-cache seed

The release bundle may ship read-only cache files below this directory. At startup shadPS4 copies
only missing seed files into the user's writable runtime cache; newer runtime files are never
overwritten.

```text
CUSA00063/
  spirv/v1/<exact-profile>/       # .spv, .meta, .key and profile.bin
  native/mesa/v1/<mac-os-host>/   # KosmicKrisp/Mesa translated Metal cache
  native/vulkan/v1/<gpu-driver>/  # reserved for a future native Vulkan cache
  native/metal/v1/<gpu-driver>/   # reserved for a future explicit Metal archive
```

`<exact-profile>` includes the game version, serialization schemas and a hash of the complete
shadPS4 shader profile. A seed built for another profile is not imported.
