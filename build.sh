#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build HALO. The firmware package needs JieLi's Linux toolchain and AC79 SDK (docs/DEPLOY.md; GitHub Actions has
# both, .github/workflows/build.yml); the host simulator needs only a C compiler and Python with Pillow.
#   ./build.sh                 the firmware: firmware/build/halo.fwsc
#   ./build.sh --release 0.1   a release: firmware/build/halo-0.1.fwsc
#   ./build.sh --host          the simulator: firmware/build/host/halo_host
#   ./build.sh --web           the browser emulator: firmware/build/emu/ (Emscripten)
set -e
cd "$(dirname "$0")"
[ -f base/tools/build.py ] || git submodule update --init --recursive
exec python3 base/tools/build.py firmware "$@"
