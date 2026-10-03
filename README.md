# mpc-addin-remote

The screen and touchscreen of an Akai MPC OS standalone device (MPC Live/One/X/Key, Force) in a web browser: watch
the screen live and drive it with a mouse or a finger. It's an **addin**: a small shared library that MPC loads at
start through `LD_PRELOAD`, running inside the MPC process. Nothing else is installed and no system binary is
changed.

It is also an **MCP server** (`/mcp`), so an AI model can see the screen, touch it, play MIDI into
MPC and read the device's status and files.

**Status:** passes the offline tests (x86: unit, the HTTP server and the MCP endpoint under ASan+UBSan and TSan, the
official MCP Python SDK as a client, the preload gate, the installer under busybox) and builds for armhf (glibc
symbols up to 2.31). Runs on an MPC: the HTTP routes, the web page and every MCP tool.

## Connecting: an SSH tunnel

The addin listens on the device only (`bind=127.0.0.1`): it has no login, and it runs as root inside MPC. To reach it
from a computer, open an SSH tunnel and leave it running:

```sh
ssh -N -L 6720:127.0.0.1:6720 root@<device address>
```

Then use `http://localhost:6720` on that computer, as in the rest of this file. To open it to your network instead,
set `bind=0.0.0.0` in the settings: anyone who can reach the port can then see and touch the screen, send MIDI into
MPC and read the `mcp_files` folders.

## What it does

- `http://localhost:6720/`: the live screen, scaled to the window. Click or drag on it to touch, with a half
  resolution option for slow networks.
- An HTTP API for scripts:

| Route | |
|---|---|
| `GET /screen.png[?half=1]` | one frame as PNG |
| `GET /stream[?half=1&fps=N]` | frames as `multipart/x-mixed-replace` PNGs (an unchanged screen sends nothing once its frame has gone out twice) |
| `GET /tap?x=&y=[&hold=ms]` | touch and release (hold 10 to 4000 ms, default 90) |
| `GET /down?x=&y=`, `/move?x=&y=`, `/up` | a drag |
| `GET /info` | JSON: screen size and pixel format, capture state, the touch device, rotations, the last frame's capture and encode times |

Coordinates are screen pixels (`/info` gives the size).

## MCP (for AI models)

`POST /mcp` speaks the Model Context Protocol (Streamable HTTP, a JSON reply per request, protocol versions
2025-03-26 to 2025-11-25). Add it to a client, for example Claude Code:

```sh
claude mcp add --transport http mpc http://localhost:6720/mcp   # with the tunnel open
```

Tools:

