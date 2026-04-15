#!/usr/bin/env python3
"""
E-Ink Calendar Display Simulator
=================================
Renders the same layout as the ESP32 firmware to a PNG file
so you can iterate on styling without uploading to the device.

Usage:
    python simulator.py                  # Use sample events, current time
    python simulator.py --time 10:30     # Override the "now" time
    python simulator.py --events events.json  # Load events from JSON file

The events JSON file uses the same format the Apps Script returns:
[
  {"title": "Standup", "start": "2026-04-15T15:00:00.000Z", "end": "2026-04-15T15:30:00.000Z", "allDay": false},
  {"title": "Company Holiday", "allDay": true}
]

Output: simulator_output.png
"""

import argparse
import json
import os
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ── Layout constants (must match main.cpp) ─────────────────
SCREEN_W   = 272
SCREEN_H   = 792

HEADER_H   = 42
FOOTER_H   = 18
TIMELINE_Y = HEADER_H
TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H  # 732
FOOTER_Y   = SCREEN_H - FOOTER_H

HOUR_START    = 9
HOUR_END      = 17
TOTAL_MINUTES = (HOUR_END - HOUR_START) * 60  # 480

LABEL_W = 48
EVENT_X = LABEL_W + 2
EVENT_W = SCREEN_W - EVENT_X - 4  # ~228

BLACK = 0
WHITE = 255

# ── Font loading ────────────────────────────────────────────
# Try to find FreeSans via system fonts, fall back to default

def _find_font(name, size):
    """Try common system font paths."""
    search = [
        f"/usr/share/fonts/truetype/freefont/{name}.ttf",
        f"/usr/share/fonts/TTF/{name}.ttf",
        f"/usr/share/fonts/gnu-free/{name}.ttf",
        f"/usr/share/fonts/freefont-ttf/{name}.ttf",
        f"/usr/share/fonts/{name}.ttf",
    ]
    for p in search:
        if os.path.isfile(p):
            return ImageFont.truetype(p, size)
    # Last resort: try by name (works if fontconfig is available)
    try:
        return ImageFont.truetype(name, size)
    except OSError:
        return None

def load_fonts():
    header_font = _find_font("FreeSansBold", 16) or ImageFont.load_default()
    label_font  = _find_font("FreeSans", 9)       or ImageFont.load_default()
    event_font  = _find_font("FreeSansBold", 12)  or ImageFont.load_default()
    time_font   = _find_font("FreeSans", 12)       or ImageFont.load_default()
    footer_font = _find_font("FreeSans", 10)       or ImageFont.load_default()
    return header_font, label_font, event_font, time_font, footer_font


# ── Helpers ─────────────────────────────────────────────────

def time_to_y(hour, minute):
    mins = (hour - HOUR_START) * 60 + minute
    mins = max(0, min(mins, TOTAL_MINUTES))
    return TIMELINE_Y + mins * TIMELINE_H // TOTAL_MINUTES


def truncate_to_fit(draw, text, font, max_w):
    bbox = draw.textbbox((0, 0), text, font=font)
    if bbox[2] - bbox[0] <= max_w:
        return text
    for i in range(len(text), 0, -1):
        candidate = text[:i] + "…"
        bbox = draw.textbbox((0, 0), candidate, font=font)
        if bbox[2] - bbox[0] <= max_w:
            return candidate
    return "…"


# ── Drawing functions ───────────────────────────────────────

