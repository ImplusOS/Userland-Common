#!/usr/bin/env python3
"""
Minimal standalone SVG-path -> white RGBA PNG rasteriser.

Used by fetch_icons.sh to turn the Material Design icon SVGs
(@material-design-icons) into pre-rendered, anti-aliased PNG glyphs the
window manager / ImUI toolkit / login screen blit and tint at runtime.

Handles the subset the Material icons use: <path d="..."> with
M/L/H/V/C/S/Q/T/A/Z (abs + rel), fill-rule nonzero|evenodd, and a per-path
opacity. Output is RGB = white, A = coverage (4x4 supersampled), so the
consumer picks the colour.

No third-party deps -- PNG is emitted with the stdlib zlib.
"""
import sys, re, math, zlib, struct

# ---------------------------------------------------------------- path parse

_TOK = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|-?\d*\.?\d+(?:[eE][-+]?\d+)?")

def parse_path(d):
    toks = _TOK.findall(d)
    i = 0
    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    subpaths = []
    cur = []
    x = y = sx = sy = 0.0
    px = py = 0.0          # last control point for S/T
    cmd = None
    prev_cmd = None
    while i < len(toks):
        t = toks[i]
        if re.match(r"[A-Za-z]", t):
            cmd = t; i += 1
        else:
            # implicit repeat of previous command (M->L, m->l)
            cmd = prev_cmd
            if cmd == 'M': cmd = 'L'
            elif cmd == 'm': cmd = 'l'
        rel = cmd.islower()
        C = cmd.upper()
        if C == 'M':
            x = (x + num()) if rel else num()
            y = (y + num()) if rel else num()
            if cur: subpaths.append(cur)
            cur = [(x, y)]
            sx, sy = x, y
        elif C == 'L':
            x = (x + num()) if rel else num()
            y = (y + num()) if rel else num()
            cur.append((x, y))
        elif C == 'H':
            x = (x + num()) if rel else num()
            cur.append((x, y))
        elif C == 'V':
            y = (y + num()) if rel else num()
            cur.append((x, y))
        elif C in ('C', 'S'):
            if C == 'C':
                x1 = (x + num()) if rel else num(); y1 = (y + num()) if rel else num()
            else:
                if prev_cmd and prev_cmd.upper() in ('C', 'S'):
                    x1 = 2 * x - px; y1 = 2 * y - py
                else:
                    x1, y1 = x, y
            x2 = (x + num()) if rel else num(); y2 = (y + num()) if rel else num()
            ex = (x + num()) if rel else num(); ey = (y + num()) if rel else num()
            _bezier3(cur, x, y, x1, y1, x2, y2, ex, ey)
            px, py = x2, y2
            x, y = ex, ey
        elif C in ('Q', 'T'):
            if C == 'Q':
                x1 = (x + num()) if rel else num(); y1 = (y + num()) if rel else num()
            else:
                if prev_cmd and prev_cmd.upper() in ('Q', 'T'):
                    x1 = 2 * x - px; y1 = 2 * y - py
                else:
                    x1, y1 = x, y
            ex = (x + num()) if rel else num(); ey = (y + num()) if rel else num()
            _bezier2(cur, x, y, x1, y1, ex, ey)
            px, py = x1, y1
            x, y = ex, ey
        elif C == 'A':
            rx = num(); ry = num(); rot = num()
            laf = num(); sf = num()
            ex = (x + num()) if rel else num(); ey = (y + num()) if rel else num()
            _arc(cur, x, y, rx, ry, rot, laf, sf, ex, ey)
            x, y = ex, ey
        elif C == 'Z':
            cur.append((sx, sy))
            subpaths.append(cur)
            cur = [(sx, sy)]
            x, y = sx, sy
        prev_cmd = cmd
    if cur and len(cur) > 1:
        subpaths.append(cur)
    return subpaths

def _bezier3(out, x0, y0, x1, y1, x2, y2, x3, y3, n=18):
    for k in range(1, n + 1):
        t = k / n; u = 1 - t
        bx = u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3
        by = u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3
        out.append((bx, by))

def _bezier2(out, x0, y0, x1, y1, x2, y2, n=14):
    for k in range(1, n + 1):
        t = k / n; u = 1 - t
        bx = u*u*x0 + 2*u*t*x1 + t*t*x2
        by = u*u*y0 + 2*u*t*y1 + t*t*y2
        out.append((bx, by))

