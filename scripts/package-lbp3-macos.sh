#!/bin/zsh
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

SCRIPT_DIR=${0:A:h}
PROJECT_ROOT=${SCRIPT_DIR:h}
BUNDLE_PARENT=$PROJECT_ROOT
POLICY_SEARCH_ROOT=$PROJECT_ROOT
while [[ "$POLICY_SEARCH_ROOT" != "/" ]]; do
    if [[ -f "$POLICY_SEARCH_ROOT/AGENTS.md" ]]; then
        BUNDLE_PARENT=$POLICY_SEARCH_ROOT
        break
    fi
    POLICY_SEARCH_ROOT=${POLICY_SEARCH_ROOT:h}
done
BUNDLE_PATH=${1:-"$BUNDLE_PARENT/shadPS4-lbp3.app"}
BUNDLE_NAME=${BUNDLE_PATH:t}
BUILD_DIR=${SHADPS4_BUILD_DIR:-"$PROJECT_ROOT/Build/x64-Clang-Release"}
LAUNCHER_BINARY="$PROJECT_ROOT/launcher/target/release/shadps4-launcher"
CORE_BINARY="$BUILD_DIR/shadps4"

if [[ "$BUNDLE_NAME" != "shadPS4-lbp3.app" ]]; then
    print -u2 "Refusing to package a bundle named '$BUNDLE_NAME'; it must be shadPS4-lbp3.app"
    exit 2
fi

if [[ -z ${SHADPS4_BUNDLED_GAME_DIR:-} && \
    -f "$BUNDLE_PATH/Contents/Resources/Game/CUSA00063/eboot.bin" ]]; then
    print -u2 "Refusing to replace a bundle that contains a game. Re-run with "
    print -u2 "SHADPS4_BUNDLED_GAME_DIR='$BUNDLE_PATH/Contents/Resources/Game' to preserve it."
    exit 2
fi
if [[ -z ${SHADPS4_BUNDLED_ADDONS_DIR:-} && \
    -d "$BUNDLE_PATH/Contents/Resources/Addons/CUSA00063" ]]; then
    print -u2 "Refusing to replace a bundle that contains DLC. Re-run with "
    print -u2 "SHADPS4_BUNDLED_ADDONS_DIR='$BUNDLE_PATH/Contents/Resources/Addons' to preserve it."
    exit 2
fi

for required in "$CORE_BINARY" "$BUILD_DIR/libvulkan.dylib" \
    "$BUILD_DIR/libvulkan_kosmickrisp.dylib" "$BUILD_DIR/kosmickrisp_mesa_icd.json"; do
    if [[ ! -f "$required" ]]; then
        print -u2 "Missing build artifact: $required"
        exit 1
    fi
done

cargo build --release --manifest-path "$PROJECT_ROOT/launcher/Cargo.toml"

STAGE_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/shadps4-lbp3-package.XXXXXX")
trap 'rm -rf -- "$STAGE_ROOT"' EXIT
STAGED_BUNDLE="$STAGE_ROOT/shadPS4-lbp3.app"
STAGED_MACOS="$STAGED_BUNDLE/Contents/MacOS"
STAGED_RESOURCES="$STAGED_BUNDLE/Contents/Resources"
STAGED_CORE_BUNDLE="$STAGED_BUNDLE/Contents/Helpers/shadPS4-lbp3.app"
STAGED_CORE_MACOS="$STAGED_CORE_BUNDLE/Contents/MacOS"
mkdir -p "$STAGED_MACOS" "$STAGED_CORE_MACOS" "$STAGED_RESOURCES/Game" \
    "$STAGED_RESOURCES/Addons"

PARTYCHAT_SOURCE=${SHADPS4_PARTYCHAT_BIN:-}
if [[ -z "$PARTYCHAT_SOURCE" && -f "$BUNDLE_PATH/Contents/MacOS/partychat" ]]; then
    PARTYCHAT_SOURCE="$BUNDLE_PATH/Contents/MacOS/partychat"
fi
if [[ -n "$PARTYCHAT_SOURCE" ]]; then
    if [[ ! -f "$PARTYCHAT_SOURCE" ]]; then
        print -u2 "PartyChat binary does not exist: $PARTYCHAT_SOURCE"
        exit 1
    fi
    cp "$PARTYCHAT_SOURCE" "$STAGED_MACOS/partychat"
    chmod 755 "$STAGED_MACOS/partychat"
