# Builds the nodosumi logo in Blender and renders the 2D exports.
#
#   blender -b -P branding/build_logo.py [-- preview]
#
# The ensō is modelled as a bundle of fine bristle curves swept along an open
# circular path. Bristles run dry as the stroke goes on and start skipping, so
# the tail breaks into dry-brush streaks (kasure). A 墨 (sumi, ink) built from
# even-width sans strokes, painted with the same brush, sits in the middle.
# Rendered top-down and flat in a single color on a transparent background.

import math
import os
import random
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "export")
PREVIEW = "preview" in sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else False

ACCENT = "#E8470A"  # swirl orange

random.seed(1999)


def lin(hex_):
    def c(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    h = hex_.lstrip("#")
    return tuple(c(int(h[i:i + 2], 16)) for i in (0, 2, 4))


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


# --- scene -----------------------------------------------------------------

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "FLAT"
scene.display.shading.color_type = "MATERIAL"
scene.display.render_aa = "32"
scene.render.film_transparent = True
scene.view_settings.view_transform = "Standard"
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"

ink = bpy.data.materials.new("ink")
ink.diffuse_color = lin(ACCENT) + (1.0,)

# --- brush -----------------------------------------------------------------


def noise(x, seed):
    return (math.sin(x + seed) * 0.5 + math.sin(x * 2.3 + seed * 1.7) * 0.3
            + math.sin(x * 5.1 + seed * 2.9) * 0.2)


def brush(curve, path, length, width, dryness, bristles, landing=0.014, entry=None):
    """Paint one stroke into `curve` as a bundle of bristles.

    path(t) -> (x, y) for t in 0..1, length is its rough arc length,
    width(t) the stroke width, dryness(t) 0 (loaded) .. 1 (bone dry).
    entry(u) optionally gives where bristle u (-1..1 across) touches down."""

    def frame(t):
        x0, y0 = path(max(t - 1e-4, 0))
        x1, y1 = path(min(t + 1e-4, 1))
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy)
        return path(t), (-dy / n, dx / n)

    samples = max(80, int(length * 170))
    bristle_r = width(0.05) / bristles * 1.35
    for i in range(bristles):
        u = -1 + 2 * (i + random.uniform(0.2, 0.8)) / bristles
        edge = abs(u)
        seed = random.uniform(0, 100)
        freq = random.uniform(2.5, 6.0) * length
        phase = random.uniform(0, 2 * math.pi)
        if entry:
            t_start = entry(u)
        else:
            # rounded landing: outer bristles touch down a touch later
            t_start = landing * (1 - math.sqrt(max(1 - u * u, 0))) + random.uniform(0, landing / 7)
        t_end = 1.0 - 0.12 * edge ** 2 * random.uniform(0.3, 1.0) - random.uniform(0, 0.025)

        segs, cur = [], []
        for s in range(samples + 1):
            t = s / samples
            # a dry bristle skips the paper; edges run dry first
            dry = min(dryness(t) * (0.75 + 0.5 * edge), 1.0)
            if t_start <= t <= t_end and noise(t * freq, seed) > -1.0 + 1.5 * dry:
                cur.append(t)
                continue
            if len(cur) > 3:
                segs.append(cur)
            cur = []
        if len(cur) > 3:
            segs.append(cur)

        seg_first = segs[0][0] if segs else None
        for seg in segs:
            ta, tb = seg[0], seg[-1]
            sp = curve.splines.new("POLY")
            sp.points.add(len(seg) - 1)
            for p, t in zip(sp.points, seg):
                (x, y), (nx, ny) = frame(t)
                splay = (0.01 + 0.06 * dryness(t)) * math.sin(7 * t + phase)
                off = (u + splay) * width(t) / 2
                # bristles taper where they lift off; on a shaped entry they land full
                k_in = (smoothstep(ta - 0.001, ta + 0.002, t) if entry and ta == seg_first
                        else smoothstep(ta - 0.004, ta + 0.01, t))
                k = min(k_in, smoothstep(tb + 0.004, tb - 0.02, t))
                p.co = (x + nx * off, y + ny * off, 0.0, 1.0)
                p.radius = bristle_r * (0.45 + 0.55 * k)


def bristle_curve(name):
    curve = bpy.data.curves.new(name, "CURVE")
    curve.dimensions = "3D"
    curve.bevel_depth = 1.0
    curve.bevel_resolution = 1
    ob = bpy.data.objects.new(name, curve)
    ob.data.materials.append(ink)
    scene.collection.objects.link(ob)
    return curve


# --- ensō ------------------------------------------------------------------

R = 1.0               # path radius
W = 0.27              # stroke width where the brush lands
THETA0 = math.radians(228)
SWEEP = math.radians(318)


def enso_path(t):
    # a hand-drawn circle, not a compass one: just off true, with faint flats
    # and bulges and a little drift as the hand comes around
    theta = THETA0 - SWEEP * t
    r = R * (1
             + 0.008 * math.cos(2 * (theta - 0.6))      # egg
             + 0.005 * math.sin(3 * theta + 1.1)        # lumps
             + 0.003 * math.sin(5 * theta + 2.3)
             - 0.05 * t ** 2.5)                         # spirals in at the end
    drift_x, drift_y = 0.01 * t, -0.008 * t * t
    return r * math.cos(theta) + drift_x, r * 0.975 * math.sin(theta) + drift_y


