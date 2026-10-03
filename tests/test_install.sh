#!/usr/bin/env bash
# install.sh / uninstall.sh against scratch copies of systemd unit layouts (run with busybox sh where available,
# the device's shell). Checks that LD_PRELOAD keeps every other addin, installs are idempotent and uninstall
# restores the line.
set -euo pipefail
cd "$(dirname "$0")/.."
SH="sh"; BB="${BUSYBOX:-$(command -v busybox || true)}"; [ -n "$BB" ] && SH="$BB sh"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
pkg() { rm -rf "$T/pkg"; mkdir -p "$T/pkg"; cp install.sh uninstall.sh preload.sh mpc_remote_addin.conf "$T/pkg/"; echo so > "$T/pkg/mpc_remote_addin.so"; }
run() { ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" ADDIN_TEST_LOG="$T/log" $SH "$T/pkg/$1" -y -t "$T/addin" "${@:2}" > "$T/out" 2>&1 || { cat "$T/out"; return 1; }; }
SO="$T/addin/mpc_remote_addin.so"
unit() { mkdir -p "$T/root/usr/lib/systemd/system"; printf '[Unit]\nDescription=MPC\n\n[Service]\n%s\nExecStart=/usr/bin/az01-launch-MPC\n' "$1" > "$T/root/usr/lib/systemd/system/acvs.service"; }
line() { grep '^Environment' "$T/root/usr/lib/systemd/system/acvs.service" || true; }

# 1. the service already preloads two addins
rm -rf "$T/root" "$T/addin"; pkg
unit 'Environment=LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so'
run install.sh
[ "$(line)" = "Environment=LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so:$SO" ] && ok "appended to the existing list" || bad "append: $(line)"
[ -f "$T/root/usr/lib/systemd/system/acvs.service.bak-remote-addin" ] && ok "backup kept" || bad "no backup"
[ -f "$SO" ] && [ -f "$T/addin/mpc_remote_addin.conf" ] && ok "files installed" || bad "files"
grep -q "systemctl restart acvs" "$T/log" && ok "MPC restarted" || bad "no restart"
echo "port=9000" > "$T/addin/mpc_remote_addin.conf"
run install.sh -n
[ "$(line)" = "Environment=LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so:$SO" ] && ok "installing again changes nothing" || bad "idempotent: $(line)"
grep -q "port=9000" "$T/addin/mpc_remote_addin.conf" && ok "the user's settings survive a reinstall" || bad "conf overwritten"
[ "$(grep -c restart "$T/log")" = 1 ] && ok "-n doesn't restart" || bad "-n restarted"
run uninstall.sh
[ "$(line)" = "Environment=LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so" ] && ok "uninstall leaves the others" || bad "uninstall: $(line)"
[ ! -e "$T/addin" ] && ok "folder removed" || bad "folder left"

# 2. quoted, space-separated, with another variable on the line
rm -rf "$T/root" "$T/addin" "$T/log"; pkg
unit 'Environment="LD_PRELOAD=/usr/lib/a.so /usr/lib/b.so" FOO=1'
run install.sh
[ "$(line)" = "Environment=\"LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so:$SO\" FOO=1" ] && ok "quoted form" || bad "quoted: $(line)"
run uninstall.sh
[ "$(line)" = 'Environment="LD_PRELOAD=/usr/lib/a.so:/usr/lib/b.so" FOO=1' ] && ok "quoted form uninstall" || bad "quoted uninstall: $(line)"

# 3. nothing preloaded yet: a drop-in, removed again on uninstall
rm -rf "$T/root" "$T/addin" "$T/log"; pkg
unit 'Restart=always'
run install.sh
D="$T/root/etc/systemd/system/acvs.service.d/50-mpc-remote-addin.conf"
grep -qx "Environment=LD_PRELOAD=$SO" "$D" && ok "drop-in when nothing preloads" || bad "drop-in"
run install.sh
[ "$(grep -c LD_PRELOAD "$D")" = 1 ] && grep -qx "Environment=LD_PRELOAD=$SO" "$D" && ok "drop-in reinstall" || bad "drop-in reinstall: $(cat "$D")"
run uninstall.sh
[ ! -e "$D" ] && ok "drop-in removed" || bad "drop-in left"

# 4. only this addin in the unit's line: uninstall drops the assignment
rm -rf "$T/root" "$T/addin" "$T/log"; pkg
unit "Environment=LD_PRELOAD=$SO"
run uninstall.sh
[ -z "$(line)" ] && ok "a line holding only this addin goes away" || bad "only: $(line)"

# 5. another drop-in sets LD_PRELOAD (it wins over the unit): that one is edited
rm -rf "$T/root" "$T/addin" "$T/log"; pkg
unit 'Environment=LD_PRELOAD=/usr/lib/a.so'
mkdir -p "$T/root/etc/systemd/system/acvs.service.d"
printf '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/a.so:/data/nam.so\n' > "$T/root/etc/systemd/system/acvs.service.d/10-nam.conf"
run install.sh
grep -qx "Environment=LD_PRELOAD=/usr/lib/a.so:/data/nam.so:$SO" "$T/root/etc/systemd/system/acvs.service.d/10-nam.conf" \
  && [ "$(line)" = "Environment=LD_PRELOAD=/usr/lib/a.so" ] && ok "the winning drop-in is the one edited" || bad "drop-in precedence"

# 6. the inmusic-mpc service name, and refusing odd folders
rm -rf "$T/root" "$T/addin" "$T/log"; pkg
mkdir -p "$T/root/usr/lib/systemd/system"; printf '[Service]\nEnvironment=LD_PRELOAD=/usr/lib/a.so\n' > "$T/root/usr/lib/systemd/system/inmusic-mpc.service"
run install.sh
grep -q "$SO" "$T/root/usr/lib/systemd/system/inmusic-mpc.service" && grep -q "restart inmusic-mpc" "$T/log" && ok "inmusic-mpc service" || bad "inmusic-mpc"
if ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/pkg/install.sh" -y -t '/data/x;rm' >/dev/null 2>&1; then bad "odd folder accepted"; else ok "odd folder refused"; fi
if ADDIN_INSTALL_TEST=1 SYSTEMD_ROOT="$T/root" $SH "$T/pkg/install.sh" -y -t 'rel/dir' >/dev/null 2>&1; then bad "relative folder accepted"; else ok "relative folder refused"; fi

[ $fails = 0 ] && echo "install: all passed" || { echo "install: $fails FAILED"; exit 1; }
