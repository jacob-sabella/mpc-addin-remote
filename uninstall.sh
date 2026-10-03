#!/bin/sh
# Remove mpc-addin-remote: take its .so out of LD_PRELOAD (other addins stay), restart MPC, delete the folder.
#   sh uninstall.sh [-y] [-n] [-t <folder>]
set -e
cd "$(dirname "$0")"
DIR=/data/mpc-addins/remote; YES=0; RESTART=1
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -n) RESTART=0; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; DIR="$2"; shift 2 ;;
        *) die "usage: sh uninstall.sh [-y] [-n] [-t <folder>]" ;;
    esac
done
case "$DIR" in /*) ;; *) die "-t must be an absolute path" ;; esac
case "$DIR" in *[!A-Za-z0-9/._-]*) die "the folder may only contain letters, digits and / . _ -" ;; esac
SO="$DIR/mpc_remote_addin.so"
[ -n "$ADDIN_INSTALL_TEST" ] || [ "$(id -u)" = 0 ] || die "run as root"
. ./preload.sh
SVC=$(mpc_service)
if [ $YES = 0 ]; then
    printf "Remove the remote addin%s? [y/N] " "$([ $RESTART = 1 ] && echo ' and restart MPC')"; read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
preload_remove "$SVC" "$SO"
svc daemon-reload
[ $RESTART = 1 ] && svc restart "$SVC"
rm -rf "$DIR"   # after the restart: MPC no longer has the .so mapped
sync
echo "Removed."
