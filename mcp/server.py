#!/usr/bin/env python3
"""
video_redac MCP server — author, inspect and render JSON scenes.

The engine is a CLI, so this is a thin wrapper: every tool shells out to the
`video_redac` binary and hands back what it printed. Nothing about a scene is
interpreted here, on purpose. Two implementations of the schema would drift
apart, and the binary is the one that actually renders — so it stays the single
source of truth, and this file only translates between MCP and argv.

The design point worth knowing about is `preview_frames`. An agent writing scene
JSON cannot see what it produced, and a validator only catches what it was told
to look for — sizes, missing targets, off-canvas objects. It says nothing about
a curve plotted upside down, a highlight band four times too dim, or twelve
"spokes" stacked into one line. Every one of those was a real bug in this
project's own demo scenes, and every one was caught by looking at a frame. So
previews return the image itself, not a path to it.

Environment:
  VIDEO_REDAC_BIN   path to the binary   (default: ../video_redac next to this file)
  VIDEO_REDAC_OUT   where renders go     (default: ~/.video_redac/out)
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Annotated, Any

from mcp.server import MCPServer
from mcp.server.mcpserver.utilities.types import Image
from pydantic import Field

# --------------------------------------------------------------------------- #
# Paths and limits                                                             #
# --------------------------------------------------------------------------- #

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

BIN = Path(os.environ.get("VIDEO_REDAC_BIN", ROOT / "video_redac")).resolve()
OUT = Path(os.environ.get("VIDEO_REDAC_OUT", Path.home() / ".video_redac" / "out"))
GUIDE = ROOT / "docs" / "03-json-reference.md"

# A render is fast — a 22-second film takes ~3.4 s on the GPU, ~17 s on the CPU
# — but a pathological project should not be able to hang the session.
RENDER_TIMEOUT = 900
QUICK_TIMEOUT = 120

# Previews come back inline as base64, so they have to stay small enough to be
# worth reading. Anything wider is downscaled for the reply only; the file on
# disk keeps full resolution.
PREVIEW_MAX_WIDTH = 900
MAX_PREVIEW_FRAMES = 6


class EngineError(RuntimeError):
    """Something the caller can act on: a bad scene, a missing binary."""


# --------------------------------------------------------------------------- #
# Running the engine                                                           #
# --------------------------------------------------------------------------- #


def _run(args: list[str], timeout: int = QUICK_TIMEOUT) -> subprocess.CompletedProcess[str]:
    if not BIN.exists():
        raise EngineError(
            f"video_redac binary not found at {BIN}. Build it with `make` "
            f"(or `make CPU=1` on a machine without an NVIDIA GPU)."
        )
    try:
        return subprocess.run(
            [str(BIN), *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=str(ROOT),
        )
    except subprocess.TimeoutExpired as exc:
        raise EngineError(f"the engine did not finish within {timeout}s") from exc


def _scene_to_path(scene: Any, path: str | None,
                   base_dir: str | None = None) -> tuple[Path, Path | None]:
    """
    Resolve the three ways a scene can arrive: as a file, inline, or inline with
    a base directory for its relative asset paths.

    Returns (json_path, path_to_remove).

    The engine resolves every relative asset path against **the JSON file's own
    directory**. An inline scene has no directory, so by default it is written
    into a fresh temporary one: `"photos/1.jpg"` then resolves inside that
    temporary directory and finds nothing, rather than silently picking up a
    file from wherever the server happens to be running.

    That is the safe default and it is also a dead end for any scene that wants
    the project's own assets. `base_dir` is the explicit way out: the scene is
    written there instead, so relative paths mean what they would mean in a real
    project file. Explicit, because "which directory do relative paths mean" is
    exactly the question that should not be answered by accident.
    """
    if path:
        p = Path(path).expanduser()
        if not p.is_absolute():
            p = (ROOT / p).resolve()
        if not p.exists():
            raise EngineError(f"scene file not found: {p}")
        return p, None

    if scene is None:
        raise EngineError("provide either `scene` (the JSON) or `path` (a file)")

    if isinstance(scene, str):
        try:
            scene = json.loads(scene)
        except json.JSONDecodeError as exc:
            raise EngineError(f"`scene` is not valid JSON: {exc}") from exc

    if base_dir:
        d = Path(base_dir).expanduser()
        if not d.is_absolute():
            d = (ROOT / d).resolve()
        if not d.is_dir():
            raise EngineError(f"base_dir is not a directory: {d}")
        # A single file rather than a directory, deleted afterwards — writing
        # into someone's project is acceptable only if nothing is left behind.
        fd, name = tempfile.mkstemp(prefix=".vr_scene_", suffix=".json", dir=d)
        os.close(fd)
        f = Path(name)
        f.write_text(json.dumps(scene), encoding="utf-8")
        return f, f

    tmp = Path(tempfile.mkdtemp(prefix="vr_scene_"))
    f = tmp / "scene.json"
    f.write_text(json.dumps(scene), encoding="utf-8")
    return f, tmp


# Assets an inline scene could not reach, which is nearly always the base_dir
# question rather than a missing file.
_ASSET_MISS = re.compile(r"cannot load|could not load image|rasterizing .* failed")


def _asset_hint(stderr: str, tmp: Path | None) -> str:
    """A pointer to `base_dir` when an isolated inline scene lost its assets."""
    if tmp is None or not _ASSET_MISS.search(stderr or ""):
        return ""
    return ("\n\nHint: this scene was passed inline, so its relative asset paths "
            "resolved inside a temporary directory and found nothing. Pass "
            "`base_dir` (the directory the paths are relative to, e.g. the "
            "project root or 'anim'), or use `path` to point at a real file.")


def _cleanup(tmp: Path | None) -> None:
    if tmp is None:
        return
    if tmp.is_dir():
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        tmp.unlink(missing_ok=True)


def _safe_name(name: str) -> str:
    """A caller-supplied output name reduced to something safe to join."""
    base = os.path.basename(name or "")
    base = re.sub(r"[^A-Za-z0-9._-]", "_", base).strip("._-")
    return base or f"render_{int(time.time())}"


def _contact_sheet(pngs: list[Path], max_width: int) -> bytes | None:
    """
    Tile several frames into one image.

    Six separate pictures answer "what does this moment look like"; one strip
    answers "does this timeline work" — which is the question an author actually
    has. Reading them side by side is also how a stalled animation or a
    mistimed entrance becomes obvious.
    """
    if not pngs or not shutil.which("ffmpeg"):
        return None

    cols = min(len(pngs), 3)
    rows = (len(pngs) + cols - 1) // cols
    cell = max(160, max_width // cols)

    cmd: list[str] = ["ffmpeg", "-v", "error"]
    for p in pngs:
        cmd += ["-i", str(p)]

    # Every input is scaled to the same cell size: xstack requires matching
    # dimensions, and a scene's frames are all one size anyway.
    #
    # `grid=CxR` rather than a `layout` string. xstack's layout takes *pixel
    # offsets*, not grid indices — "1_0" means one pixel right, not one cell
    # right — so a hand-built index layout stacks every frame on the same spot.
    chains = "".join(f"[{i}:v]scale={cell}:-2,setsar=1[v{i}];" for i in range(len(pngs)))
    inputs = "".join(f"[v{i}]" for i in range(len(pngs)))

    if len(pngs) == 1:
        filt = f"{chains}[v0]null[out]"
    else:
        filt = (f"{chains}{inputs}"
                f"xstack=inputs={len(pngs)}:grid={cols}x{rows}:fill=black[out]")

    cmd += ["-filter_complex", filt, "-map", "[out]",
            "-frames:v", "1", "-f", "image2", "-c:v", "png", "-"]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=60)
        if proc.returncode == 0 and proc.stdout:
            return proc.stdout
    except subprocess.TimeoutExpired:
        pass
    return None


def _downscale(png: Path, max_width: int) -> bytes:
    """Shrink a preview for the reply only; the file on disk is untouched."""
    data = png.read_bytes()
    if not shutil.which("ffmpeg"):
        return data
    try:
        proc = subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(png),
             "-vf", f"scale='min({max_width},iw)':-2",
             "-f", "image2", "-c:v", "png", "-"],
            capture_output=True, timeout=30,
        )
        if proc.returncode == 0 and proc.stdout:
            return proc.stdout
    except subprocess.TimeoutExpired:
        pass
    return data


# --------------------------------------------------------------------------- #
# Server                                                                       #
# --------------------------------------------------------------------------- #

server = MCPServer(
    "video-redac",
    instructions=(
        "Renders JSON scenes to MP4 with a CUDA/CPU engine.\n\n"
        "Suggested flow: get_authoring_guide → write the scene → validate_scene "
        "→ preview_frames → render_video.\n\n"
        "Do not skip preview_frames. validate_scene only checks what it was told "
        "to look for; it cannot tell you that a scene looks wrong."
    ),
)

SceneArg = Annotated[
    Any, Field(default=None, description="The scene JSON itself (object or string).")
]
PathArg = Annotated[
    str | None, Field(default=None, description="Path to a .json scene file, instead of `scene`.")
]


@server.tool(
    description=(
        "The complete JSON scene reference: every key, type and default. Read "
        "this before authoring a scene. Pass `section` to get one part instead "
        "of the whole reference, which is long.\n\n"
        "Sections worth knowing exist, because they are easy not to look for: "
        "'mesh', 'camera', 'light', 'emitter', 'timeline', 'repeat'; the "
        "grading controls 'lut', 'lift_gamma_gain', 'qualifier', 'window', "
        "'bloom'; 'motion_blur'; the per-object 'key' (chroma key); the mesh "
        "maps 'normal_map' and 'emissive'; the audio 'duck' and channel strip; "
        "and 'alpha' for keeping a matte."
    )
)
def get_authoring_guide(
    section: Annotated[str | None, Field(default=None, description="Section to return alone.")] = None,
) -> str:
    if not GUIDE.exists():
        raise EngineError(f"the reference is missing at {GUIDE}")
    text = GUIDE.read_text(encoding="utf-8")
    if not section:
        return text

    want = section.strip().lower()

    def split_on(pattern):
        return re.split(pattern, text, flags=re.MULTILINE)

    def match(blocks):
        return [b for b in blocks if want in b.split("\n", 1)[0].lower()]

    # Top-level sections first, then the subsections inside them. Splitting only
    # on "## " missed everything documented one level down — which is where the
    # object types live, so asking for "mesh" or "text", the likeliest request
    # of all, answered that no such section existed.
    top = split_on(r"^(?=## )")
    hits = match(top) or match(split_on(r"^(?=### )"))

    if not hits:
        titles = [b.split("\n", 1)[0].lstrip("# ").strip()
                  for b in top if b.startswith("## ")]
        subs = re.findall(r"^### +(.+)$", text, flags=re.MULTILINE)
        return (f"No section matching {section!r}.\n\nSections:\n- "
                + "\n- ".join(titles)
                + "\n\nSubsections:\n- " + "\n- ".join(subs))
    return "\n".join(hits)


@server.tool(
    description=(
        "The exact names the engine accepts, straight from the binary: effects, "
        "transitions, easings, timeline actions, animatable properties, object "
        "types, mesh shapes, or installed fonts. Use this rather than guessing "
        "a name — the lists come from the code that implements them, so they "
        "cannot be out of date the way this description could."
    )
)
def list_vocabulary(
    kind: Annotated[str, Field(
        description="effects | transitions | easings | actions | properties | "
                    "widgets | shapes | lights | fonts")],
) -> str:
    proc = _run(["--list", kind])
    if proc.returncode != 0:
        raise EngineError(proc.stderr.strip() or f"unknown vocabulary {kind!r}")
    return proc.stdout.strip()


@server.tool(
    description=(
        "Parse and check a scene without rendering. Reports zero-size and "
        "off-canvas objects, unresolved timeline targets, zero durations and "
        "audio past the end, plus the resolved resolution, duration and frame "
        "count. Cheap — run it after every edit.\n\n"
        "It cannot tell you whether the scene LOOKS right. Use preview_frames."
    )
)
def validate_scene(scene: SceneArg = None, path: PathArg = None,
                   base_dir: Annotated[str | None, Field(default=None, description="Directory the scene's relative asset paths resolve against (inline scenes only).")] = None) -> str:
    json_path, tmp = _scene_to_path(scene, path, base_dir)
    try:
        proc = _run([str(json_path), "--check", "--json"])
        if not proc.stdout.strip():
            # The parser refused the file outright; its reason is on stderr.
            return json.dumps(
                {"ok": False,
                 "problems": [{"severity": "error",
                               "message": (proc.stderr.strip() or "could not load project")
                                          + _asset_hint(proc.stderr, tmp)}]},
                indent=2,
            )
        return proc.stdout.strip()
    finally:
        _cleanup(tmp)


@server.tool(
    description=(
        "What the engine actually decided, after variables, styles, layout "
        "expressions and repeat/emitter expansion: every object's resolved "
        "position, size and z-order, and every scene's start and duration. Use "
        "this when a scene parses but an object is not where you meant it."
    )
)
def describe_scene(scene: SceneArg = None, path: PathArg = None,
                   base_dir: Annotated[str | None, Field(default=None, description="Directory the scene's relative asset paths resolve against (inline scenes only).")] = None) -> str:
    json_path, tmp = _scene_to_path(scene, path, base_dir)
    try:
        proc = _run([str(json_path), "--dump", "--json", "--dry-run"])
        if not proc.stdout.strip():
            raise EngineError(proc.stderr.strip() or "could not describe the project")
        return proc.stdout.strip()
    finally:
        _cleanup(tmp)


@server.tool(
    description=(
        "Render single frames at given timestamps and return the images.\n\n"
        "Use this before render_video, and whenever a scene is not doing what "
        "you expect. A validator only catches what it was told to look for: it "
        "will happily pass a curve plotted upside down, a highlight four times "
        "too dim, or a dozen objects stacked on one spot. Looking at a frame is "
        "the only way to catch those.\n\n"
        "A preview is byte-identical to the corresponding frame of the full "
        "render, and takes about a twentieth of a second."
    ),
    # The return is a mix of text and images, which has no useful JSON schema —
    # and asking pydantic to derive one from `list[Image | str]` fails outright.
    structured_output=False,
)
def preview_frames(
    times: Annotated[list[float], Field(description=f"Timestamps in seconds (max {MAX_PREVIEW_FRAMES}).")],
    scene: SceneArg = None,
    path: PathArg = None,
    max_width: Annotated[int, Field(default=PREVIEW_MAX_WIDTH,
                                    description="Downscale the returned image; the saved file keeps full size.")] = PREVIEW_MAX_WIDTH,
    contact_sheet: Annotated[bool, Field(default=True,
                                         description="Return one tiled strip instead of separate images.")] = True,
    base_dir: Annotated[str | None, Field(default=None, description="Directory the scene's relative asset paths resolve against (inline scenes only).")] = None,
):
    if not times:
        raise EngineError("give at least one timestamp in `times`")
    if len(times) > MAX_PREVIEW_FRAMES:
        raise EngineError(f"at most {MAX_PREVIEW_FRAMES} frames per call")

    json_path, tmp = _scene_to_path(scene, path, base_dir)
    OUT.mkdir(parents=True, exist_ok=True)
    stamp = f"preview_{int(time.time() * 1000)}"

    out: list[Image | str] = []
    written: list[Path] = []
    try:
        for t in times:
            png = OUT / f"{stamp}_{t:g}s.png"
            proc = _run([str(json_path), "--frame", str(t), "-o", str(png)])
            if proc.returncode != 0 or not png.exists():
                raise EngineError(
                    f"rendering the frame at {t}s failed:\n"
                    f"{proc.stderr.strip()[-1500:]}{_asset_hint(proc.stderr, tmp)}"
                )
            written.append(png)
    finally:
        _cleanup(tmp)

    if contact_sheet and len(written) > 1:
        sheet = _contact_sheet(written, max_width)
        if sheet is not None:
            labels = ",  ".join(f"{t:g}s" for t in times)
            out.append(f"frames at {labels} (left to right, top to bottom)")
            out.append(Image(data=sheet, format="png"))
            out.append("saved: " + ", ".join(str(p) for p in written))
            return out

    for t, png in zip(times, written):
        out.append(f"t = {t:g}s  →  {png}")
        out.append(Image(data=_downscale(png, max_width), format="png"))
    return out


@server.tool(
    description=(
        "Render the scene to an MP4 and return its path. Validate first — a "
        "scene with problems will still render, just not as intended. Give "
        "`start`/`end` to render only part of the timeline.\n\n"
        "Rendering cost is not uniform: `project.motion_blur` renders the whole "
        "scene once per sample, so 16 samples is roughly 14x the time. Check it "
        "is what you want before rendering a long timeline with it on."
    )
)
def render_video(
    scene: SceneArg = None,
    path: PathArg = None,
    name: Annotated[str | None, Field(default=None, description="Output file name, not a path.")] = None,
    start: Annotated[float | None, Field(default=None, description="Render from this second.")] = None,
    end: Annotated[float | None, Field(default=None, description="Render up to this second.")] = None,
    base_dir: Annotated[str | None, Field(default=None, description="Directory the scene's relative asset paths resolve against (inline scenes only).")] = None,
) -> str:
    json_path, tmp = _scene_to_path(scene, path, base_dir)
    OUT.mkdir(parents=True, exist_ok=True)

    out_name = _safe_name(name or f"render_{int(time.time())}")
    if not out_name.lower().endswith((".mp4", ".mkv", ".mov", ".webm")):
        out_name += ".mp4"
    out_path = OUT / out_name

    args = [str(json_path), "-o", str(out_path)]
    if start is not None and end is not None and end > start:
        args += ["--range", f"{start}:{end}"]

    began = time.time()
    try:
        proc = _run(args, timeout=RENDER_TIMEOUT)
    finally:
        _cleanup(tmp)

    if proc.returncode != 0 or not out_path.exists():
        raise EngineError(f"render failed:\n{proc.stderr.strip()[-2500:]}"
                          f"{_asset_hint(proc.stderr, tmp)}")

    # The engine's last line carries the frame count and rate — the most useful
    # single sentence about what just happened.
    summary = next((ln for ln in reversed(proc.stderr.strip().splitlines())
                    if ln.startswith("done:")), "")

    return json.dumps(
        {
            "ok": True,
            "output": str(out_path),
            "bytes": out_path.stat().st_size,
            "wall_seconds": round(time.time() - began, 2),
            "engine": summary,
        },
        indent=2,
    )


def main() -> None:
    asyncio.run(server.run_stdio_async())


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, BrokenPipeError):
        sys.exit(0)
