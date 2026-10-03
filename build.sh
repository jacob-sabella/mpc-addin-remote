#!/usr/bin/env bash
# The addin for 32-bit ARM MPC OS devices: build/mpc_remote_addin.so (and build/standalone, for trying it without
# restarting MPC), with the install scripts and default settings beside them. Built against glibc 2.31 so it loads on older MPC OS too. Needs Docker with QEMU for arm32v7.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm32v7/gcc:11-bullseye sh -c '
  set -e
  F="-std=gnu11 -O2 -Wall -Wextra -Werror -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard"
  gcc $F -fPIC -shared -fvisibility=hidden -Wl,-z,defs -Wl,--as-needed -o build/mpc_remote_addin.so \
    src/remote.c src/capture.c src/png.c src/touch.c src/conf.c -ldl -lpthread
  strip --strip-unneeded build/mpc_remote_addin.so
  gcc $F -o build/standalone tools/standalone.c -ldl
  strip build/standalone
  max=$(objdump -T build/mpc_remote_addin.so build/standalone | grep -o "GLIBC_[0-9.]*" | sort -t. -k2 -n -u | tail -n 1)
  echo "highest glibc symbol version: $max"
  case "$max" in GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|GLIBC_2.3[01]) ;; *) echo "too new for older MPC OS: $max" >&2; exit 1 ;; esac
  readelf -d build/mpc_remote_addin.so | grep NEEDED
'
cp -f mpc_remote_addin.conf install.sh uninstall.sh preload.sh build/   # build/ is the folder to copy to a device
ls -l build/mpc_remote_addin.so build/standalone
