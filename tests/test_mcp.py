#!/usr/bin/env python3
"""The MCP endpoint (POST /mcp) over raw HTTP against a fake framebuffer and touch file: the JSON-RPC handshake and
errors, every tool's result (PNG pixels checked against the framebuffer, touch events decoded), argument errors as
tool errors, the Origin and size limits, file access confined to the configured folders, and MIDI both ways
through the real ALSA sequencer when this machine has one (aseqdump and aseqsend).
Usage: test_mcp.py <host_main binary>"""
import base64, json, os, shutil, socket, struct, subprocess, sys, tempfile, threading, time, zlib

W, H = 320, 200
fails = 0
# The version the addin reports, from the header it is built with.
VERSION = open(os.path.join(os.path.dirname(__file__), "..", "src", "version.h")).read().split('REMOTE_VERSION "')[1].split('"')[0]


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + "mcp: " + what)
    fails += not cond


def png_decode(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, b"".join(raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)] for y in range(h))


def http(port, method, path, body=b"", headers=None, timeout=30):
    hs = {"Host": f"127.0.0.1:{port}", "Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    hs.update(headers or {})
    if body is not None and "Content-Length" not in hs:
        hs["Content-Length"] = str(len(body))
    head = f"{method} {path} HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in hs.items() if v is not None) + "\r\n"
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(head.encode() + (body or b""))
    out = b""
    while True:
        c = s.recv(1 << 20)
        if not c:
            break
        out += c
    s.close()
    head, _, rest = out.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1])
    return status, head.decode(errors="replace"), rest


class Server:
    def __init__(self, port):
        self.port, self.n = port, 0

    def rpc(self, method, params=None, **kw):
        self.n += 1
        msg = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            msg["params"] = params
        st, head, body = http(self.port, "POST", "/mcp", json.dumps(msg).encode(), **kw)
        return st, json.loads(body) if body else None

    def call(self, name, args=None):
        st, r = self.rpc("tools/call", {"name": name, "arguments": args or {}})
        assert st == 200 and "result" in r, (st, r)
        res = r["result"]
        texts = "\n".join(c["text"] for c in res["content"] if c["type"] == "text")
        images = [base64.b64decode(c["data"]) for c in res["content"] if c["type"] == "image"]
        return res["isError"], texts, images


def events(path):
    if not os.path.exists(path):
        return []
    data = open(path, "rb").read()
    size = struct.calcsize("llHHi")
    return [struct.unpack("llHHi", data[i:i + size])[2:] for i in range(0, len(data) - size + 1, size)]


def positions(ev):
    """(x, y) panel positions written, in order (MT X then Y pairs)."""
    out, x = [], None
    for t, c, v in ev:
        if t == 3 and c == 0x35:
            x = v
        elif t == 3 and c == 0x36 and x is not None:
            out.append((x, v))
    return out


def to_panel(x, y):
    return (x * 2048 // W, y * 2048 // H)


def main(exe):
    tmp = tempfile.mkdtemp()
    fbpath, touchpath, confpath = (os.path.join(tmp, n) for n in ("fb.raw", "touch.ev", "remote.conf"))
    fb = bytearray()
    for y in range(H):
        for x in range(W):
            fb += bytes((x % 256, y % 256, (x * 7 + y * 3) % 256, 0xFF))    # B G R X
    open(fbpath, "wb").write(fb)
    files = os.path.join(tmp, "files")
    os.makedirs(os.path.join(files, "Projects", "Beat"))
    open(os.path.join(files, "Projects", "Beat", "Beat.xpj"), "w").write("project\n" + "line\n" * 2000)
    open(os.path.join(files, "Projects", "Kick 01.WAV"), "wb").write(b"RIFF\0\0\0\0WAVE")
    open(os.path.join(files, "latin1.txt"), "wb").write(b"caf\xe9 ok\n")
    os.symlink("/etc", os.path.join(files, "escape"))
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    open(confpath, "w").write(f"port={port}\nbind=127.0.0.1\nmcp_files={files}\n")
    env = dict(os.environ, REMOTE_FAKE_FB=f"{fbpath},{W},{H}", REMOTE_FAKE_TOUCH=touchpath, MPC_REMOTE_ADDIN_CONF=confpath)
    proc = subprocess.Popen([exe], env=env, stderr=subprocess.PIPE, text=True)
    srv = Server(port)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)
        protocol(srv, port)
        screen(srv, fb)
        touch(srv, touchpath, fbpath)
        tool_errors(srv)
        filesystem(srv, files)
        st_err, text, _ = srv.call("device_status")
        check(not st_err and "Memory:" in text and "Load average:" in text, "device_status")
        midi(srv)
    finally:
        proc.terminate()
        try:
            _, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, err = proc.communicate()
    bad = [l for l in err.splitlines() if "ERROR" in l or "runtime error" in l or "WARNING: ThreadSanitizer" in l]
    check(not bad, "no sanitizer reports" + "".join("\n     " + l for l in bad[:5]))
    if fails:
        print(err[-3000:])