def enso_width(t):
    # pressed in hard on landing, then settles
    land = 1.0 + 0.14 * smoothstep(0.0, 0.02, t) * (1 - smoothstep(0.02, 0.09, t))
    lift = 1.0 - 0.38 * smoothstep(0.08, 0.75, t) - 0.45 * smoothstep(0.7, 1.0, t)
    # pressure never holds perfectly steady
    pressure = 1.0 + 0.02 * math.sin(2 * math.pi * 2.3 * t + 0.4) \
        + 0.01 * math.sin(2 * math.pi * 5.7 * t + 2.1)
    return W * land * lift * pressure


def enso_entry(u):
    # the brush tip comes down at an angle and rounds off as it presses: a
    # slanted start where the inner edge lands first, with a rounded heel.
    # It's the wettest point, so keep it crisp.
    a = (1 + u) / 2
    return 0.02 * a + 0.016 * (1 - math.sqrt(max(1 - u * u, 0))) + random.uniform(0, 0.001)


def enso_dryness(t):
    # streaky all the way round, driest at the tail; the landing stays wet
    return smoothstep(0.0, 0.06, t) * (0.34 + 0.66 * smoothstep(0.35, 1.0, t))


brush(bristle_curve("enso"), enso_path, R * SWEEP, enso_width, enso_dryness, 240,
      entry=enso_entry)

# --- 墨 (sumi, ink) ----------------------------------------------------------
#
# Drawn as even-width sans strokes, each painted with the same brush. Points
# are on a 1000-unit em, y down, listed in the direction the brush travels.

GLYPH_STROKES = [
    # (points, free end?) strokes that end in open space run drier at the end
    ([(220, 382), (220, 182), (785, 182), (785, 382)], False),  # 里: box
    ([(497, 182), (497, 570)], False),               # spine
    ([(220, 278), (785, 278)], False),               # middle bar
    ([(220, 378), (785, 378)], False),               # box bottom
    ([(170, 467), (845, 467)], True),                # lower bar
    ([(100, 570), (900, 570)], True),                # long bar
    ([(245, 625), (135, 728)], True),                # 灬
    ([(385, 628), (418, 718)], True),
    ([(572, 628), (622, 715)], True),
    ([(752, 625), (862, 722)], True),
    ([(185, 792), (815, 792)], True),                # 土: top bar
    ([(497, 705), (497, 912)], False),               #     stem
    ([(90, 912), (910, 912)], True),                 #     base
]
GLYPH_SCALE = 1.08 / 1000   # em -> scene units
GLYPH_CENTER = (500, 547)
GLYPH_W = 74 * GLYPH_SCALE


def polyline(points, corner):
    """Arc-length path through `points`, with turns rounded to `corner`."""
    pts = [points[0]]
    for (ax, ay), (bx, by), (cx, cy) in zip(points, points[1:], points[2:]):
        # replace the corner at b with a quarter-ish arc sampled finely
        l1, l2 = math.hypot(bx - ax, by - ay), math.hypot(cx - bx, cy - by)
        p0 = (bx - (bx - ax) / l1 * corner, by - (by - ay) / l1 * corner)
        p2 = (bx + (cx - bx) / l2 * corner, by + (cy - by) / l2 * corner)
        for k in range(17):
            t = k / 16
            pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * bx + t * t * p2[0],
                        (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * by + t * t * p2[1]))
    pts.append(points[-1])
    acc = [0.0]
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        acc.append(acc[-1] + math.hypot(bx - ax, by - ay))
    total = acc[-1]

    def path(t):
        d = t * total
        for i in range(1, len(acc)):
            if d <= acc[i] or i == len(acc) - 1:
                f = (d - acc[i - 1]) / max(acc[i] - acc[i - 1], 1e-9)
                (ax, ay), (bx, by) = pts[i - 1], pts[i]
                return ax + (bx - ax) * f, ay + (by - ay) * f

    return path, total


def glyph_dryness(t, free):
    # same streak as the ensō throughout; open ends run drier still
    base = smoothstep(0.0, 0.06, t) * 0.34
    return base + (0.4 * smoothstep(0.65, 1.0, t) if free else 0.0)


glyph = bristle_curve("glyph")
for stroke, free in GLYPH_STROKES:
    pts = [((x - GLYPH_CENTER[0]) * GLYPH_SCALE, (GLYPH_CENTER[1] - y) * GLYPH_SCALE)
           for x, y in stroke]
    path, length = polyline(pts, GLYPH_W * 0.6)
    taper = 0.1 if free else 0.0
    brush(glyph, path, length,
          lambda t, taper=taper: GLYPH_W * (1.0 - taper * smoothstep(0.7, 1.0, t)),
          lambda t, free=free: glyph_dryness(t, free),
          70, landing=0.04 * min(1.0, 0.3 / length))

# --- camera & render -------------------------------------------------------

cam_data = bpy.data.cameras.new("cam")
cam_data.type = "ORTHO"
cam_data.ortho_scale = 2.78
cam = bpy.data.objects.new("cam", cam_data)
cam.location = (0.0, 0.0, 10)
scene.collection.objects.link(cam)
scene.camera = cam

os.makedirs(OUT, exist_ok=True)
size = 1024 if PREVIEW else 2048
scene.render.resolution_x = scene.render.resolution_y = size
scene.render.filepath = os.path.join(OUT, "preview.png" if PREVIEW else "nodosumi-logo.png")
bpy.ops.render.render(write_still=True)

if not PREVIEW:
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(HERE, "nodosumi_logo.blend"))
