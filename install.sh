#!/bin/sh
# Install mpc-preload-addin-remote on an MPC OS device. Run on the device as root, from the unpacked folder:
#   sh install.sh [-y] [-n] [-t <folder>]
# Copies mpc_remote_addin.so (and, the first time, mpc_remote_addin.conf) into <folder> (default
# /data/mpc-addins/remote), adds the .so to the LD_PRELOAD of MPC's systemd service, and restarts MPC.
# LD_PRELOAD is extended, never replaced: other add-ins already in it stay. Where the service already sets it, that
# line is edited in place (backed up first). A drop-in would replace the whole list. Otherwise a drop-in sets it.
#   -y  don't ask   -n  don't restart MPC (the add-in starts with MPC's next start)
set -e
cd "$(dirname "$0")"
DIR=/data/mpc-addins/remote; YES=0; RESTART=1
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -n) RESTART=0; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; DIR="$2"; shift 2 ;;
        *) die "usage: sh install.sh [-y] [-n] [-t <folder>]" ;;
    esac
done
case "$DIR" in /*) ;; *) die "-t must be an absolute path" ;; esac
case "$DIR" in *[!A-Za-z0-9/._-]*) die "the folder may only contain letters, digits and / . _ -" ;; esac
SO="$DIR/mpc_remote_addin.so"

if [ -z "$ADDIN_INSTALL_TEST" ]; then
    [ "$(id -u)" = 0 ] || die "run as root"
    case "$(uname -m)" in armv7*) ;; *) die "this build is for 32-bit ARM MPC OS devices; this one is $(uname -m)" ;; esac
fi
[ -f mpc_remote_addin.so ] || die "mpc_remote_addin.so is missing next to install.sh"
. ./preload.sh

SVC=$(mpc_service)
UNIT=$(unit_with_preload "$SVC")
echo "Installing mpc-preload-addin-remote:"
echo "  $SO"
if [ -n "$UNIT" ]; then echo "  LD_PRELOAD in $UNIT gains it (a backup is kept)"; else echo "  a drop-in sets LD_PRELOAD for $SVC"; fi
[ $RESTART = 1 ] && echo "  then MPC restarts: save your project first"
if [ $YES = 0 ]; then
    printf "Continue? [y/N] "; read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

mkdir -p "$DIR"
cp mpc_remote_addin.so "$SO.new" && chmod 644 "$SO.new" && mv "$SO.new" "$SO"
[ -f "$DIR/mpc_remote_addin.conf" ] || cp mpc_remote_addin.conf "$DIR/mpc_remote_addin.conf"
preload_add "$SVC" "$UNIT" "$SO"
svc daemon-reload
sync
if [ $RESTART = 1 ]; then svc restart "$SVC"; echo "Done. MPC restarted: open http://<this device's address>:$(conf_port)/"
else echo "Done. The add-in starts with MPC's next start."; fi