def protocol(srv, port):
    st, r = srv.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
    res = r["result"]
    check(st == 200 and res["protocolVersion"] == "2025-06-18" and "tools" in res["capabilities"]
          and res["serverInfo"]["name"] == "mpc-remote" and "screenshot" in res["instructions"], "initialize")
    _, r = srv.rpc("initialize", {"protocolVersion": "1999-01-01"})
    check(r["result"]["protocolVersion"] == "2025-11-25", "an unknown version gets the newest")
    st, _, body = http(port, "POST", "/mcp", b'{"jsonrpc":"2.0","method":"notifications/initialized"}')
    check(st == 202 and body == b"", "a notification gets 202 and no body")
    st, _, body = http(port, "POST", "/mcp", b'{"jsonrpc":"2.0","id":7,"result":{}}')
    check(st == 202, "a response gets 202")
    _, r = srv.rpc("ping")
    check(r["result"] == {} and r["id"] == srv.n, "ping")
    st, _, body = http(port, "POST", "/mcp", b'{"jsonrpc":"2.0","id":"abc\\"d","method":"ping"}')
    check(json.loads(body)["id"] == 'abc"d', "a string id comes back as sent")
    _, r = srv.rpc("tools/list")
    tools = r["result"]["tools"]
    names = [t["name"] for t in tools]
    want = ["screenshot", "tap", "double_tap", "long_press", "drag", "scroll", "touch", "wait_for_screen",
            "get_screen_info", "play_notes", "send_midi", "midi_listen", "midi_status", "device_status", "list_files",
            "find_files", "read_text_file"]
    check(names == want, f"tools/list names {names}")
    check(all(t["inputSchema"]["type"] == "object" and t["description"] and "readOnlyHint" in t["annotations"] for t in tools),
          "every tool has a schema, a description and annotations")
    _, r = srv.rpc("server/discover")
    check(r["error"]["code"] == -32601, "server/discover: method not found (newer clients then use initialize)")
    _, r = srv.rpc("nope/nothing")
    check(r["error"]["code"] == -32601, "an unknown method")
    _, r = srv.rpc("tools/call", {"name": "format_disk"})
    check(r["error"]["code"] == -32602 and "Unknown tool" in r["error"]["message"], "an unknown tool")
    st, _, body = http(port, "POST", "/mcp", b"{not json")
    check(st == 400 and json.loads(body)["error"]["code"] == -32700, "a parse error")
    st, _, body = http(port, "POST", "/mcp", b'[{"jsonrpc":"2.0","id":1,"method":"ping"}]')
    check(st == 400 and json.loads(body)["error"]["code"] == -32600, "a batch is refused")
    st, _, body = http(port, "POST", "/mcp", b"[" * 100 + b"]" * 100)
    check(st == 400, "nesting too deep")
    st, head, _ = http(port, "GET", "/mcp", None)
    check(st == 405 and "Allow: POST" in head, "GET /mcp: 405, POST only (no SSE stream)")
    st, _, _ = http(port, "POST", "/mcp", b"{}", headers={"Content-Length": None})
    check(st == 411, "a POST without Content-Length")
    st, _, _ = http(port, "POST", "/mcp", b"", headers={"Content-Length": "999999"})
    check(st == 413, "a body over 64 KB")
    st, _, _ = http(port, "POST", "/mcp", b'{"jsonrpc":', headers={"Content-Length": "40"}, timeout=10)
    check(st == 400, "a body that ends early")
    ping = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
    st, _, _ = http(port, "POST", "/mcp", ping, headers={"Origin": "http://evil.example"})
    check(st == 403, "a cross-origin page is refused")
    st, _, _ = http(port, "POST", "/mcp", ping, headers={"Origin": "http://rebound.example:%d" % port, "Host": "rebound.example:%d" % port})
    check(st == 403, "a DNS-rebound name is refused")
    st, _, _ = http(port, "POST", "/mcp", ping, headers={"Origin": f"http://127.0.0.1:{port}"})
    check(st == 200, "a page from the device itself is allowed")
    st, _, _ = http(port, "POST", "/mcp", ping, headers={"Origin": f"http://mpc.local:{port}", "Host": f"mpc.local:{port}"})
    check(st == 200, "a .local name is allowed")
    big = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping", "params": {"pad": "x" * 60000}}).encode()
    st, _, _ = http(port, "POST", "/mcp", big)
    check(st == 200, "a 60 KB body arrives whole")
    st, _, body = http(port, "GET", "/info", None)
    check(st == 200 and json.loads(body)["version"] == VERSION, "the HTTP API still answers")


