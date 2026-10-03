#!/usr/bin/env python3
"""The MCP endpoint through the official MCP Python SDK's client (pip/uv package "mcp"), as a model's client would
use it: the version probe and handshake, tools/list, and calls whose results the SDK must validate.
Usage: test_mcp_sdk.py <host_main binary>. tests/test.sh runs it with $MCP_PYTHON (a Python that has "mcp")."""
import asyncio, os, socket, subprocess, sys, tempfile, time

fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + "sdk: " + what)
    fails += not cond


async def run(url):
    from mcp import Client
    for mode in ("auto", "legacy"):            # auto probes server/discover first, then falls back to initialize
        async with Client(url, mode=mode) as c:
            tools = await c.list_tools()
            names = {t.name for t in tools.tools}
            check({"screenshot", "tap", "drag", "scroll", "play_notes", "device_status"} <= names, f"{mode}: tools/list ({len(names)} tools)")
            r = await c.call_tool("screenshot", {"zoom": 0.5, "grid": True})
            kinds = [x.type for x in r.content]
            check(not r.is_error and "image" in kinds and r.content[0].mime_type == "image/png", f"{mode}: screenshot is an image ({kinds})")
            r = await c.call_tool("tap", {"x": 10, "y": 10, "screenshot": "none"})
            check(not r.is_error and "Tapped (10, 10)" in r.content[0].text, f"{mode}: tap: {r.content[0].text}")
            r = await c.call_tool("tap", {"x": 99999, "y": 1})
            check(r.is_error and "off the screen" in r.content[0].text, f"{mode}: a bad tap is a tool error the model can read")


def main():
    try:
        import mcp  # noqa: F401
    except ImportError:
        print("skip sdk: the mcp package isn't installed (set MCP_PYTHON to a Python that has it)")
        return 0
    exe = sys.argv[1]
    tmp = tempfile.mkdtemp()
    W, H = 160, 100
    open(os.path.join(tmp, "fb"), "wb").write(bytes(range(256)) * (W * H * 4 // 256))
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    open(os.path.join(tmp, "conf"), "w").write(f"port={port}\nbind=127.0.0.1\nmcp_files={tmp}\n")
    env = dict(os.environ, REMOTE_FAKE_FB=f"{tmp}/fb,{W},{H}", REMOTE_FAKE_TOUCH=f"{tmp}/touch", MPC_REMOTE_ADDIN_CONF=f"{tmp}/conf")
    p = subprocess.Popen([exe], env=env, stderr=subprocess.PIPE, text=True)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)
        asyncio.run(run(f"http://127.0.0.1:{port}/mcp"))
    finally:
        p.terminate()
        _, err = p.communicate(timeout=10)
    bad = [l for l in err.splitlines() if "ERROR" in l or "runtime error" in l or "WARNING: ThreadSanitizer" in l]
    check(not bad, "no sanitizer reports" + "".join("\n     " + l for l in bad[:5]))
    print(f"sdk: {'all passed' if not fails else f'{fails} FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
