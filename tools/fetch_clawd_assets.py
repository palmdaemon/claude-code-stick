#!/usr/bin/env python3
"""
Fetch clawd-tank slack-emoji GIFs (MIT licensed by marciogranzotto), make
their solid backgrounds transparent, center-pad onto the StickS3 buddy
canvas, and write to firmware/data/characters/clawd/.

Output sizing:
  Canvas: 135×120 (top half of 135×240 portrait display).
  Each sprite is scaled-if-needed to fit, centered, transparent margins.

Usage:
  python tools/fetch_clawd_assets.py [--canvas 135x120] [--dry-run]
"""

import argparse
import io
import os
import sys
import time
from pathlib import Path
from typing import Tuple

import requests
from PIL import Image, ImageSequence

REPO_RAW = "https://raw.githubusercontent.com/marciogranzotto/clawd-tank/master/assets/slack-emojis"

# state-name → source-filename (no path prefix). Names follow the clawd
# theme convention; firmware references states by name.
ASSETS = {
    "idle":              "clawd-idle-living.gif",
    "sleeping":          "clawd-sleeping.gif",
    "notification":      "clawd-notification.gif",
    "happy":             "clawd-happy.gif",
    "disconnected":      "clawd-disconnected.gif",
    "going_away":        "clawd-going-away.gif",
    "dizzy":             "clawd-dizzy.gif",  # error/confused
    "working_typing":    "clawd-working-typing.gif",     # Edit/Write
    "working_thinking":  "clawd-working-thinking.gif",   # general thinking
    "working_debugger":  "clawd-working-debugger.gif",   # Read/Grep
    "working_building":  "clawd-working-building.gif",   # Bash
    "working_conducting":"clawd-working-conducting.gif", # Agent
    "working_wizard":    "clawd-working-wizard.gif",     # WebFetch
    "working_beacon":    "clawd-working-beacon.gif",     # LSP/MCP
    "working_sweeping":  "clawd-working-sweeping.gif",   # context compact
    "working_juggling":  "clawd-working-juggling.gif",   # multi-task
    "working_overheated":"clawd-working-overheated.gif", # high load
    "working_confused":  "clawd-working-confused.gif",   # 60s+ wait
    "static_base":       "clawd-static-base.gif",        # fallback
}


def download(url: str) -> bytes:
    r = requests.get(url, timeout=30)
    r.raise_for_status()
    return r.content


def detect_bg_color(frame: Image.Image) -> Tuple[int, int, int]:
    """Background color = the most common color among the four corners
    plus the top edge sampled every 5 px. Works on these emoji GIFs because
    the character is centered and backgrounds are flat."""
    img = frame.convert("RGB")
    w, h = img.size
    samples = []
    for x, y in [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]:
        samples.append(img.getpixel((x, y)))
    for x in range(0, w, 5):
        samples.append(img.getpixel((x, 0)))
        samples.append(img.getpixel((x, h - 1)))
    # Most common color among corners/edge.
    from collections import Counter
    return Counter(samples).most_common(1)[0][0]


def make_transparent(frames, bg: Tuple[int, int, int], tol: int = 12):
    """Replace pixels within `tol` of bg color with transparent. Yields RGBA
    frames at the same canvas size as input."""
    out = []
    for f in frames:
        rgba = f.convert("RGBA")
        data = rgba.getdata()
        new = []
        br, bgr, bb = bg
        for r, g, b, a in data:
            if abs(r - br) <= tol and abs(g - bgr) <= tol and abs(b - bb) <= tol:
                new.append((r, g, b, 0))
            else:
                new.append((r, g, b, a))
        rgba.putdata(new)
        out.append(rgba)
    return out