def expected(fb, x0, y0, w, h, z2):
    out = bytearray()
    ow, oh = max(1, w * z2 // 2), max(1, h * z2 // 2)
    for v in range(oh):
        sy = min(y0 + v * 2 // z2, y0 + h - 1)
        for u in range(ow):
            sx = min(x0 + u * 2 // z2, x0 + w - 1)
            i = (sy * W + sx) * 4
            out += fb[i:i + 3][::-1]
    return ow, oh, bytes(out)


def screen(srv, fb):
    err, text, imgs = srv.call("screenshot")
    w, h, px = png_decode(imgs[0])
    check(not err and (w, h) == (W, H) and px == expected(fb, 0, 0, W, H, 2)[2] and f"{W} x {H}" in text, "screenshot: the whole screen, exact")
    err, text, imgs = srv.call("screenshot", {"x": 10, "y": 20, "width": 50, "height": 30, "zoom": 3})
    w, h, px = png_decode(imgs[0])
    ew, eh, epx = expected(fb, 10, 20, 50, 30, 6)
    check(not err and (w, h) == (ew, eh) == (150, 90) and px == epx and "(10 + u / 3, 20 + v / 3)" in text, "screenshot: a region at zoom 3, exact")
    err, text, imgs = srv.call("screenshot", {"zoom": 0.5})
    w, h, px = png_decode(imgs[0])
    check((w, h) == (W // 2, H // 2) and px == expected(fb, 0, 0, W, H, 1)[2], "screenshot: half size")
    err, text, imgs = srv.call("screenshot", {"grid": True})
    w, h, px = png_decode(imgs[0])
    check(not err and px != expected(fb, 0, 0, W, H, 2)[2] and "every 100" in text, "screenshot: a grid is drawn")
    err, text, _ = srv.call("screenshot", {"x": 300, "y": 10, "width": 500, "zoom": "2"})
    check(not err and "x 300-319" in text, "screenshot: a region past the edge is cut to the screen; zoom as a string")
    err, text, _ = srv.call("screenshot", {"x": 400})
    check(err and "off the screen" in text, "screenshot: a region off the screen")
    err, text, _ = srv.call("screenshot", {"zoom": 5})
    check(err and "zoom must be" in text, "screenshot: a bad zoom")
    err, text, _ = srv.call("get_screen_info")
    info = json.loads(text.split("\n")[0])
    check(info["width"] == W and info["height"] == H and info["display"] == "ok" and info["png"] is True, f"get_screen_info {info}")


def modify_later(fbpath, x, y, delay):
    def go():
        time.sleep(delay)
        with open(fbpath, "r+b") as f:
            f.seek((y * W + x) * 4)
            f.write(b"\x01\x02\x03\xff")
    t = threading.Thread(target=go)
    t.start()
    return t


def touch(srv, touchpath, fbpath):
    n = len(events(touchpath))
    err, text, imgs = srv.call("tap", {"x": 100, "y": 50})
    ev = events(touchpath)[n:]
    w, h, _ = png_decode(imgs[0])
    check(not err and positions(ev) == [to_panel(100, 50)] and (1, 0x14a, 1) in ev and (1, 0x14a, 0) in ev, "tap: one press and lift at the point")
    check("The screen did not change" in text and (w, h) == (W // 2, H // 2), "tap: no change reported, half-size screenshot")
    t = modify_later(fbpath, 33, 44, 0.05)
    err, text, imgs = srv.call("tap", {"x": "5", "y": 5, "screenshot": "full"})
    t.join()
    check(not err and "changed in x 33-33, y 44-44" in text and png_decode(imgs[0])[:2] == (W, H), f"tap: where the screen changed ({text[:80]})")
    err, text, imgs = srv.call("tap", {"x": 5, "y": 5, "screenshot": "none"})
    check(not imgs, "tap: screenshot none")
    n = len(events(touchpath))
    t0 = time.time()
    err, text, _ = srv.call("long_press", {"x": 7, "y": 8, "duration_ms": 600, "screenshot": "none"})
    check(not err and time.time() - t0 >= 0.6 and positions(events(touchpath)[n:]) == [to_panel(7, 8)], "long_press holds")
    n = len(events(touchpath))
    srv.call("double_tap", {"x": 9, "y": 9, "screenshot": "none"})
    ev = events(touchpath)[n:]
    check(ev.count((1, 0x14a, 1)) == 2 and ev.count((1, 0x14a, 0)) == 2, "double_tap: two taps")
    n = len(events(touchpath))
    err, text, _ = srv.call("drag", {"from_x": 10, "from_y": 100, "to_x": 200, "to_y": 150, "duration_ms": 200, "hold_ms": 0, "screenshot": "none"})
    pos = positions(events(touchpath)[n:])
    check(not err and pos[0] == to_panel(10, 100) and pos[-1] == to_panel(200, 150) and len(pos) > 5, f"drag: from start to end in steps ({len(pos)})")
    n = len(events(touchpath))
    err, text, _ = srv.call("scroll", {"x": 160, "y": 100, "direction": "down", "amount": 100, "screenshot": "none"})
    pos = positions(events(touchpath)[n:])
    check(not err and pos[0] == to_panel(160, 150) and pos[-1] == to_panel(160, 50), "scroll down: the finger moves up")
    n = len(events(touchpath))
    srv.call("scroll", {"x": 5, "y": 100, "direction": "right", "amount": 1000, "screenshot": "none"})
    pos = positions(events(touchpath)[n:])
    check(all(0 <= p[0] <= to_panel(W - 1, 0)[0] for p in pos), "scroll: the stroke stays on the screen")
    n = len(events(touchpath))
    srv.call("touch", {"action": "down", "x": 1, "y": 2})
    srv.call("touch", {"action": "move", "x": 3, "y": 4})
    err, text, _ = srv.call("touch", {"action": "up"})
    ev = events(touchpath)[n:]
    check(positions(ev) == [to_panel(1, 2), to_panel(3, 4)] and ev.count((1, 0x14a, 0)) == 1 and "lifted" in text, "touch: down, move, up")
    err, text, _ = srv.call("wait_for_screen", {"until": "stable", "timeout_ms": 1000, "screenshot": "none"})
    check(not err and "still" in text, "wait_for_screen: stable")
    err, text, _ = srv.call("wait_for_screen", {"timeout_ms": 300, "screenshot": "none"})
    check(not err and "did not change in" in text, "wait_for_screen: no change before the timeout")
    t = modify_later(fbpath, 200, 100, 0.3)
    err, text, imgs = srv.call("wait_for_screen", {"timeout_ms": 3000})
    t.join()
    check(not err and "changed after" in text and "x 200-200, y 100-100" in text and imgs, f"wait_for_screen: a change ({text[:60]})")


def tool_errors(srv):
    for args, want in [({"y": 5}, "x is required"), ({"x": 1.5, "y": 1}, "whole number"), ({"x": W, "y": 0}, "off the screen"),
                       ({"x": 1, "y": 1, "screenshot": "maybe"}, "screenshot must be one of"),
                       ({"x": 1, "y": 1, "hold_ms": 5}, "hold_ms must be")]:
        err, text, _ = srv.call("tap", args)
        check(err and want in text, f"tap {args}: {text[:60]}")
    err, text, _ = srv.call("scroll", {"x": 1, "y": 1, "direction": "sideways"})
    check(err and "direction must be one of: up down left right" in text, "an enum error lists the choices")
    st, r = srv.rpc("tools/call", {"name": "tap", "arguments": [1, 2]})
    check(r["error"]["code"] == -32602, "arguments that aren't an object")


def filesystem(srv, root):
    err, text, _ = srv.call("list_files")
    check(not err and root + "/" in text, "list_files: the readable folders")
    err, text, _ = srv.call("list_files", {"path": root + "/Projects"})
    lines = text.splitlines()
    check(not err and lines[1] == "Beat/" and lines[2].startswith("Kick 01.WAV  ("), f"list_files: folders first {lines}")
    err, text, _ = srv.call("read_text_file", {"path": root + "/Projects/Beat/Beat.xpj", "max_bytes": 256})
    check(not err and "bytes 0-256 of 10008 (more" in text and "project\nline\n" in text, "read_text_file: a part, and how much is left")
    err, text, _ = srv.call("read_text_file", {"path": root + "/Projects/Beat/Beat.xpj", "offset": 10000})
    check(not err and "bytes 10000-10008 of 10008\n" in text, "read_text_file: from an offset")
    err, text, _ = srv.call("read_text_file", {"path": root + "/Projects/Kick 01.WAV"})
    check(err and "binary" in text, "read_text_file: a binary file is refused")
    err, text, _ = srv.call("read_text_file", {"path": root + "/latin1.txt"})
    check(not err and "caf� ok" in text, "read_text_file: bytes that aren't UTF-8 become U+FFFD")
    for p in ("/etc/passwd", root + "/../remote.conf", root + "/escape/passwd", root + "x/a"):
        err, text, _ = srv.call("read_text_file", {"path": p})
        check(err and ("outside" in text or "No such" in text), f"read_text_file {p} is refused: {text[:70]}")
    err, text, _ = srv.call("list_files", {"path": "/"})
    check(err and "outside" in text, "list_files / is refused")
    err, text, _ = srv.call("find_files", {"pattern": "*.xpj"})
    check(not err and "1 match(es)" in text and "Beat/Beat.xpj" in text, "find_files: every readable folder")
    err, text, _ = srv.call("find_files", {"pattern": "*kick*", "path": root})
    check(not err and "Kick 01.WAV" in text, "find_files: case-insensitive")
    err, text, _ = srv.call("find_files", {"pattern": "passwd", "path": root})
    check(not err and "0 match(es)" in text, "find_files doesn't follow a link out")


def midi(srv):
    if not (os.access("/dev/snd/seq", os.R_OK | os.W_OK) and shutil.which("aseqdump") and shutil.which("aseqsend")):
        print("skip mcp: MIDI (no ALSA sequencer, aseqdump or aseqsend here)")
        err, text, _ = srv.call("play_notes", {"notes": ["C3"]})
        check(err or "Played" in text, "play_notes answers without a sequencer")
        return
    err, text, _ = srv.call("midi_status")
    check(not err and '"MPC Remote"' in text and '"Out"' in text and "Cells" not in text, "midi_status")
    client = text.split("sequencer client ")[1].split(",")[0]          # by number: other "MPC Remote"s may exist
    dump = subprocess.Popen(["stdbuf", "-oL", "aseqdump", "-p", client + ":0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.5)
    err, text, _ = srv.call("play_notes", {"notes": ["C3", 64, "G#3"], "duration_ms": 50, "channel": 2})
    check(not err and "C3 (60) E3 (64) G#3 (68)" in text, f"play_notes: {text[:90]}")
    srv.call("play_notes", {"notes": ["Bb-1", "c4"], "mode": "one_by_one", "duration_ms": 20, "gap_ms": 10})
    srv.call("send_midi", {"type": "cc", "controller": 7, "value": 99, "channel": 16})
    srv.call("send_midi", {"type": "program_change", "value": 5})
    srv.call("send_midi", {"type": "pitch_bend", "bend": -8192})
    srv.call("send_midi", {"type": "start"})
    err, text, _ = srv.call("send_midi", {"type": "mmc", "mmc": "locate_start"})
    check(not err and "F0 7F 7F 06 44 06 01 00 00 00 00 00 F7" in text, f"send_midi mmc: {text}")
    err, text, _ = srv.call("send_midi", {"type": "sysex", "sysex": "F0 7E 7F 06 01 F7"})
    check(not err, "send_midi sysex")
    err, text, _ = srv.call("send_midi", {"type": "sysex", "sysex": "F0 80 F7"})
    check(err, "send_midi: a sysex data byte over 7F is refused")
    err, text, _ = srv.call("play_notes", {"notes": ["H2"]})
    check(err and "C3 = 60" in text, "play_notes: a bad note name")
    time.sleep(0.5)
    dump.terminate()
    out = dump.communicate(timeout=5)[0]
    for want in ("Note on                 1, note 60, velocity 100", "Note on                 1, note 64",
                 "Note on                 1, note 68", "Note off                1, note 60",
                 "Note on                 0, note 22", "Note on                 0, note 72",
                 "Control change         15, controller 7, value 99", "Program change          0, program 5",
                 "Pitch bend              0, value -8192", "Start", "System exclusive"):
        check(want in out, f"aseqdump saw: {want}")
    def send_later():
        time.sleep(0.4)
        subprocess.run(["aseqsend", "-p", client + ":1", "90 3C 64 B0 01 20 80 3C 00"], check=False)
    t = threading.Thread(target=send_later)
    t.start()
    err, text, _ = srv.call("midi_listen", {"duration_ms": 1500})
    t.join()
    check(not err and "3 message(s)" in text and "note_on  ch 1  C3 (60)  velocity 100" in text
          and "cc ch 1  controller 1  value 32" in text and "note_off ch 1  C3 (60)" in text, f"midi_listen: {text}")


if __name__ == "__main__":
    main(sys.argv[1])
    print(f"mcp: {'all passed' if not fails else f'{fails} FAILED'}")
    sys.exit(1 if fails else 0)
