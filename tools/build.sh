#!/usr/bin/env bash
# PS5 DevLoop - build the resident payload, from macOS or inside the VM.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/build.sh [make arguments]      -> build/ps5loop.elf
#
# The payload SDK (ps5-payload-dev/sdk) is found through PS5_PAYLOAD_SDK, or in
# ~/ps5-payload-sdk, /usr/local/ps5-payload-sdk or /opt/ps5-payload-sdk. On
# macOS with no SDK set, the build runs in a Lima VM that has one and mounts
# this folder at the same path: PS5LOOP_VM names it (default "ps5").

set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $(uname) == Darwin && -z ${PS5_PAYLOAD_SDK:-} ]] && command -v limactl >/dev/null; then
    exec limactl shell "${PS5LOOP_VM:-ps5}" -- bash -lc 'cd "$1"; shift; exec bash tools/build.sh "$@"' _ "$root" "$@"
fi

sdk="${PS5_PAYLOAD_SDK:-}"
if [[ -z $sdk ]]; then
    for candidate in "$HOME/ps5-payload-sdk" /usr/local/ps5-payload-sdk /opt/ps5-payload-sdk; do
        if [[ -x "$candidate/bin/prospero-clang" ]]; then
            sdk="$candidate"
            break
        fi
    done
fi
if [[ -z $sdk || ! -x "$sdk/bin/prospero-clang" ]]; then
    echo "build: no PS5 payload SDK; set PS5_PAYLOAD_SDK to one" >&2
    exit 1
fi
export PS5_PAYLOAD_SDK="$sdk"

make -C "$root/payload" "$@"
echo "==> $root/build/ps5loop.elf"