| Tool | |
|---|---|
| `screenshot` | the screen, or a region of it, at zoom 0.5 to 4, optionally with a grid labelled in screen pixels |
| `tap`, `double_tap`, `long_press` | touch a point |
| `drag` | slide a finger from one point to another (faders, knobs, moving things) |
| `scroll` | swipe a list or page up, down, left or right |
| `touch` | one step of a gesture of its own: finger down, move or up |
| `wait_for_screen` | wait until the screen changes (or stops changing) |
| `get_screen_info` | the screen size, whether the display and touchscreen were found, the version |
| `play_notes` | play notes or chords (names like `C3`, `F#2`, or numbers) with a length and velocity, through MPC's MIDI input |
| `send_midi` | any MIDI message: CC, program change, pitch bend, pressure, transport (start/stop/continue/clock), MMC, raw SysEx |
| `midi_listen` | collect what MPC sends to the addin's MIDI input port for a while |
| `midi_status` | the addin's MIDI port and the device's MIDI clients and connections |
| `device_status` | OS, uptime, CPU (and MPC's share), memory, temperatures, storage, network addresses |
| `list_files`, `find_files`, `read_text_file` | browse and read text files under `mcp_files` (read-only) |

Every coordinate, at any zoom, is a pixel of the full-size upright screen. Touch tools wait for the screen to
settle, say which part of it changed, and return a screenshot, so a model sees the result of each touch.

**MIDI:** the addin opens an ALSA sequencer client, "MPC Remote", with an output port (notes and messages to MPC) and
an input port (what MPC sends back). MPC sees a new port at once; to play a track from it, enable it for tracks in
Preferences > MIDI (or MIDI Devices), and pick it as the track's input if the track isn't set to all. Note names put
middle C (60) at C3.

**Files:** `list_files`, `find_files` and `read_text_file` see only the folders in `mcp_files` (symbolic links that
lead out of them are refused). Nothing can be written or deleted through MCP.

**Security:** like the rest of the server there is no login. A request with an `Origin` header must come from the
same host, and the `Host` must be an IP address, `localhost` or a `.local` name, so a web page can't reach the
endpoint through DNS rebinding. Bodies are limited to 64 KB. `mcp=0` turns the endpoint off.

## How it works

- **Screen:** DRM/KMS, not `/dev/mem`. A root process may get a handle to any framebuffer (`GETFB2`, needs
  `CAP_SYS_ADMIN`) on its own DRM file and map it read-only (`MAP_DUMB`). The addin finds the lit CRTC on
  `/dev/dri/card*` and asks it for its framebuffer on every frame, so page flips are followed. Size and pixel format
  (XRGB/XBGR 8888, RGB565) come from the device; tiled or compressed buffers are refused. The scanout is the
  physical panel, which may be portrait with the UI drawn into it on its side (the MPC Key 37's is 800 x 1280):
  `screen_rotate` turns it upright (auto: a portrait scanout turns 90 degrees, since MPC's UI is landscape). The DRM ioctls are
  written out in `src/drm_min.h`, so the build needs no kernel or libdrm headers.
- **PNG:** zlib, loaded at run time (`libz.so.1`, already loaded by MPC); level 1. Before encoding, a CRC of the
  frame is compared with the last one sent, so a still screen costs a capture and a CRC, not an encode.
- **Touch:** events written into the touchscreen's own evdev node, which MPC reads like a real finger (slot 0 of
  multitouch protocol B, plus `BTN_TOUCH` and `ABS_X/Y`). The device is found automatically: the first
  multitouch device that isn't virtual, preferring one that says it is on the screen (not every driver does), so a
  mouse addin's touch device, which has no multitouch axes, is skipped. Its axis ranges are read from the device. Touches are mapped from the upright picture back into the scanout, which is the touch
  panel's own frame, so normally no touch rotation is needed (`touch_rotate` is there for a panel mounted
  otherwise).

### Living inside MPC

An addin shares MPC's process, so it is written so that it can't hurt MPC:
- it starts only in the process whose executable is named `MPC`. The launch script and anything else that
  inherits `LD_PRELOAD` load it and do nothing (`MPC_REMOTE_ADDIN_DISABLE=1` also keeps it off);
- it runs only on its own threads: normal scheduling even when created from a real-time thread, nice 10 by
  default, every signal blocked (MPC's signals still reach MPC's threads), 256 KB stacks;
- no process-wide changes: no `signal()`, `send(MSG_NOSIGNAL)` instead of ignoring `SIGPIPE`, every descriptor
  close-on-exec;
- bounded work: at most `max_clients` connections and two streams, 5 s socket timeouts, frames capped at `max_fps`;
- it exports one symbol, `mpc_remote_addin_start`, so it can't shadow anything MPC or another addin uses;
- if the port is taken it retries every 10 s rather than failing; if no display or touchscreen is found, the rest
  still works.

## Settings

`mpc_remote_addin.conf` next to the `.so` (or the file `$MPC_REMOTE_ADDIN_CONF` names), `key=value`:

| Key | Default | |
|---|---|---|
| `enabled` | 1 | 0 keeps it loaded but idle |
| `bind` | 127.0.0.1 | listen address: the device only, reached through an SSH tunnel. `0.0.0.0` is every interface. **There is no login:** anyone who can reach the port sees and touches the screen |
| `port` | 6720 | |
| `touch_device` | auto | or `/dev/input/eventN` |
| `screen_rotate` | auto | the clockwise turn that shows the scanout upright: auto (portrait turns 90), 0, 90, 180, 270 |
| `touch_rotate` | 0 | how the touch panel sits against the scanout: 0, 90, 180 or 270 |
| `max_fps` | 10 | stream frame cap (1 to 60) |
| `max_clients` | 6 | connections at once |
| `nice` | 10 | the addin threads' nice value (0 to 19) |
| `mcp` | 1 | 0 turns the MCP endpoint (`/mcp`) off |
| `mcp_files` | /media,/sdcard,/data/mpc-addins | the folders MCP's file tools may read, comma-separated absolute paths (at most 8), or `none` |

Settings are read when MPC starts.

## Install

Needs root on the device (SSH). Unzip a release (`MPC-Remote-addin-<version>-mpc-armv7.zip`) on the device and run, as root,
in its folder:

```sh
sh install.sh            # asks first; -y doesn't ask, -n doesn't restart MPC, -t <folder> installs elsewhere
sh /data/mpc-addins/remote/uninstall.sh   # later, to remove it
```

It installs into `/data/mpc-addins/remote/` (the root file system is often nearly full) and **adds** the `.so` to
`LD_PRELOAD` in MPC's systemd service (`acvs`, or `inmusic-mpc`); other addins already in it stay. Reinstalling keeps
your settings file; uninstalling takes only this addin out of the list. The scripts are the shared addin
installer from [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (`tools/release/addin`; identical in every
addin, `addin.manifest` describes this one). Its `docs/ADDINS.md` has the rules: why it edits the line that takes effect rather than adding a second `LD_PRELOAD=`, the backup
it keeps, and the drop-in shared by all addins.

If `/data` isn't mounted when MPC starts, the loader prints a warning and MPC starts without the addin.

### Trying it without restarting MPC

DRM capture and touch injection work from any root process, so `build/standalone` runs the addin in a process of
its own, next to a running MPC:

```sh
printf 'port=8090\nbind=127.0.0.1\n' > /tmp/remote.conf
MPC_REMOTE_ADDIN_CONF=/tmp/remote.conf ./standalone ./mpc_remote_addin.so
```

## Building and testing

```sh
./build.sh          # build/mpc_remote_addin.so and build/standalone (armhf, Docker + QEMU, glibc <= 2.31 checked)
tests/test.sh       # offline, on the build machine (x86)
tools/release.sh 1.0.0   # dist/MPC-Remote-addin-1.0.0-mpc-armv7.zip, with the installer, checked as the catalog does
```

`tests/test.sh` runs the unit tests, then `tests/test_remote.py` and `tests/test_mcp.py` against the server built
with ASan+UBSan and again with TSan, then `tests/test_mcp_sdk.py` (the official MCP Python SDK as the client; set
`MCP_PYTHON` to a Python that has the `mcp` package, otherwise it is skipped). The MIDI tests need the host's ALSA
sequencer (`aseqdump`, `aseqsend`) and skip without it. `tools/gen_mcp_tools.py` writes the tool definitions into
`src/mcp_tools.h`; the tests check that it is up to date. Test builds (`-DREMOTE_TEST`) can stand a raw file in for the display (`REMOTE_FAKE_FB`) and a
file in for the touchscreen (`REMOTE_FAKE_TOUCH`); release builds can't. The test then preloads the real `.so` into
a process named `MPC` and one named otherwise, and runs `tests/test_install.sh`: the shared installer with this manifest against a scratch systemd tree
(`BUSYBOX=/path/to/busybox` runs it in the device's shell). The installer and the release tool come from
mpc-vst-plugins checked out next to this repo (or `MPC_VST=/path`).

CI (`.github/workflows`): `test.yml` runs the tests and the armhf build on every push; `release.yml` (Actions > Release, with the version) builds the zip and attaches it to a draft release, to publish once it has been tried on a device.

## Not included

Pressing the hardware buttons, pads and Q-Links (they arrive as MIDI from the control surface, not as input
events; MCP's `play_notes` plays notes instead), audio, and any login.

## License

MIT, see [LICENSE](LICENSE).
