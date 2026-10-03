#!/usr/bin/env python3
"""Writes src/mcp_tools.h, the MCP tools/list reply, from the definitions below. Edit here, run it, commit both.
tests/test.sh checks the header is up to date (--check)."""
import json, os, sys

COORD = "Pixels of the full-size screen, origin top left (see get_screen_info), whatever zoom a screenshot was taken at."
SHOT = {"type": "string", "enum": ["half", "full", "none"], "default": "half",
        "description": "The screenshot returned once the screen has settled: half size (the default, enough to see "
                       "what happened), full size, or none."}
NO_SHOT = {**SHOT, "default": "none", "description": "A screenshot afterwards: none (the default), half or full size."}
LISTEN = {"type": "integer", "minimum": 0, "maximum": 10000, "default": 0,
          "description": "Afterwards, also collect this long what arrives on the addin's MIDI input (MPC Remote In), "
                         "for example what a MIDI-generating plugin sends back."}
CHANNEL = {"type": "integer", "minimum": 1, "maximum": 16, "default": 1, "description": "MIDI channel, 1 to 16."}
NOTE = {"oneOf": [{"type": "integer", "minimum": 0, "maximum": 127},
                  {"type": "string", "description": "A name: C3, C#3, Db3 ... (C3 = 60, as MPC names notes; octaves -2 to 8)."}]}


def xy(what="Where to touch"):
    return {"x": {"type": "integer", "minimum": 0, "description": f"{what}: x. {COORD}"},
            "y": {"type": "integer", "minimum": 0, "description": f"{what}: y. {COORD}"}}


def obj(props, req=()):
    o = {"type": "object", "properties": props, "additionalProperties": False}
    if req:
        o["required"] = list(req)
    return o


def ann(title, read_only):
    return {"title": title, "readOnlyHint": read_only, "destructiveHint": not read_only,
            "idempotentHint": read_only, "openWorldHint": False}


CHANGED = " Returns whether the screen changed (and where) and a screenshot once it has settled."
MIDI_PORT = ("The addin plays through its own MIDI port, \"MPC Remote Out\": MPC adds it as a MIDI input by itself, "
             "and the track plays it when the port is enabled for tracks (Preferences > MIDI) and the track's MIDI "
             "input is that port or All, on the channel used (or All).")

