# mpc-preload-addin-remote

The screen and touchscreen of an Akai MPC OS standalone device (MPC Live/One/X/Key, Force) in a web browser: watch
the screen live and drive it with a mouse or a finger. It's an **add-in**: a small shared library that MPC loads at
start through `LD_PRELOAD`, running inside the MPC process. Nothing else is installed and no system binary is
changed.

**Status:** passes the offline tests (x86: unit, the HTTP server under ASan+UBSan and TSan, the preload gate, the
installer under busybox) and builds for armhf (glibc symbols up to 2.17).

## What it does

- `http://<device>:8080/`: the live screen, scaled to the window. Click or drag on it to touch, with a half
  resolution option for slow networks.
- An HTTP API for scripts (the same routes as the earlier stand-alone `mpc-remote`, so its scripts keep working):

| Route | |
|---|---|
| `GET /screen.png[?half=1]` | one frame as PNG |
| `GET /stream[?half=1&fps=N]` | frames as `multipart/x-mixed-replace` PNGs (an unchanged screen sends nothing) |
| `GET /tap?x=&y=[&hold=ms]` | touch and release (hold 10 to 4000 ms, default 90) |
| `GET /down?x=&y=`, `/move?x=&y=`, `/up` | a drag |
| `GET /info` | JSON: screen size and pixel format, capture state, the touch device, rotations, the last frame's capture and encode times |

Coordinates are screen pixels (`/info` gives the size).

## How it works

- **Screen:** DRM/KMS, not `/dev/mem`. A root process may get a handle to any framebuffer (`GETFB2`, needs
  `CAP_SYS_ADMIN`) on its own DRM file and map it read-only (`MAP_DUMB`). The add-in finds the lit CRTC on
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
  mouse add-in's touch device, which has no multitouch axes, is skipped. Its axis ranges are read from the device. Touches are mapped from the upright picture back into the scanout, which is the touch
  panel's own frame, so normally no touch rotation is needed (`touch_rotate` is there for a panel mounted
  otherwise).

### Living inside MPC

An add-in shares MPC's process, so it is written so that it can't hurt MPC:
- it starts only in the process whose executable is named `MPC`. The launch script and anything else that
  inherits `LD_PRELOAD` load it and do nothing (`MPC_REMOTE_ADDIN_DISABLE=1` also keeps it off);
- it runs only on its own threads: normal scheduling even when created from a real-time thread, nice 10 by
  default, every signal blocked (MPC's signals still reach MPC's threads), 256 KB stacks;
- no process-wide changes: no `signal()`, `send(MSG_NOSIGNAL)` instead of ignoring `SIGPIPE`, every descriptor
  close-on-exec;
- bounded work: at most `max_clients` connections and two streams, 5 s socket timeouts, frames capped at `max_fps`;
- it exports one symbol, `mpc_remote_addin_start`, so it can't shadow anything MPC or another add-in uses;
- if the port is taken it retries every 10 s rather than failing; if no display or touchscreen is found, the rest
  still works.

## Settings

`mpc_remote_addin.conf` next to the `.so` (or the file `$MPC_REMOTE_ADDIN_CONF` names), `key=value`:

| Key | Default | |
|---|---|---|
| `enabled` | 1 | 0 keeps it loaded but idle |
| `bind` | 0.0.0.0 | listen address. **There is no login:** anyone who can reach the port sees and touches the screen. Use `127.0.0.1` and an SSH tunnel on a network you don't trust |
| `port` | 8080 | |
| `touch_device` | auto | or `/dev/input/eventN` |
| `screen_rotate` | auto | the clockwise turn that shows the scanout upright: auto (portrait turns 90), 0, 90, 180, 270 |
| `touch_rotate` | 0 | how the touch panel sits against the scanout: 0, 90, 180 or 270 |
| `max_fps` | 10 | stream frame cap (1 to 60) |
| `max_clients` | 6 | connections at once |
| `nice` | 10 | the add-in threads' nice value (0 to 19) |

Settings are read when MPC starts.

## Install

Needs root on the device (SSH). Copy `build/` (or a release folder) to the device and run:

```sh
sh install.sh            # asks first; -y doesn't ask, -n doesn't restart MPC, -t <folder> installs elsewhere
sh uninstall.sh
```

It installs into `/data/mpc-addins/remote/` (the root file system is often nearly full) and **adds** the `.so` to
`LD_PRELOAD` in MPC's systemd service (`acvs`, or `inmusic-mpc`). Other add-ins already in it stay. Where the service
already sets `LD_PRELOAD`, the installer edits that line in place and keeps a `.bak-remote-addin` copy. It doesn't
add a drop-in, because a second `Environment=LD_PRELOAD=` replaces the whole list and silently drops the other
add-ins. Where nothing sets it, a drop-in does. Reinstalling keeps your settings file. Uninstalling removes only
this add-in from the list.

If `/data` isn't mounted when MPC starts, the loader prints a warning and MPC starts without the add-in.

### Trying it without restarting MPC

DRM capture and touch injection work from any root process, so `build/standalone` runs the add-in in a process of
its own, next to a running MPC:

```sh
printf 'port=8090\nbind=127.0.0.1\n' > /tmp/remote.conf
MPC_REMOTE_ADDIN_CONF=/tmp/remote.conf ./standalone ./mpc_remote_addin.so
```

## Building and testing

```sh
./build.sh          # build/mpc_remote_addin.so and build/standalone (armhf, Docker + QEMU, glibc <= 2.31 checked)
tests/test.sh       # offline, on the build machine (x86)
```

`tests/test.sh` runs the unit tests, then `tests/test_remote.py` against the server built with ASan+UBSan and
again with TSan. Test builds (`-DREMOTE_TEST`) can stand a raw file in for the display (`REMOTE_FAKE_FB`) and a
file in for the touchscreen (`REMOTE_FAKE_TOUCH`); release builds can't. The test then preloads the real `.so` into
a process named `MPC` and one named otherwise, and runs `tests/test_install.sh` against scratch systemd layouts
(`BUSYBOX=/path/to/busybox` runs it in the device's shell).

## Not included

The hardware buttons, pads and Q-Links (they arrive as MIDI from the control surface, not as input events),
audio, and any login.

## License

MIT, see [LICENSE](LICENSE).
