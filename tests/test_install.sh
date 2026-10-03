#!/usr/bin/env bash
# The shared addin installer (mpc-vst-plugins' tools/release/addin, whose own tests cover its rules) with this addin's
# manifest: install into a scratch systemd tree next to another preloaded library, settings kept on reinstall,
# uninstall restores the line. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path). BUSYBOX=/path/to/busybox
# runs it in the device's shell.
set -euo pipefail
cd "$(dirname "$0")/.."
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"; INST="$MPC_VST/tools/release/addin"
[ -f "$INST/addin-lib.sh" ] || { echo "FAIL installer: no mpc-vst-plugins at $MPC_VST (set MPC_VST)"; exit 1; }
SH="sh"; BB="${BUSYBOX:-$(command -v busybox || true)}"; [ -n "$BB" ] && SH="$BB sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/pkg" "$T/root/usr/lib/systemd/system"
cp "$INST/install.sh" "$INST/uninstall.sh" "$INST/addin-lib.sh" addin.manifest "$T/pkg/"
cp mpc_remote_addin.conf "$T/pkg/"; echo so > "$T/pkg/mpc_remote_addin.so"; echo bin > "$T/pkg/standalone"
U="$T/root/usr/lib/systemd/system/acvs.service"
printf '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/x.so\nExecStart=/usr/bin/az01-launch-MPC\n' > "$U"
run() { ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/pkg/$1" -y -t "$T/addin" > "$T/out" 2>&1 || { cat "$T/out"; exit 1; }; }
fail() { echo "FAIL installer: $1"; exit 1; }
run install.sh
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so:$T/addin/mpc_remote_addin.so" "$U" || fail "LD_PRELOAD: $(grep Env "$U")"
[ -f "$T/addin/standalone" ] && grep -q "port=" "$T/addin/mpc_remote_addin.conf" && grep -q ":6720" "$T/out" || fail "files"
echo "port=9000" > "$T/addin/mpc_remote_addin.conf"; run install.sh
grep -qx "port=9000" "$T/addin/mpc_remote_addin.conf" || fail "settings not kept"
ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/addin/uninstall.sh" -y -t "$T/addin" > "$T/out" 2>&1 || { cat "$T/out"; exit 1; }
grep -qx "Environment=LD_PRELOAD=/usr/lib/x.so" "$U" && [ ! -e "$T/addin" ] || fail "uninstall"
echo "ok   installer: install, reinstall keeps settings, uninstall from the installed folder"
