#!/usr/bin/env bash
# The addin for 64-bit ARM MPC OS devices (e.g. the MPC Sample): build/aarch64/mpc_remote_addin.so and
# build/aarch64/standalone. Needs Docker with QEMU for arm64.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/aarch64
docker run --rm --platform linux/arm64 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm64v8/gcc:12-bookworm sh -c '
  set -e
  F="-std=gnu11 -O2 -Wall -Wextra -Werror"
  gcc $F -fPIC -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed -o build/aarch64/mpc_remote_addin.so \
    src/remote.c src/capture.c src/png.c src/touch.c src/conf.c src/json.c src/image.c src/mcp.c src/midi.c \
    src/device.c src/buttons.c -ldl -lpthread
  strip --strip-unneeded build/aarch64/mpc_remote_addin.so
  gcc $F -o build/aarch64/standalone tools/standalone.c -ldl
  strip build/aarch64/standalone
  max=$(objdump -T build/aarch64/mpc_remote_addin.so build/aarch64/standalone | grep -o "GLIBC_[0-9.]*" | sort -t. -k2 -n -u | tail -n 1)
  echo "highest glibc symbol version: $max"
  readelf -d build/aarch64/mpc_remote_addin.so | grep NEEDED
'
cp -f mpc_remote_addin.conf addin.manifest build/aarch64/   # the release packages build/aarch64 with the installer
ls -l build/aarch64/mpc_remote_addin.so build/aarch64/standalone
