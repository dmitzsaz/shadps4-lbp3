#!/bin/zsh
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

SCRIPT_DIR=${0:A:h}
PROJECT_ROOT=${SCRIPT_DIR:h}
DELIVERY_ROOT=$PROJECT_ROOT
POLICY_SEARCH_ROOT=$PROJECT_ROOT
while [[ "$POLICY_SEARCH_ROOT" != "/" ]]; do
    if [[ -f "$POLICY_SEARCH_ROOT/AGENTS.md" ]]; then
        DELIVERY_ROOT=$POLICY_SEARCH_ROOT
        break
    fi
    POLICY_SEARCH_ROOT=${POLICY_SEARCH_ROOT:h}
done

OUTPUT_PARENT=${1:-"$DELIVERY_ROOT/updater-dist"}
BUNDLE_PATH="$OUTPUT_PARENT/shadPS4-lbp3.app"
RUNTIME_BUNDLE=${SHADPS4_RUNTIME_BUNDLE:-"$DELIVERY_ROOT/shadPS4-lbp3.app"}

if [[ "${BUNDLE_PATH:t}" != "shadPS4-lbp3.app" ]]; then
    print -u2 "Updater bundle must be named shadPS4-lbp3.app"
    exit 2
fi
if [[ "${RUNTIME_BUNDLE:t}" != "shadPS4-lbp3.app" || ! -d "$RUNTIME_BUNDLE" ]]; then
    print -u2 "Runtime source must be a shadPS4-lbp3.app bundle: $RUNTIME_BUNDLE"
    exit 2
fi
if [[ "${RUNTIME_BUNDLE:A}" == "${BUNDLE_PATH:A}" ]]; then
    print -u2 "Runtime source and updater output must be different bundles"
    exit 2
fi

RUNTIME_FILES=(
    Contents/MacOS/shadps4
    Contents/MacOS/shadps4-core
    Contents/MacOS/partychat
    Contents/MacOS/libvulkan.dylib
    Contents/MacOS/libvulkan_kosmickrisp.dylib
    Contents/MacOS/kosmickrisp_mesa_icd.json
    Contents/Info.plist
    Contents/Resources/LICENSE-shadPS4.txt
    Contents/Resources/LICENSE-PartyChat.txt
)
for relative_path in $RUNTIME_FILES; do
    if [[ ! -f "$RUNTIME_BUNDLE/$relative_path" ]]; then
        print -u2 "Missing runtime payload: $RUNTIME_BUNDLE/$relative_path"
        exit 1
    fi
done

PARTYCHAT_HELP=$("$RUNTIME_BUNDLE/Contents/MacOS/partychat" --help 2>&1 || true)
if ! print -r -- "$PARTYCHAT_HELP" | /usr/bin/grep -Fq 'serve [flags]' || \
    ! print -r -- "$PARTYCHAT_HELP" | /usr/bin/grep -Fq 'index [flags]'; then
    print -u2 "PartyChat payload is missing the required serve/index commands"
    exit 1
fi

STAGE_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/shadps4-core-updater.XXXXXX")
trap 'rm -rf -- "$STAGE_ROOT"' EXIT
STAGED_BUNDLE="$STAGE_ROOT/shadPS4-lbp3.app"
PAYLOAD_ROOT="$STAGED_BUNDLE/Contents/Resources/Runtime"

mkdir -p "$STAGED_BUNDLE/Contents/MacOS" "$PAYLOAD_ROOT"
/usr/bin/xcrun swiftc \
    -O \
    -whole-module-optimization \
    -target arm64-apple-macos14.0 \
    -framework AppKit \
    -framework CryptoKit \
    "$SCRIPT_DIR/CoreUpdater.swift" \
    -o "$STAGED_BUNDLE/Contents/MacOS/shadps4-runtime-updater"

cp "$SCRIPT_DIR/Info.plist" "$STAGED_BUNDLE/Contents/Info.plist"
for relative_path in $RUNTIME_FILES; do
    payload_path="$PAYLOAD_ROOT/${relative_path:t}"
    /bin/cp -X "$RUNTIME_BUNDLE/$relative_path" "$payload_path"
    /usr/bin/xattr -c "$payload_path"
done
chmod 755 "$STAGED_BUNDLE/Contents/MacOS/shadps4-runtime-updater" \
    "$PAYLOAD_ROOT/shadps4" \
    "$PAYLOAD_ROOT/shadps4-core" \
    "$PAYLOAD_ROOT/partychat" \
    "$PAYLOAD_ROOT/libvulkan.dylib" \
    "$PAYLOAD_ROOT/libvulkan_kosmickrisp.dylib"
chmod 644 "$PAYLOAD_ROOT/kosmickrisp_mesa_icd.json" \
    "$PAYLOAD_ROOT/Info.plist" \
    "$PAYLOAD_ROOT/LICENSE-shadPS4.txt" \
    "$PAYLOAD_ROOT/LICENSE-PartyChat.txt"

/usr/bin/plutil -lint "$STAGED_BUNDLE/Contents/Info.plist"
for signed_payload in \
    "$PAYLOAD_ROOT/shadps4" \
    "$PAYLOAD_ROOT/shadps4-core" \
    "$PAYLOAD_ROOT/partychat" \
    "$PAYLOAD_ROOT/libvulkan.dylib" \
    "$PAYLOAD_ROOT/libvulkan_kosmickrisp.dylib"; do
    /usr/bin/codesign --force --sign - "$signed_payload"
done
/usr/bin/codesign --force --deep --sign - "$STAGED_BUNDLE"
/usr/bin/codesign --verify --deep --strict "$STAGED_BUNDLE"

mkdir -p "$OUTPUT_PARENT"
if [[ -e "$BUNDLE_PATH" || -L "$BUNDLE_PATH" ]]; then
    rm -rf -- "$BUNDLE_PATH"
fi
/usr/bin/ditto "$STAGED_BUNDLE" "$BUNDLE_PATH"
/usr/bin/codesign --verify --deep --strict "$BUNDLE_PATH"

print -r -- "$BUNDLE_PATH"
