#!/usr/bin/env python3
"""Drive the addin's HTTP server (tests/host_main built with sanitizers) against a fake framebuffer and touch
file: the page, /info, PNG frames (decoded and compared pixel by pixel), the stream, touch events, bad requests
and the connection limits.  Usage: test_remote.py <host_main binary>"""
import json, os, socket, struct, subprocess, sys, tempfile, threading, time, urllib.request, zlib

W, H = 64, 40     # the scanout (main() also runs a portrait one: 40 x 64, shown turned 90 degrees)
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        fails += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def png_decode(data):
    """RGB24 pixels of a filter-0, 8-bit RGB PNG (what the encoder writes)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w, h = 8, b"", None, None
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        assert zlib.crc32(kind + body) == crc, "bad CRC"
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and ctype == 2
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = [raw[y * (1 + w * 3):(y + 1) * (1 + w * 3)] for y in range(h)]
    assert all(r[0] == 0 for r in rows)
    return w, h, b"".join(r[1:] for r in rows)


def expected(fb, scale, turn):
    """The upright RGB frame: the scanout sampled every `scale`, turned `turn` degrees clockwise."""
    ws, hs = W // scale, H // scale
    src = [[fb[((y * scale) * W + x * scale) * 4:((y * scale) * W + x * scale) * 4 + 3][::-1] for x in range(ws)] for y in range(hs)]
    for _ in range(turn // 90):
        src = [list(r) for r in zip(*src[::-1])]   # one clockwise quarter turn
    return b"".join(b"".join(r) for r in src)


def upright_to_scanout(x, y, turn):
    return (y, H - 1 - x) if turn == 90 else (x, y)


def get(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()


def raw(port, data, timeout=5):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(data)
    out = b""
    try:
        time.sleep(0.05)
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
    except socket.timeout:
        out += b"<timeout>"
    except ConnectionResetError:
        out += b"<reset>"
    s.close()
    return out


def events(path):
    data = open(path, "rb").read()
    size = struct.calcsize("llHHi")
    return [struct.unpack("llHHi", data[i:i + size])[2:] for i in range(0, len(data) - size + 1, size)]


def run(exe, full):
    turn = 90 if H > W else 0
    UW, UH = (H, W) if turn else (W, H)
    print(f"-- scanout {W} x {H}, shown {UW} x {UH}")
    tmp = tempfile.mkdtemp()
    fbpath, touchpath, confpath = (os.path.join(tmp, n) for n in ("fb.raw", "touch.ev", "remote.conf"))
    fb = bytearray()
    for y in range(H):
        for x in range(W):
            fb += bytes((x * 4 % 256, y * 6 % 256, (x ^ y) * 3 % 256, 0xFF))   # B G R X
    open(fbpath, "wb").write(fb)
    port = free_port()
    open(confpath, "w").write(f"port={port}\nbind=127.0.0.1\nmax_fps=20\nmax_clients=4\nbogus line\n")
    env = dict(os.environ, REMOTE_FAKE_FB=f"{fbpath},{W},{H}", REMOTE_FAKE_TOUCH=touchpath, MPC_REMOTE_ADDIN_CONF=confpath)
    proc = subprocess.Popen([exe], env=env, stderr=subprocess.PIPE, text=True)
    base = f"http://127.0.0.1:{port}"
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)

        st, ct, body = get(base + "/")
        check(st == 200 and "text/html" in ct and b"/stream" in body, "page")
        st, ct, body = get(base + "/info")
        info = json.loads(body)
        check(st == 200 and info["width"] == UW and info["height"] == UH and info["format"] == "XR24"
              and info["capture"] == "ok" and info["png"] is True and info["screen_rotate"] == -1
              and info["touch_rotate"] == 0, f"/info {info}")

        st, ct, body = get(base + "/screen.png")
        w, h, px = png_decode(body)
        check(st == 200 and ct == "image/png" and (w, h) == (UW, UH) and px == expected(fb, 1, turn), "full frame matches the framebuffer")
        st, ct, body = get(base + "/screen.png?half=1")
        w, h, px = png_decode(body)
        check((w, h) == (UW // 2, UH // 2) and px == expected(fb, 2, turn), "half frame takes every other pixel")
        info = json.loads(get(base + "/info")[2])
        check(info["capture_ms"] >= 0 and info["encode_ms"] >= 0, "timings reported")

        # touch: a tap lands on the scanout point under the upright one, then lifts
        before = len(events(touchpath)) if os.path.exists(touchpath) else 0
        st, _, body = get(base + "/tap?x=16&y=30&hold=20")
        ev = events(touchpath)[before:]
        EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
        sx, sy = upright_to_scanout(16, 30, turn)
        pos = {c: v for t, c, v in ev if t == EV_ABS and c in (0x35, 0x36)}
        check(st == 200 and pos == {0x35: sx * 2048 // W, 0x36: sy * 2048 // H}, f"tap position {pos}")
        check((EV_KEY, 0x14a, 1) in ev and (EV_KEY, 0x14a, 0) in ev and (EV_ABS, 0x39, -1) in ev
              and ev[-1] == (EV_SYN, 0, 0), "tap presses, lifts and syncs")
        if full:

            # stream: the first frame comes at once, an unchanged screen sends no more, a change sends one
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.sendall(b"GET /stream HTTP/1.1\r\nHost: x\r\n\r\n")
            buf = b""
            t0 = time.time()
            while buf.count(b"--mpcframe") < 1 and time.time() - t0 < 5:
                buf += s.recv(65536)
            check(b"multipart/x-mixed-replace; boundary=mpcframe" in buf, "stream header")
            time.sleep(0.6)
            s.settimeout(0.3)
            more = b""
            try:
                more = s.recv(65536)
            except socket.timeout:
                pass
            rest = buf.split(b"--mpcframe", 1)[1]
            n_frames = (rest + more).count(b"--mpcframe") + 1
            check(n_frames == 1, f"an unchanged screen sends no new frames (got {n_frames})")
            with open(fbpath, "r+b") as f:
                f.write(b"\x00\x00\xff\xff")      # pixel (0,0) turns red
            got = b""
            s.settimeout(3)
            t0 = time.time()
            while b"--mpcframe" not in got and time.time() - t0 < 3:
                got += s.recv(65536)
            check(b"--mpcframe" in got, "a changed screen sends a frame")
            s.close()

            before = len(events(touchpath))
            get(base + "/move?x=1&y=1")
            check(len(events(touchpath)) == before, "a move with no finger down does nothing")
            get(base + "/down?x=10&y=10"); get(base + "/move?x=20&y=10"); get(base + "/up")
            ev = events(touchpath)[before:]
            check(ev.count((EV_KEY, 0x14a, 1)) == 1 and ev.count((EV_KEY, 0x14a, 0)) == 1
                  and sum(1 for e in ev if e[:2] == (EV_ABS, 0x36)) == 2, "down, move, up")
            check(get(base + "/tap?x=5")[0] == 400, "a tap without y is refused")
            check(get(base + "/tap?max=5&y=5")[0] == 400, "x is not found inside another key")

            # bad and hostile requests
            check(get(base + "/nope")[0] == 404, "404")
            check(raw(port, b"POST / HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 405"), "POST refused")
            check(raw(port, b"\r\n\r\n").startswith(b"HTTP/1.1 400"), "empty request")
            check(raw(port, b"GET /" + b"a" * 5000 + b" HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 400"), "oversized request line")
            check(raw(port, b"GET / HTTP/1.1\r\n", timeout=8).startswith(b"HTTP/1.1 200"), "a request without the blank line still gets an answer after the timeout")
            check(raw(port, b"GET /screen.png?half=7&x=\xff HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 200"), "odd query values")

            # limits: two streams at most; max_clients=4 connections at once
            holders = []
            for _ in range(2):
                c = socket.create_connection(("127.0.0.1", port), timeout=5)
                c.sendall(b"GET /stream HTTP/1.1\r\n\r\n")
                c.recv(100)
                holders.append(c)
            check(get(base + "/stream")[0] == 503, "a third stream is refused")
            idle = [socket.create_connection(("127.0.0.1", port), timeout=8) for _ in range(2)]
            time.sleep(0.2)
            resp = raw(port, b"GET / HTTP/1.1\r\n\r\n")
            check(resp.startswith(b"HTTP/1.1 503"), "connections over max_clients are refused")
            for c in holders + idle:
                c.close()
            time.sleep(6)                          # the idle ones time out, the streams notice their client left
            check(get(base + "/")[0] == 200, "slots come back after clients leave")
    finally:
        proc.terminate()
        try:
            _, err = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, err = proc.communicate()
    check("not understood" in err, "a bad config line is reported")
    bad = [l for l in err.splitlines() if "ERROR" in l or "runtime error" in l or "WARNING: ThreadSanitizer" in l]
    check(not bad, "no sanitizer reports" + ("".join("\n     " + l for l in bad[:5])))
    if fails:
        print(err[-4000:])


def main():
    global W, H
    run(sys.argv[1], True)
    W, H = 40, 64
    run(sys.argv[1], False)
    print(f"http: {'all passed' if not fails else f'{fails} FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