def _arc(out, x0, y0, rx, ry, phi_deg, laf, sf, x, y, n=24):
    if rx == 0 or ry == 0:
        out.append((x, y)); return
    phi = math.radians(phi_deg)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx2 = (x0 - x) / 2.0; dy2 = (y0 - y) / 2.0
    x1p =  cosp * dx2 + sinp * dy2
    y1p = -sinp * dx2 + cosp * dy2
    rx, ry = abs(rx), abs(ry)
    lam = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry)
    if lam > 1:
        s = math.sqrt(lam); rx *= s; ry *= s
    num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    den = rx*rx*y1p*y1p + ry*ry*x1p*x1p
    co = math.sqrt(max(0.0, num/den)) if den else 0.0
    if laf == sf: co = -co
    cxp = co * rx * y1p / ry
    cyp = -co * ry * x1p / rx
    cx = cosp * cxp - sinp * cyp + (x0 + x) / 2.0
    cy = sinp * cxp + cosp * cyp + (y0 + y) / 2.0
    def ang(ux, uy, vx, vy):
        d = (ux*vx + uy*vy) / (math.hypot(ux, uy) * math.hypot(vx, vy))
        d = max(-1.0, min(1.0, d))
        a = math.acos(d)
        if ux*vy - uy*vx < 0: a = -a
        return a
    th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sf and dth > 0: dth -= 2 * math.pi
    if sf and dth < 0: dth += 2 * math.pi
    for k in range(1, n + 1):
        th = th1 + dth * k / n
        ex = cosp * rx * math.cos(th) - sinp * ry * math.sin(th) + cx
        ey = sinp * rx * math.cos(th) + cosp * ry * math.sin(th) + cy
        out.append((ex, ey))

# ---------------------------------------------------------------- rasterise

def winding(subpaths, x, y):
    w = 0
    for sp in subpaths:
        for i in range(len(sp) - 1):
            x1, y1 = sp[i]; x2, y2 = sp[i + 1]
            if (y1 <= y) != (y2 <= y):
                xin = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if x < xin:
                    w += 1 if y2 > y1 else -1
    return w

def inside(subpaths, x, y, evenodd):
    w = winding(subpaths, x, y)
    return (w % 2) != 0 if evenodd else w != 0

def rasterise(paths, vb, size, ss=4):
    vx, vy, vw, vh = vb
    W = H = size
    sx = W / vw
    sy = H / vh
    out = bytearray(W * H * 4)
    for py in range(H):
        for px in range(W):
            acc = 1.0
            for (subpaths, opacity, evenodd) in paths:
                hits = 0
                for j in range(ss):
                    for i in range(ss):
                        ux = vx + (px + (i + 0.5) / ss) / sx
                        uy = vy + (py + (j + 0.5) / ss) / sy
                        if inside(subpaths, ux, uy, evenodd):
                            hits += 1
                cov = hits / (ss * ss) * opacity
                acc *= (1.0 - cov)
            a = int(round((1.0 - acc) * 255))
            o = (py * W + px) * 4
            out[o] = out[o + 1] = out[o + 2] = 255
            out[o + 3] = a
    return bytes(out), W, H

# ---------------------------------------------------------------- PNG

def write_png(path, data, w, h):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff))
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += data[y * w * 4:(y + 1) * w * 4]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)

# ---------------------------------------------------------------- main

def main():
    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    svg = open(src, "r", encoding="utf-8", errors="ignore").read()
    m = re.search(r'viewBox="([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)"', svg)
    vb = tuple(map(float, m.groups())) if m else (0.0, 0.0, 24.0, 24.0)
    paths = []
    for pm in re.finditer(r"<path\b([^>]*?)/?>", svg, re.S):
        attrs = pm.group(1)
        dm = re.search(r'\bd="([^"]+)"', attrs)
        if not dm:
            continue
        fill = re.search(r'\bfill="([^"]+)"', attrs)
        if fill and fill.group(1).strip().lower() == "none":
            continue
        op = 1.0
        om = re.search(r'\b(?:fill-)?opacity="([-\d.]+)"', attrs)
        if om:
            op = float(om.group(1))
        eo = bool(re.search(r'fill-rule="evenodd"', attrs))
        paths.append((parse_path(dm.group(1)), op, eo))
    if not paths:
        sys.exit("no drawable <path> in " + src)
    data, w, h = rasterise(paths, vb, size)
    write_png(dst, data, w, h)

if __name__ == "__main__":
    main()