TOOLS = [
    ("screenshot", "Screenshot", True,
     "See the MPC's screen now: a PNG of the whole screen, or of a region of it, at a zoom. All coordinates (here "
     "and in tap, drag, scroll) are pixels of the full-size screen, origin top left, whatever the zoom: "
     "get_screen_info gives its size (1280 x 800 on most models). grid=true draws lines labelled with those "
     "coordinates, to aim a touch. To read small text, zoom into a region (for example width=400, height=250, zoom=2).",
     obj({"x": {"type": "integer", "minimum": 0, "default": 0, "description": "Left edge of the region, in screen pixels."},
          "y": {"type": "integer", "minimum": 0, "default": 0, "description": "Top edge of the region, in screen pixels."},
          "width": {"type": "integer", "minimum": 1, "description": "Region width in screen pixels (default: to the right edge)."},
          "height": {"type": "integer", "minimum": 1, "description": "Region height in screen pixels (default: to the bottom edge)."},
          "zoom": {"type": "number", "enum": [0.5, 1, 2, 3, 4], "default": 1,
                   "description": "Image pixels per screen pixel: 0.5 is half size, 2 to 4 magnify a region. The "
                                  "picture may be at most 2560 pixels on a side."},
          "grid": {"type": "boolean", "default": False,
                   "description": "Draw lines labelled with screen coordinates over the picture."}})),
    ("tap", "Tap", False,
     "Tap the touchscreen at (x, y) with one finger, as a person would: buttons, tabs, menu and list items, pads and "
     "fields on the screen." + CHANGED + " Only the touchscreen is reachable this way: for notes use play_notes.",
     obj({**xy(), "hold_ms": {"type": "integer", "minimum": 10, "maximum": 4000, "default": 80,
                              "description": "How long the finger stays down."},
          "screenshot": SHOT}, ("x", "y"))),
    ("double_tap", "Double tap", False,
     "Two quick taps at (x, y), for whatever responds to a double tap." + CHANGED,
     obj({**xy(), "screenshot": SHOT}, ("x", "y"))),
    ("long_press", "Long press", False,
     "Press and hold at (x, y), then lift: for whatever opens on a long press." + CHANGED,
     obj({**xy(), "duration_ms": {"type": "integer", "minimum": 300, "maximum": 10000, "default": 1000,
                                  "description": "How long to hold."},
          "screenshot": SHOT}, ("x", "y"))),
    ("drag", "Drag", False,
     "Press at (from_x, from_y), slide to (to_x, to_y) over duration_ms, and lift: for sliders, faders, knobs and "
     "value fields that follow a finger, moving things, and other gestures. For lists and pages, scroll is simpler."
     + CHANGED,
     obj({"from_x": {"type": "integer", "minimum": 0, "description": "Start x. " + COORD},
          "from_y": {"type": "integer", "minimum": 0, "description": "Start y. " + COORD},
          "to_x": {"type": "integer", "minimum": 0, "description": "End x. " + COORD},
          "to_y": {"type": "integer", "minimum": 0, "description": "End y. " + COORD},
          "duration_ms": {"type": "integer", "minimum": 50, "maximum": 10000, "default": 400,
                          "description": "Time to slide from start to end; slower is more precise."},
          "hold_ms": {"type": "integer", "minimum": 0, "maximum": 3000, "default": 100,
                      "description": "Time to hold still at the start before sliding (some controls need a press first)."},
          "screenshot": SHOT}, ("from_x", "from_y", "to_x", "to_y"))),
    ("scroll", "Scroll", False,
     "Scroll the list or page under (x, y). direction is what you want to see more of (down shows what is below, "
     "as with a mouse wheel); the finger moves the other way, about `amount` screen pixels, and pauses before "
     "lifting so the list doesn't fling." + CHANGED,
     obj({**xy("A point inside the list or page"),
          "direction": {"type": "string", "enum": ["up", "down", "left", "right"], "description": "Which way to see more of."},
          "amount": {"type": "integer", "minimum": 20, "maximum": 2000, "default": 300,
                     "description": "How far to move the content, in screen pixels (the finger stays on the screen)."},
          "screenshot": SHOT}, ("x", "y", "direction"))),
    ("touch", "Touch (low level)", False,
     "One step of a gesture the other tools don't cover: down puts a finger on (x, y), move slides it to (x, y), up "
     "lifts it. Always finish with up. Prefer tap, long_press, drag and scroll.",
     obj({"action": {"type": "string", "enum": ["down", "move", "up"]}, **xy("Where (needed for down and move)"),
          "screenshot": NO_SHOT}, ("action",))),
    ("wait_for_screen", "Wait for the screen", True,
     "Wait until the screen changes (until=change: for something you started, such as a page loading) or stops "
     "changing (until=stable: an animation, meters or a progress bar), at most timeout_ms; then say whether it did "
     "and return a screenshot.",
     obj({"until": {"type": "string", "enum": ["change", "stable"], "default": "change"},
          "timeout_ms": {"type": "integer", "minimum": 100, "maximum": 20000, "default": 3000},
          "screenshot": SHOT})),
    ("get_screen_info", "Screen info", True,
     "The screen's size in pixels (the coordinate space of the touch tools), whether the display and the "
     "touchscreen were found, and this addin's version.",
     obj({})),
    ("play_notes", "Play notes", False,
     "Play notes into MPC, as a MIDI keyboard would: a chord (together) or one after another. " + MIDI_PORT +
     " On a drum program the notes trigger the pads mapped to them. Takes duration plus gaps; at most 30 s.",
     obj({"notes": {"type": "array", "items": NOTE, "minItems": 1, "maxItems": 64,
                    "description": "Note numbers 0-127 or names (C3 = 60), for example [\"C3\", \"E3\", \"G3\"]."},
          "mode": {"type": "string", "enum": ["together", "one_by_one"], "default": "together",
                   "description": "together: a chord; one_by_one: each note in turn."},
          "velocity": {"type": "integer", "minimum": 1, "maximum": 127, "default": 100},
          "duration_ms": {"type": "integer", "minimum": 10, "maximum": 10000, "default": 400,
                          "description": "How long each note (or the chord) is held."},
          "gap_ms": {"type": "integer", "minimum": 0, "maximum": 5000, "default": 50,
                     "description": "one_by_one: the silence between notes."},
          "channel": CHANNEL, "listen_ms": LISTEN, "screenshot": NO_SHOT}, ("notes",))),
    ("send_midi", "Send MIDI", False,
     "Send one MIDI message into MPC: a note on or off, a control change, a program change, pitch bend, "
     "aftertouch, transport (start, stop, continue: MPC follows them only when it is set to receive MIDI clock), "
     "an MMC transport command (MPC follows those only when it is set to receive MMC), or raw system exclusive. "
     + MIDI_PORT,
     obj({"type": {"type": "string", "enum": ["note_on", "note_off", "cc", "program_change", "pitch_bend",
                                              "channel_pressure", "poly_pressure", "start", "stop", "continue",
                                              "clock", "mmc", "sysex"]},
          "channel": CHANNEL,
          "note": {**NOTE, "description": "note_on, note_off, poly_pressure: the note."},
          "velocity": {"type": "integer", "minimum": 0, "maximum": 127, "default": 100,
                       "description": "note_on, note_off: the velocity."},
          "controller": {"type": "integer", "minimum": 0, "maximum": 127, "description": "cc: the controller number."},
          "value": {"type": "integer", "minimum": 0, "maximum": 127,
                    "description": "cc: the value; program_change: the program (0-127; many devices show it as 1-128); "
                                   "channel_pressure, poly_pressure: the pressure."},
          "bend": {"type": "integer", "minimum": -8192, "maximum": 8191, "default": 0,
                   "description": "pitch_bend: -8192 (down) to 8191 (up), 0 is the centre."},
          "mmc": {"type": "string", "enum": ["play", "stop", "deferred_play", "fast_forward", "rewind",
                                             "record_strobe", "record_exit", "pause", "locate_start"],
                  "description": "mmc: the command."},
          "sysex": {"type": "string", "description": "sysex: the whole message in hex, F0 ... F7, for example \"F0 7E 7F 06 01 F7\"."},
          "listen_ms": LISTEN}, ("type",))),
    ("midi_listen", "Listen to MIDI", True,
     "Collect the MIDI that arrives on the addin's input port, \"MPC Remote In\", for duration_ms: set a track's "
     "MIDI output (or a plugin's) to it in MPC first. Lists each message with its time.",
     obj({"duration_ms": {"type": "integer", "minimum": 100, "maximum": 30000, "default": 3000}})),
    ("midi_status", "MIDI status", True,
     "The addin's MIDI ports and what is connected to them (whether MPC has picked up MPC Remote Out yet), and the "
     "other MIDI ports on the device.",
     obj({})),
    ("device_status", "Device status", True,
     "The device now: OS and kernel, uptime, load and CPU use (all cores and MPC's own), memory, temperatures, free "
     "space on each storage, and network addresses.",
     obj({})),
    ("list_files", "List files", True,
     "List a folder on the device's storage (projects, samples, programs, settings): folders first, then files with "
     "size and date. With no path, lists the folders files can be read from. Read-only.",
     obj({"path": {"type": "string", "description": "An absolute folder path, inside the readable folders."}})),
    ("find_files", "Find files", True,
     "Find files and folders by name under a folder (or every readable folder): a shell pattern, case-insensitive, "
     "for example \"*.xpj\" (projects), \"*.xpm\" (programs), \"*kick*.wav\".",
     obj({"pattern": {"type": "string", "description": "A name pattern with * and ?."},
          "path": {"type": "string", "description": "Where to search (default: every readable folder)."},
          "max_results": {"type": "integer", "minimum": 1, "maximum": 500, "default": 100}}, ("pattern",))),
    ("read_text_file", "Read a text file", True,
     "Read a text file on the device's storage (settings, logs, program or project files that are text), in parts "
     "of up to max_bytes from offset. Binary files are refused. Read-only.",
     obj({"path": {"type": "string", "description": "An absolute file path, inside the readable folders."},
          "offset": {"type": "integer", "minimum": 0, "default": 0},
          "max_bytes": {"type": "integer", "minimum": 256, "maximum": 65536, "default": 16384}}, ("path",))),
]


def build():
    tools = [{"name": n, "title": t, "description": d, "inputSchema": s, "annotations": ann(t, ro)}
             for n, t, ro, d, s in TOOLS]
    out = ["// MCP tool definitions: the tools/list reply. Generated by tools/gen_mcp_tools.py; edit that, not this.",
           "static const char TOOLS_JSON[] =", '    "["']
    for i, t in enumerate(tools):
        s = json.dumps(t, separators=(",", ":"), ensure_ascii=True).replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "' + s + ("," if i < len(tools) - 1 else "") + '"')
    out.append('    "]";')
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "mcp_tools.h")
    text = build()
    if sys.argv[1:] == ["--check"]:
        ok = os.path.exists(path) and open(path).read() == text
        print("ok   src/mcp_tools.h is up to date" if ok else "FAIL src/mcp_tools.h is stale: run tools/gen_mcp_tools.py")
        sys.exit(0 if ok else 1)
    open(path, "w").write(text)
    print(f"{path}: {len(TOOLS)} tools")
