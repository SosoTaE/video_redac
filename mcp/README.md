# video_redac MCP server

Exposes the renderer to an MCP client: author a JSON scene, check it, look at a
frame, render it.

## Install

```bash
make                 # or: make CPU=1   (no NVIDIA GPU / no CUDA toolkit)
python3 -m venv mcp/.venv
mcp/.venv/bin/pip install -r mcp/requirements.txt
```

## Register

```bash
claude mcp add video-redac -- /home/sosotae/Documents/video_redac/mcp/.venv/bin/python \
                               /home/sosotae/Documents/video_redac/mcp/server.py
```

Or by hand, in `~/.claude.json` under `mcpServers`:

```json
"video-redac": {
  "type": "stdio",
  "command": "/home/sosotae/Documents/video_redac/mcp/.venv/bin/python",
  "args": ["/home/sosotae/Documents/video_redac/mcp/server.py"],
  "env": { "VIDEO_REDAC_OUT": "/home/sosotae/.video_redac/out" }
}
```

| Variable | Default | Meaning |
|---|---|---|
| `VIDEO_REDAC_BIN` | `../video_redac` | the engine binary |
| `VIDEO_REDAC_OUT` | `~/.video_redac/out` | where renders and previews are written |

## Tools

| Tool | Does |
|---|---|
| `get_authoring_guide` | the full JSON reference, or one section of it |
| `list_vocabulary` | effect / transition / easing / action / property / object / **font** names, from the binary |
| `validate_scene` | parse and check without rendering |
| `describe_scene` | resolved positions, sizes and timings after expansion |
| `preview_frames` | frames at given timestamps, returned as one tiled contact sheet (or separately with `contact_sheet: false`) |
| `render_video` | render to MP4, return the path |

A scene may be passed inline as `scene`, or by `path` to a `.json` file. Inline
scenes are written to a fresh temporary directory, so relative asset paths
resolve there and nowhere else.

## Why `preview_frames` matters

`validate_scene` only catches what it was told to look for: zero sizes, missing
timeline targets, objects off the canvas. It passes a scene that is wrong in
every way that matters visually.

Each of these was a real bug in this project's own demo scenes, and the
validator was silent for all four:

- a highlight band four times too dim (alpha applied twice)
- a loss curve plotted upside down
- twelve "spokes" stacked into a single line
- `trim` that was binary instead of progressive

All four were found by looking at a rendered frame. An agent authoring scenes is
in exactly that position — writing JSON it cannot see. A preview is
byte-identical to the corresponding frame of the full render and takes about
0.05 s, so there is no reason to skip it.

Frames come back tiled into one strip by default: six separate pictures answer
"what does this moment look like", while one strip answers "does this timeline
work" — which is the question an author actually has.

**The one thing a preview cannot catch** is a missing font. Cairo substitutes
silently, so a scene naming a font that is not installed renders in some other
face and looks entirely plausible. `list_vocabulary(kind="fonts")` is the only
defence.

## Design

The server interprets nothing. Every tool shells out to the `video_redac`
binary and returns what it printed, because a second implementation of the
schema would drift from the one that actually renders. The engine gained three
small flags for this: `--json` (machine-readable `--check`/`--dump`), `--list`
(its own vocabulary), and `--frame T` (one still, expressed internally as a
one-frame `--range` so a preview cannot disagree with the render).
