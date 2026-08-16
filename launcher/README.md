<!-- SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# shadPS4 LBP3 launcher

The macOS bundle uses the Rust launcher as `Contents/MacOS/shadps4` and keeps the emulator core as
`Contents/MacOS/shadps4-core`. Invocations with command-line arguments are forwarded to the core;
an invocation without arguments opens the launcher window. After a successful GUI launch, the
launcher exits and leaves only the game in the Dock. The core changes its LaunchServices display
name to the title loaded from the game's `param.sfo` after SDL registers the application.

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

After adding or changing files inside a signed bundle, sign it again:

```sh
codesign --force --deep --sign - /path/to/shadPS4-lbp3.app
```

Build the canonical bundle with:

```sh
scripts/package-lbp3-macos.sh
```

To include game or DLC files before signing, set `SHADPS4_BUNDLED_GAME_DIR` and/or
`SHADPS4_BUNDLED_ADDONS_DIR`. The game variable can point either at `CUSA00063` itself or at its
parent; the add-ons variable follows the same convention.
