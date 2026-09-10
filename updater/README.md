# shadPS4-lbp3 Runtime Updater

Build the minimal macOS updater with the current runtime bundle embedded:

```sh
./updater/build-updater.sh
```

The default runtime source is `/Applications/shadPS4-lbp3.app`; override it with
`SHADPS4_RUNTIME_BUNDLE` when packaging another validated installation.
The resulting updater bundle is always `updater-dist/shadPS4-update.app`. Open it, select the
separate `shadPS4-lbp3.app` that contains the game, and the updater replaces the launcher, core,
PartyChat, Vulkan libraries, ICD JSON, Info.plist, BuildInfo.json, and license files. It verifies every SHA-256
and refreshes the ad-hoc bundle signature while preserving `Resources/Game`,
`Resources/Addons`, `dry.db`, and saves.

Signed runtime binaries are copied byte-for-byte and verified, without re-signing
them. Only the rebuilt updater and the enclosing bundle seals are signed.
The complete source bundle and updater resource seal are verified because the
launcher cannot be validated separately from its original app resources. The
ICD JSON's generic signature is retained in extended attributes.
Version 1.8 adds one shared controller/profile menu for hotplug, Guide and F2,
with session-only controller reassignment and occupied-profile swaps. It also carries the validated indexed QuadList and attachmentless render-area
fixes, alongside controller replay and bounded performance diagnostics. The
runtime uses KosmicKrisp/Metal on Apple Silicon and requires macOS 26 or newer;
the updater UI itself can run on macOS 14 or newer.

The build rejects obsolete PartyChat binaries that do not provide both the standalone
`serve` command and the archive `index` command.

Package for another Mac with Apple's archive format so the ICD signature survives:

```sh
ditto -c -k --sequesterRsrc --keepParent \
  updater-dist/shadPS4-update.app shadPS4-update-v1.8.zip
```

Validate an updater, including an actual update and a second idempotent update,
without running the game or modifying the installed application:

```sh
python3 updater/test-updater.py --updater /path/shadPS4-update.app \
  --runtime /Applications/shadPS4-lbp3.app --output /tmp/updater-validation.json
```
