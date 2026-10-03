#!/usr/bin/env bash
# Offline tests on the build machine (x86): unit tests and the HTTP server under ASan+UBSan and under TSan, and a
# preload smoke test of the real .so (it starts in a process named MPC and stays out of every other one), and the
# installer against scratch systemd layouts (BUSYBOX=/path/to/busybox runs it in the device's shell).
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/host
mkdir -p "$B"
SRC="src/remote.c src/capture.c src/png.c src/touch.c src/conf.c"
W="-std=gnu11 -O1 -g -Wall -Wextra -Werror -DREMOTE_TEST -fno-omit-frame-pointer"

cc $W -fsanitize=address,undefined -fno-sanitize-recover=all -o "$B/unit" tests/unit_test.c src/conf.c src/png.c src/touch.c -ldl -lpthread
"$B/unit"

cc $W -fsanitize=address,undefined -fno-sanitize-recover=all -o "$B/host_asan" tests/host_main.c $SRC -ldl -lpthread
python3 tests/test_remote.py "$B/host_asan"
cc $W -fsanitize=thread -o "$B/host_tsan" tests/host_main.c $SRC -ldl -lpthread
TSAN_OPTIONS="halt_on_error=0" python3 tests/test_remote.py "$B/host_tsan"

# the real (non-test) .so, preloaded
cc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared -fvisibility=hidden -o "$B/mpc_remote_addin.so" $SRC -ldl -lpthread
cc -O2 -o "$B/MPC" tests/fake_mpc.c
cp "$B/MPC" "$B/other"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
printf 'port=%s\nbind=127.0.0.1\n' "$PORT" > "$B/smoke.conf"
check_port() { python3 -c "import socket,sys; s=socket.socket(); s.settimeout(1); sys.exit(s.connect_ex(('127.0.0.1',$PORT)))"; }
MPC_REMOTE_ADDIN_CONF="$B/smoke.conf" LD_PRELOAD="$PWD/$B/mpc_remote_addin.so" "$B/other" & P=$!
sleep 0.5
if check_port; then echo "FAIL the add-in started in a process not named MPC"; kill $P; exit 1; fi
kill $P; wait $P 2>/dev/null || true
echo "ok   preload: stays out of other processes"
MPC_REMOTE_ADDIN_CONF="$B/smoke.conf" LD_PRELOAD="$PWD/$B/mpc_remote_addin.so" "$B/MPC" 2>"$B/smoke.log" & P=$!
for _ in $(seq 50); do check_port && break; sleep 0.1; done
INFO=$(curl -s "http://127.0.0.1:$PORT/info")
kill $P; wait $P 2>/dev/null || true
case "$INFO" in *'"version"'*) echo "ok   preload: serves inside MPC: $INFO" ;; *) echo "FAIL preload: no answer"; cat "$B/smoke.log"; exit 1 ;; esac
MPC_REMOTE_ADDIN_DISABLE=1 MPC_REMOTE_ADDIN_CONF="$B/smoke.conf" LD_PRELOAD="$PWD/$B/mpc_remote_addin.so" "$B/MPC" & P=$!
sleep 0.5
if check_port; then echo "FAIL MPC_REMOTE_ADDIN_DISABLE ignored"; kill $P; exit 1; fi
kill $P; wait $P 2>/dev/null || true
echo "ok   preload: MPC_REMOTE_ADDIN_DISABLE keeps it off"
EXPORTS=$(nm -D --defined-only "$B/mpc_remote_addin.so" | awk '$2 == "T" {print $3}' | tr '\n' ' ')
[ "$EXPORTS" = "mpc_remote_addin_start " ] || { echo "FAIL exports: $EXPORTS"; exit 1; }
echo "ok   exports only mpc_remote_addin_start"
tests/test_install.sh
echo "all tests passed"