def pad_to_canvas(frames, canvas_w: int, canvas_h: int):
    """Center each frame on a transparent canvas. If frame is larger, scale
    down (preserving aspect)."""
    out = []
    for f in frames:
        fw, fh = f.size
        # Scale down if needed.
        scale = min(canvas_w / fw, canvas_h / fh, 1.0)
        if scale < 1.0:
            new_size = (int(fw * scale), int(fh * scale))
            f = f.resize(new_size, Image.LANCZOS)
            fw, fh = f.size
        canvas = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
        ox = (canvas_w - fw) // 2
        oy = (canvas_h - fh) // 2
        canvas.paste(f, (ox, oy), f)
        out.append(canvas)
    return out


def save_gif(frames, durations, out_path: Path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # GIF only supports 1-bit alpha (index 0 transparent). Quantize each
    # frame; PIL will pick transparent index automatically.
    quantized = []
    for f in frames:
        # convert("P", palette=ADAPTIVE) preserves transparency through
        # the 'transparency' info on the first frame.
        q = f.convert("RGBA")
        # Force pixels with alpha<128 to a sentinel color, rest to opaque.
        bg_marker = (0, 1, 0)  # near-black, very rare in clawd palette
        rgb = q.convert("RGB")
        alpha = q.split()[3]
        rgb_data = list(rgb.getdata())
        for i, a in enumerate(alpha.getdata()):
            if a < 128:
                rgb_data[i] = bg_marker
        flat = Image.new("RGB", q.size)
        flat.putdata(rgb_data)
        p = flat.convert("P", palette=Image.ADAPTIVE, colors=255)
        # Find sentinel index and mark as transparent.
        pal = p.getpalette()
        sentinel_idx = None
        for i in range(0, len(pal), 3):
            if pal[i:i+3] == list(bg_marker):
                sentinel_idx = i // 3
                break
        if sentinel_idx is not None:
            p.info["transparency"] = sentinel_idx
        quantized.append(p)
    quantized[0].save(
        out_path,
        format="GIF",
        save_all=True,
        append_images=quantized[1:],
        duration=durations,
        loop=0,
        disposal=2,  # restore to background — keeps transparency clean
        optimize=False,
    )


def process_one(state: str, src_name: str, out_dir: Path,
                canvas: Tuple[int, int], dry_run: bool):
    url = f"{REPO_RAW}/{src_name}"
    out_path = out_dir / f"{state}.gif"
    print(f"[{state:20s}] {src_name}")
    if dry_run:
        return
    try:
        raw = download(url)
    except Exception as e:
        print(f"  ! download failed: {e}")
        return
    src = Image.open(io.BytesIO(raw))
    # PIL gotcha: iterating + .copy() reuses the decoder state; every frame
    # ends up identical to the last one decoded. Must seek+convert per index.
    frames = []
    durations = []
    for i in range(src.n_frames):
        src.seek(i)
        frames.append(src.convert("RGBA"))
        durations.append(src.info.get("duration", 80))
    if not frames:
        print("  ! no frames")
        return
    bg = detect_bg_color(frames[0])
    print(f"  bg={bg} frames={len(frames)} src_size={frames[0].size}")
    transp = make_transparent(frames, bg)
    padded = pad_to_canvas(transp, *canvas)
    save_gif(padded, durations, out_path)
    sz_kb = out_path.stat().st_size / 1024
    print(f"  → {out_path}  {sz_kb:.1f}KB  canvas={canvas[0]}x{canvas[1]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canvas", default="135x120",
                    help="WxH for output GIFs (default 135x120)")
    ap.add_argument("--out",
                    default=str(Path(__file__).resolve().parents[1]
                                / "firmware" / "data" / "characters" / "clawd"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    w, h = (int(x) for x in args.canvas.lower().split("x"))
    out_dir = Path(args.out)
    print(f"output: {out_dir}  canvas: {w}x{h}")

    for state, fname in ASSETS.items():
        process_one(state, fname, out_dir, (w, h), args.dry_run)
        time.sleep(0.05)  # be polite to raw.githubusercontent.com

    print("\ndone.")


if __name__ == "__main__":
    main()
