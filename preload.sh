# Shared by install.sh and uninstall.sh: MPC's systemd service and its LD_PRELOAD list.
# Tests set ADDIN_INSTALL_TEST=1, SYSTEMD_ROOT (a scratch tree holding the unit files) and ADDIN_TEST_LOG.

UNIT_DIRS="/etc/systemd/system /run/systemd/system /usr/lib/systemd/system /lib/systemd/system"
DROPIN_NAME=50-mpc-remote-addin.conf

svc() {   # systemctl, or a log line under test
    if [ -n "$ADDIN_INSTALL_TEST" ]; then echo "systemctl $*" >> "${ADDIN_TEST_LOG:-/dev/null}"; return 0; fi
    systemctl "$@"
}

# MPC's service: acvs on stock firmware, inmusic-mpc on some modified ones.
mpc_service() {
    for s in acvs inmusic-mpc; do
        for d in $UNIT_DIRS; do
            [ -f "$SYSTEMD_ROOT$d/$s.service" ] && { echo "$s"; return; }
        done
    done
    echo acvs
}

# The unit file or drop-in whose Environment= line sets LD_PRELOAD and wins (the last one systemd reads), if any.
unit_with_preload() {
    found=""
    for d in $UNIT_DIRS; do   # the main unit: the first directory that has it
        f="$SYSTEMD_ROOT$d/$1.service"
        if [ -f "$f" ]; then grep -q '^Environment=.*LD_PRELOAD=' "$f" && found="$f"; break; fi
    done
    for f in $(for d in $UNIT_DIRS; do ls "$SYSTEMD_ROOT$d/$1.service.d/"*.conf 2>/dev/null; done | awk -F/ '{print $NF "\t" $0}' | sort | cut -f2); do
        grep -q '^Environment=.*LD_PRELOAD=' "$f" && found="$f"   # drop-ins apply in name order
    done
    echo "$found"
}

# Rewrite the LD_PRELOAD value on each matching line of a file with an awk program ("add" or "remove" $so).
edit_preload() {   # file mode so
    awk -v mode="$2" -v so="$3" '
    /^Environment=/ && (i = index($0, "LD_PRELOAD=")) {
        pre = substr($0, 1, i - 1); rest = substr($0, i + 11)
        quoted = substr(pre, length(pre), 1) == "\""   # Environment="LD_PRELOAD=/a.so /b.so" runs to the quote
        e = index(rest, quoted ? "\"" : " "); if (!e) e = length(rest) + 1
        val = substr(rest, 1, e - 1); post = substr(rest, e)
        n = split(val, parts, /[: ]+/); out = ""; seen = 0
        for (k = 1; k <= n; k++) {
            if (parts[k] == "") continue
            if (parts[k] == so) { seen = 1; if (mode == "remove") continue }
            out = out (out == "" ? "" : ":") parts[k]
        }
        if (mode == "add" && !seen) out = out (out == "" ? "" : ":") so
        if (out == "") {                              # nothing left: drop the assignment (and its quotes)
            if (quoted) { pre = substr(pre, 1, length(pre) - 1); post = substr(post, 2) }
            sub(/^ +/, "", post); print pre post; next
        }
        print pre "LD_PRELOAD=" out post; next
    }
    { print }' "$1" > "$1.new"
    sed -i 's/^Environment= *$//' "$1.new"
    mv "$1.new" "$1"
}

preload_add() {   # service unit so
    if [ -n "$2" ]; then
        bak="$2.bak-remote-addin"
        [ -f "$bak" ] || cp "$2" "$bak"
        edit_preload "$2" add "$3"
        grep -q "$3" "$2" || { cp "$bak" "$2"; echo "error: editing $2 failed; restored" >&2; exit 1; }
    else
        d="$SYSTEMD_ROOT/etc/systemd/system/$1.service.d"
        mkdir -p "$d"
        printf '[Service]\nEnvironment=LD_PRELOAD=%s\n' "$3" > "$d/$DROPIN_NAME.new"
        mv "$d/$DROPIN_NAME.new" "$d/$DROPIN_NAME"
    fi
}

preload_remove() {   # service so
    for d in $UNIT_DIRS; do
        for f in "$SYSTEMD_ROOT$d/$1.service" "$SYSTEMD_ROOT$d/$1.service.d/"*.conf; do
            [ -f "$f" ] && grep -q "$2" "$f" || continue
            if [ "$(basename "$f")" = "$DROPIN_NAME" ]; then rm -f "$f"; else edit_preload "$f" remove "$2"; fi
        done
    done
}

conf_port() { sed -n 's/^port *= *\([0-9]*\).*/\1/p' "$DIR/mpc_remote_addin.conf" | tail -n 1; }
