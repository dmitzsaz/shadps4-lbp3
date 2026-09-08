<!-- SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# shadPS4 LBP3 launcher

The macOS bundle uses the Rust launcher as `Contents/MacOS/shadps4` and keeps the emulator core as
`Contents/MacOS/shadps4-core`. Invocations with command-line arguments are forwarded to the core;
an invocation without arguments opens the launcher window. `--launcher-ui -g <eboot.bin>` is the
internal testing form that opens the UI and preselects the given game instead of forwarding the
arguments. After a successful GUI launch, the launcher exits and leaves only the game in the Dock.
The core changes its LaunchServices display name to the title loaded from the game's `param.sfo`
after SDL registers the application.

The GUI starts the core through a per-user interactive LaunchAgent with `KeepAlive` disabled. This
places the game in its own macOS process coalition before the launcher exits; otherwise
LaunchServices retains the dead parent as `exited-with-subordinates` and shows a second
`shadPS4-lbp3` background item in the Dock. The inactive agent is removed and recreated on the next
launch, and it never restarts a game after a normal exit or crash.

Capture tools use `--launcher-launchd -g <valid eboot.bin>` plus the core arguments to start the
same Interactive LaunchAgent without opening the GUI. The launcher waits for the game's exit;
the launcher-only switch is consumed before forwarding. A missing or invalid game is rejected.

The built-in compatibility patches target `APP_VER 01.26`. If selected patches do not match the
chosen game's `param.sfo`, the launcher warns before starting and passes all patch switches as
disabled for that run without overwriting the saved choices. LBP Online performs the same
pre-launch warning when PartyChat is unavailable. PartyChat is probed synchronously again when the
user continues, so starting it while the warning is open is picked up by the game launch.

macOS keeps the Dock label of executables inside an `.app` pinned to that bundle's on-disk display
name. To make the label genuinely follow `param.sfo`, the launcher creates hard links for the signed
core and its Vulkan runtime under `~/Library/Caches/shadPS4/lbp3-runtime`, names the executable after
the game, and launches it there. This does not duplicate the large binaries or modify the app's
signature; copying is used only when the cache and the app are on different filesystems.

For the installed LBP3 bundle, keep the app in `/Applications/shadPS4-lbp3.app`, select its bundled
game and DLC, and keep capture data in `~/Library/Application Support/shadPS4/performance-captures`.
The runtime executable in Caches must not depend on resources or capture paths under Desktop.
When the bundle contains a game, launcher initialization does not probe the legacy external
Desktop game path. Replay tools explicitly select the current bundle's game and DLC, and copy
the input movie into the new capture directory before starting the core.

An optional bundled game is discovered at:

```text
shadPS4-lbp3.app/Contents/Resources/Game/CUSA00063/eboot.bin
```

The launcher also searches recursively below `Contents/Resources/Game` and `Contents/Game`. An
external `eboot.bin` can always be selected even when a bundled game exists.

Bundled add-ons use shadPS4's normal add-on root layout:

```text
shadPS4-lbp3.app/Contents/Resources/Addons/CUSA00063/<DLC>/sce_sys/param.sfo
```

PartyChat automatically discovers an indexed Dry Archive catalog at:

```text
shadPS4-lbp3.app/Contents/Resources/dry.db
```

The package script verifies or builds PartyChat's persistent indexes before signing the bundle, so
the signed resource stays read-only while the game is running. An explicit
`--lbp-archive-db`/`PARTYCHAT_LBP_ARCHIVE_DB` value still overrides this bundled default.

The package script signs its staged bundle before installation. Do not run a recursive forced
re-sign on the installed runtime just to launch it: its core may be hard-linked into Caches, and
ad-hoc signing identifies it by a code hash. Moving the complete bundle preserves that identity.
When updating only the launcher, seal the outer bundle without `--deep` and verify that the
signed core and driver hashes are unchanged. None of these steps resets or grants macOS consent.

Build the canonical bundle with:

```sh
scripts/package-lbp3-macos.sh
```

To include game, DLC, or Dry catalog files before signing, set `SHADPS4_BUNDLED_GAME_DIR`,
`SHADPS4_BUNDLED_ADDONS_DIR`, and/or `SHADPS4_BUNDLED_DRY_DB`. The game variable can point either at
`CUSA00063` itself or at its parent; the add-ons variable follows the same convention. The Dry
variable points directly to `dry.db`.
