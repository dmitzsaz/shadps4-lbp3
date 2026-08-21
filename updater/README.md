# shadPS4-lbp3 Runtime Updater

Build the minimal macOS updater with the current runtime bundle embedded:

```sh
./updater/build-updater.sh
```

The resulting updater bundle is always `updater-dist/shadPS4-lbp3.app`, as required by the
project's macOS bundle naming policy. Its window and bundle metadata identify it as
`shadPS4 Update`. Open it, select the separate `shadPS4-lbp3.app` that contains the game, and
the updater replaces the launcher, core, PartyChat, Vulkan libraries, ICD JSON, Info.plist, and
license files. It verifies every SHA-256 and refreshes the ad-hoc bundle signature while
preserving `Resources/Game`, `Resources/Addons`, `dry.db`, and saves.

The build rejects obsolete PartyChat binaries that do not provide both the standalone
`serve` command and the archive `index` command.