def draw_header(draw, now, header_font):
    date_str = now.strftime("%A, %B %d")
    bbox = draw.textbbox((0, 0), date_str, font=header_font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (SCREEN_W - tw) // 2
    y = (HEADER_H - th) // 2
    draw.text((x, y), date_str, fill=BLACK, font=header_font)
    draw.line([(0, HEADER_H - 1), (SCREEN_W, HEADER_H - 1)], fill=BLACK)


def draw_hour_grid(draw, label_font):
    for h in range(HOUR_START, HOUR_END + 1):
        y = time_to_y(h, 0)

        # Dotted gridline
        for x in range(LABEL_W, SCREEN_W, 4):
            draw.point((x, y), fill=BLACK)

        if h == HOUR_END:
            break
        if h == HOUR_START:
            continue  # Skip 9am label

        # Hour label
        if h == 12:
            label = "12pm"
        elif h < 12:
            label = f"{h}am"
        else:
            label = f"{h - 12}pm"

        bbox = draw.textbbox((0, 0), label, font=label_font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        lx = LABEL_W - tw - 1
        ly = y - th // 2
        draw.text((lx, ly), label, fill=BLACK, font=label_font)

    # Vertical separator
    draw.line([(LABEL_W, TIMELINE_Y), (LABEL_W, TIMELINE_Y + TIMELINE_H)], fill=BLACK)


def compute_overlap_columns(events):
    """Assign column index and column count for overlapping timed events."""
    timed = [(i, ev) for i, ev in enumerate(events)
             if not ev["allDay"]
             and ev["endHour"] * 60 + ev["endMin"] > HOUR_START * 60
             and ev["startHour"] * 60 + ev["startMin"] < HOUR_END * 60]

    col = {i: 0 for i, _ in timed}
    col_count = {i: 1 for i, _ in timed}

    for idx_a, (ai, a) in enumerate(timed):
        a_start = a["startHour"] * 60 + a["startMin"]
        a_end   = a["endHour"]   * 60 + a["endMin"]
        used = set()
        for idx_b, (bi, b) in enumerate(timed[:idx_a]):
            b_start = b["startHour"] * 60 + b["startMin"]
            b_end   = b["endHour"]   * 60 + b["endMin"]
            if b_start < a_end and b_end > a_start:
                used.add(col[bi])
        c = 0
        while c in used:
            c += 1
        col[ai] = c

    for ai, a in timed:
        a_start = a["startHour"] * 60 + a["startMin"]
        a_end   = a["endHour"]   * 60 + a["endMin"]
        max_col = col[ai]
        for bi, b in timed:
            b_start = b["startHour"] * 60 + b["startMin"]
            b_end   = b["endHour"]   * 60 + b["endMin"]
            if b_start < a_end and b_end > a_start:
                max_col = max(max_col, col[bi])
        col_count[ai] = max_col + 1

    return col, col_count


def draw_events(draw, events, event_font, time_font, label_font):
    col, col_count = compute_overlap_columns(events)

    # Timed events
    for i, ev in enumerate(events):
        if ev["allDay"]:
            continue
        sh, sm = ev["startHour"], ev["startMin"]
        eh, em = ev["endHour"], ev["endMin"]

        if eh < HOUR_START or (eh == HOUR_START and em == 0):
            continue
        if sh >= HOUR_END:
            continue

        # Clamp
        if sh < HOUR_START:
            sh, sm = HOUR_START, 0
        if eh > HOUR_END or (eh == HOUR_END and em > 0):
            eh, em = HOUR_END, 0

        y1 = time_to_y(sh, sm)
        y2 = time_to_y(eh, em)
        block_h = max(y2 - y1, 2)

        # Column-based positioning
        cols = col_count.get(i, 1)
        c    = col.get(i, 0)
        col_w = EVENT_W // cols
        ex = EVENT_X + c * col_w
        ew = (EVENT_W - c * col_w) if c == cols - 1 else col_w

        # Filled black block
        draw.rectangle([(ex, y1), (ex + ew - 1, y1 + block_h - 1)], fill=BLACK)

        if block_h >= 16:
            title = truncate_to_fit(draw, ev["title"], event_font, ew - 8)
            bbox = draw.textbbox((0, 0), title, font=event_font)
            th = bbox[3] - bbox[1]
            text_y = y1 + 3
            draw.text((ex + 4, text_y), title, fill=WHITE, font=event_font)

            if block_h >= 34:
                s_h = ev["startHour"] - 12 if ev["startHour"] > 12 else ev["startHour"]
                e_h = ev["endHour"] - 12 if ev["endHour"] > 12 else ev["endHour"]
                time_str = f"{s_h}:{ev['startMin']:02d} - {e_h}:{ev['endMin']:02d}"
                draw.text((ex + 4, text_y + th + 3), time_str, fill=WHITE, font=time_font)

    # All-day banners
    banner_y = TIMELINE_Y
    for ev in events:
        if not ev["allDay"]:
            continue
        bbox = draw.textbbox((0, 0), ev["title"], font=label_font)
        th = bbox[3] - bbox[1]
        draw.rectangle(
            [(EVENT_X, banner_y + 2), (EVENT_X + EVENT_W - 1, banner_y + 2 + th + 5)],
            outline=BLACK,
        )
        draw.text((EVENT_X + 4, banner_y + 4), ev["title"], fill=BLACK, font=label_font)
        banner_y += th + 10


def draw_now_indicator(draw, now):
    hour, minute = now.hour, now.minute
    if hour < HOUR_START or hour >= HOUR_END:
        return
    y = time_to_y(hour, minute)
    draw.line([(LABEL_W + 1, y), (SCREEN_W - 1, y)], fill=BLACK, width=1)
    draw.polygon([(LABEL_W + 1, y), (LABEL_W + 7, y - 4), (LABEL_W + 7, y + 4)], fill=BLACK)


def draw_footer(draw, now, event_count, footer_font):
    draw.line([(0, FOOTER_Y), (SCREEN_W, FOOTER_Y)], fill=BLACK)
    updated = now.strftime("Updated %H:%M")
    draw.text((4, FOOTER_Y + 4), updated, fill=BLACK, font=footer_font)
    ev_text = f"{event_count} events"
    bbox = draw.textbbox((0, 0), ev_text, font=footer_font)
    tw = bbox[2] - bbox[0]
    draw.text((SCREEN_W - tw - 4, FOOTER_Y + 4), ev_text, fill=BLACK, font=footer_font)


# ── Event parsing ───────────────────────────────────────────

SAMPLE_EVENTS = [
    {"title": "Standup",          "start": "2026-04-15T15:00:00.000Z", "end": "2026-04-15T15:30:00.000Z", "allDay": False},
    {"title": "Sprint Planning",  "start": "2026-04-15T16:00:00.000Z", "end": "2026-04-15T17:00:00.000Z", "allDay": False},
    {"title": "Lunch w/ Sarah",   "start": "2026-04-15T18:30:00.000Z", "end": "2026-04-15T19:30:00.000Z", "allDay": False},
    {"title": "1:1 with Manager", "start": "2026-04-15T20:00:00.000Z", "end": "2026-04-15T20:30:00.000Z", "allDay": False},
    {"title": "Design Sync",     "start": "2026-04-15T20:00:00.000Z", "end": "2026-04-15T21:00:00.000Z", "allDay": False},
    {"title": "Code Review",      "start": "2026-04-15T21:00:00.000Z", "end": "2026-04-15T22:00:00.000Z", "allDay": False},
    {"title": "Company Holiday",  "allDay": True},
]


def parse_events(raw_events, tz_offset_hours=-7):
    """Convert Apps-Script-style JSON events into the internal format."""
    tz = timezone(timedelta(hours=tz_offset_hours))
    parsed = []
    for ev in raw_events:
        entry = {"title": ev.get("title", ""), "allDay": ev.get("allDay", False)}
        if entry["allDay"]:
            entry["startHour"] = HOUR_START
            entry["startMin"]  = 0
            entry["endHour"]   = HOUR_END
            entry["endMin"]    = 0
        else:
            start = datetime.fromisoformat(ev["start"].replace("Z", "+00:00")).astimezone(tz)
            end   = datetime.fromisoformat(ev["end"].replace("Z", "+00:00")).astimezone(tz)
            entry["startHour"] = start.hour
            entry["startMin"]  = start.minute
            entry["endHour"]   = end.hour
            entry["endMin"]    = end.minute
        parsed.append(entry)
    # Sort by start time
    parsed.sort(key=lambda e: e["startHour"] * 60 + e["startMin"])
    return parsed


# ── Main ────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="E-Ink Calendar Display Simulator")
    parser.add_argument("--time", help="Override current time (HH:MM), e.g. 10:30")
    parser.add_argument("--events", help="Path to JSON file with events (Apps Script format)")
    parser.add_argument("--tz", type=float, default=-7, help="UTC offset in hours (default: -7 for MST)")
    parser.add_argument("-o", "--output", default="simulator_output.png", help="Output PNG path")
    parser.add_argument("--scale", type=int, default=1, help="Scale factor for output (e.g. 2 for 2x)")
    args = parser.parse_args()

    # Load events
    if args.events:
        with open(args.events) as f:
            raw = json.load(f)
    else:
        raw = SAMPLE_EVENTS
        print("Using sample events (pass --events file.json to use real data)")

    events = parse_events(raw, tz_offset_hours=args.tz)

    # Determine "now"
    tz = timezone(timedelta(hours=args.tz))
    now = datetime.now(tz)
    if args.time:
        h, m = map(int, args.time.split(":"))
        now = now.replace(hour=h, minute=m)

    print(f"Simulating: {now.strftime('%A, %B %d  %H:%M')}  ({len(events)} events)")

    # Create image
    img = Image.new("L", (SCREEN_W, SCREEN_H), WHITE)
    draw = ImageDraw.Draw(img)

    header_font, label_font, event_font, time_font, footer_font = load_fonts()

    draw_header(draw, now, header_font)
    draw_hour_grid(draw, label_font)
    draw_events(draw, events, event_font, time_font, label_font)
    draw_now_indicator(draw, now)
    draw_footer(draw, now, len(events), footer_font)

    # Scale if requested
    if args.scale > 1:
        img = img.resize(
            (SCREEN_W * args.scale, SCREEN_H * args.scale),
            Image.Resampling.NEAREST,
        )

    img.save(args.output)
    print(f"Saved → {args.output}  ({img.size[0]}×{img.size[1]})")


if __name__ == "__main__":
    main()