fi

cp "$LAUNCHER_BINARY" "$STAGED_MACOS/shadps4"
cp "$CORE_BINARY" "$STAGED_CORE_MACOS/shadps4-core"
cp "$BUILD_DIR/libvulkan.dylib" "$STAGED_CORE_MACOS/libvulkan.dylib"
cp "$BUILD_DIR/libvulkan_kosmickrisp.dylib" "$STAGED_CORE_MACOS/libvulkan_kosmickrisp.dylib"
cp "$BUILD_DIR/kosmickrisp_mesa_icd.json" "$STAGED_CORE_MACOS/kosmickrisp_mesa_icd.json"
cp "$PROJECT_ROOT/launcher/Info.plist" "$STAGED_BUNDLE/Contents/Info.plist"
cp "$PROJECT_ROOT/launcher/GameInfo.plist" "$STAGED_CORE_BUNDLE/Contents/Info.plist"
cp "$PROJECT_ROOT/LICENSE" "$STAGED_RESOURCES/LICENSE-shadPS4.txt"
chmod 755 "$STAGED_MACOS/shadps4" "$STAGED_CORE_MACOS/shadps4-core"

if [[ -f "$BUNDLE_PATH/Contents/Resources/LICENSE-PartyChat.txt" ]]; then
    cp "$BUNDLE_PATH/Contents/Resources/LICENSE-PartyChat.txt" \
        "$STAGED_RESOURCES/LICENSE-PartyChat.txt"
fi

if [[ -n ${SHADPS4_BUNDLED_GAME_DIR:-} ]]; then
    GAME_SOURCE=$SHADPS4_BUNDLED_GAME_DIR
    if [[ -f "$GAME_SOURCE/eboot.bin" ]]; then
        /usr/bin/ditto "$GAME_SOURCE" "$STAGED_RESOURCES/Game/CUSA00063"
    elif [[ -f "$GAME_SOURCE/CUSA00063/eboot.bin" ]]; then
        /usr/bin/ditto "$GAME_SOURCE/CUSA00063" "$STAGED_RESOURCES/Game/CUSA00063"
    else
        print -u2 "Bundled game source must contain CUSA00063/eboot.bin: $GAME_SOURCE"
        exit 1
    fi
fi

if [[ -n ${SHADPS4_BUNDLED_ADDONS_DIR:-} ]]; then
    ADDONS_SOURCE=$SHADPS4_BUNDLED_ADDONS_DIR
    if [[ -d "$ADDONS_SOURCE/CUSA00063" ]]; then
        /usr/bin/ditto "$ADDONS_SOURCE/CUSA00063" "$STAGED_RESOURCES/Addons/CUSA00063"
    elif [[ "${ADDONS_SOURCE:t}" == "CUSA00063" && -d "$ADDONS_SOURCE" ]]; then
        /usr/bin/ditto "$ADDONS_SOURCE" "$STAGED_RESOURCES/Addons/CUSA00063"
    else
        print -u2 "Bundled add-on source must contain a CUSA00063 directory: $ADDONS_SOURCE"
        exit 1
    fi
fi

/usr/bin/plutil -lint "$STAGED_BUNDLE/Contents/Info.plist"
/usr/bin/plutil -lint "$STAGED_CORE_BUNDLE/Contents/Info.plist"
/usr/bin/codesign --force --deep --sign - "$STAGED_CORE_BUNDLE"
/usr/bin/codesign --force --deep --sign - "$STAGED_BUNDLE"
/usr/bin/codesign --verify --deep --strict "$STAGED_BUNDLE"

mkdir -p "${BUNDLE_PATH:h}"
if [[ -e "$BUNDLE_PATH" || -L "$BUNDLE_PATH" ]]; then
    rm -rf -- "$BUNDLE_PATH"
fi
/usr/bin/ditto "$STAGED_BUNDLE" "$BUNDLE_PATH"
/usr/bin/codesign --verify --deep --strict "$BUNDLE_PATH"

print -r -- "$BUNDLE_PATH"
