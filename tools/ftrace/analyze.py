#!/usr/bin/env python3
"""Merge the core's NODOSUMI_FTRACE trace, slot's SLOT_FTRACE trace and an
optional `perf script` dump on CLOCK_MONOTONIC, then break every frame down
and attribute each dip.

  analyze.py --core nodo.bin [--slot slot.bin] [--perf perf.txt]
             [--so nodosumi_libretro.so] [--addr2line CMD] [--out DIR]

Writes a summary to stdout and per-frame CSVs to --out."""

import argparse
import bisect
import collections
import csv
import os
import shutil
import struct
import subprocess
import sys

REC = struct.Struct("<QIHBBq")
INSTANT, BEGIN, END, COUNTER, ZONE_NS, ZONE_N = range(6)
BUDGET = 1e9 / 60


def load(path):
    names, threads, tick_freq = {}, {}, None
    npath = path + ".names"
    if os.path.exists(npath):
        for line in open(npath):
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "thread":
                threads[int(parts[1])] = parts[2]
            elif parts[0] == "tick_freq":
                tick_freq = int(parts[1])
            else:
                names[int(parts[0])] = parts[1]
    tpath = path + ".threads"
    if os.path.exists(tpath):
        for line in open(tpath):
            parts = line.split()
            if len(parts) >= 3:
                threads.setdefault(int(parts[0]), parts[2])
    data = open(path, "rb").read()
    n = len(data) // REC.size
    recs = [REC.unpack_from(data, i * REC.size) for i in range(n)]
    recs.sort(key=lambda r: r[0])
    return names, threads, recs


def symbolizer(so, addr2line):
    cache = {}

    def sym(off):
        if off in cache:
            return cache[off]
        name = hex(off)
        if so and addr2line:
            try:
                out = subprocess.run(addr2line + ["-f", "-C", "-e", so, hex(off)],
                                     capture_output=True, text=True).stdout.split("\n")
                if out and out[0] and out[0] != "??":
                    name = out[0]
            except OSError:
                pass
        cache[off] = name
        return name

    return sym


class Thread:
    def __init__(self, tid, name):
        self.tid, self.name = tid, name
        self.spans = collections.defaultdict(list)  # name -> [(b, e, barg, earg)]
        self.open = collections.defaultdict(list)
        self.instants = collections.defaultdict(list)  # name -> [(t, arg)]
        self.counters = collections.defaultdict(list)  # name -> [(t, val)]
        self.flushes = []  # (t, {zone: (ns, n)})
        self._zones = None
        self._zt = None


def build(names, threads, recs, sym, core_base):
    th = {}

    def get(tid):
        if tid not in th:
            th[tid] = Thread(tid, threads.get(tid, str(tid)))
        return th[tid]

    for t, tid, ev, ph, _, arg in recs:
        name = names.get(ev, str(ev))
        if name.startswith("ptr:") and core_base is not None:
            name = "cb:" + sym(int(name[4:], 16) - core_base)
        T = get(tid)
        if ph == BEGIN:
            T.open[name].append((t, arg))
        elif ph == END:
            if T.open[name]:
                b, barg = T.open[name].pop()
                T.spans[name].append((b, t, barg, arg))
        elif ph == INSTANT:
            T.instants[name].append((t, arg))
        elif ph == COUNTER:
            T.counters[name].append((t, arg))
        elif ph in (ZONE_NS, ZONE_N):
            if T._zt != t:
                T._zones = {}
                T._zt = t
                T.flushes.append((t, T._zones))
            ns, n = T._zones.get(name, (0, 0))
            T._zones[name] = (arg, n) if ph == ZONE_NS else (ns, arg)
    return th


def find_thread(th, name):
    for T in th.values():
        if T.name == name:
            return T
    return None


def counter_at(T, name, t):
    """The last value of a counter at or before t (flush counters share t)."""
    vals = T.counters.get(name)
    if not vals:
        return None
    i = bisect.bisect_right([v[0] for v in vals], t) - 1
    return vals[i][1] if i >= 0 else None


def in_window(lst, b, e):
    return [x for x in lst if b <= x[0] < e]


class Index:
    """Sorted lookups of events on one thread."""

    def __init__(self, items):
        self.items = sorted(items)
        self.keys = [x[0] for x in self.items]

    def between(self, b, e):
        return self.items[bisect.bisect_left(self.keys, b):bisect.bisect_left(self.keys, e)]


def frame_rows(T, span_name, extra_spans, extra_instants):
    """One row per span_name span on T: duration, zones flushed at its end,
    deltas of the flush counters, and the sub-spans/instants inside it."""
    rows = []
    flush_t = [f[0] for f in T.flushes]
    cnt = {k: Index(T.counters.get(k, [])) for k in ("cpu_ns", "nvcsw", "nivcsw", "minflt", "majflt", "cpu")}
    prev = {}
    sub = {k: Index([(b, e, ba, ea) for b, e, ba, ea in T.spans.get(k, [])]) for k in extra_spans}
    inst = {k: Index(T.instants.get(k, [])) for k in extra_instants}
    for b, e, barg, earg in sorted(T.spans.get(span_name, [])):
        row = {"t": b, "dur": e - b, "arg": barg, "earg": earg}
        # the flush that closes this frame is the first one at or after e
        i = bisect.bisect_left(flush_t, e)
        if i < len(flush_t) and flush_t[i] - e < 2_000_000:
            row["zones"] = T.flushes[i][1]
            ft = flush_t[i]
            for k, idx in cnt.items():
                hits = idx.between(ft, ft + 1)
                if hits:
                    v = hits[-1][1]
                    row[k] = v if k == "cpu" else v - prev.get(k, v)
                    prev[k] = v
        else:
            row["zones"] = {}
        for k, idx in sub.items():
            s = idx.between(b, e)
            row["sp:" + k] = sum(x[1] - x[0] for x in s)
            row["spn:" + k] = len(s)
        for k, idx in inst.items():
            row["in:" + k] = idx.between(b, e)
        rows.append(row)
    return rows


def ms(ns):
    return "%.2f" % (ns / 1e6)


def zone_table(rows, title, top=40):
    tot = collections.defaultdict(int)
    cnt = collections.defaultdict(int)
    for r in rows:
        for z, (ns, n) in r["zones"].items():
            tot[z] += ns
            cnt[z] += n
    if not rows:
        return
    print("  %s (%d frames), mean exclusive ms/frame:" % (title, len(rows)))
    for z, ns in sorted(tot.items(), key=lambda x: -x[1])[:top]:
        print("    %-28s %7.3f ms  %9.1f calls" % (z, ns / len(rows) / 1e6, cnt[z] / len(rows)))


def zone_diff(slow, fast, top=25):
    def mean(rows):
        m = collections.defaultdict(float)
        for r in rows:
            for z, (ns, _) in r["zones"].items():
                m[z] += ns
        return {z: v / max(1, len(rows)) for z, v in m.items()}

    ms_, mf = mean(slow), mean(fast)
    keys = set(ms_) | set(mf)
    diff = sorted(((ms_.get(z, 0) - mf.get(z, 0), z) for z in keys), reverse=True)
    print("  zone          slow-frame mean  normal mean   extra")
    for d, z in diff[:top]:
        print("    %-28s %7.3f  %7.3f  %+7.3f ms" % (z, ms_.get(z, 0) / 1e6, mf.get(z, 0) / 1e6, d / 1e6))


def write_csv(path, rows, zone_keys):
    keys = ["t", "dur", "arg", "earg", "cpu_ns", "nvcsw", "nivcsw", "minflt", "majflt", "cpu"]
    extra = sorted({k for r in rows for k in r if k.startswith(("sp:", "spn:"))})
    with open(path, "w", newline="") as fp:
        w = csv.writer(fp)
        w.writerow(keys + extra + list(zone_keys))
        for r in rows:
            w.writerow([r.get(k, "") for k in keys] + [r.get(k, "") for k in extra] +
                       [r["zones"].get(z, (0, 0))[0] for z in zone_keys])


def load_perf(path):
    """perf script -F comm,tid,time,ip,sym,dso output, callchains or not: the
    first line of each sample is its header, the next its leaf frame."""
    samples = []
    cur = None
    for line in open(path, errors="replace"):
        if not line.strip():
            cur = None
            continue
        if not line.startswith((" ", "\t")):
            # comm tid time: [ip sym (dso)]
            try:
                head, rest = line.split(":", 1) if ": " in line else (line, "")
                parts = head.split()
                t = int(float(parts[-1]) * 1e9)
                tid = int(parts[-2])
                comm = " ".join(parts[:-2])
            except (ValueError, IndexError):
                continue
            cur = [t, tid, comm, None]
            samples.append(cur)
            rest = rest.strip()
            if rest:
                cur[3] = leaf_sym(rest)
        elif cur is not None and cur[3] is None:
            cur[3] = leaf_sym(line.strip())
    samples.sort()
    return samples


def leaf_sym(s):
    parts = s.split(None, 1)
    if len(parts) < 2:
        return s
    sym = parts[1]
    return sym


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", required=True)
    ap.add_argument("--slot")
    ap.add_argument("--perf")
    ap.add_argument("--so")
    ap.add_argument("--addr2line")
    ap.add_argument("--out", default="ftrace-out")
    ap.add_argument("--skip-s", type=float, default=3.0, help="seconds to drop after the start or the bot's state load")
    ap.add_argument("--secs", type=float, default=0, help="seconds to keep after the start")
    ap.add_argument("--worst", type=int, default=30, help="dips to print in full")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    addr2line = a.addr2line.split() if a.addr2line else (
        ["aarch64-linux-gnu-addr2line"] if shutil.which("aarch64-linux-gnu-addr2line") else None)
    names, threads, recs = load(a.core)
    core_base = None
    cb_id = next((k for k, v in names.items() if v == "core_base"), None)
    for r in recs:
        if r[2] == cb_id:
            core_base = r[5]
            break
    sym = symbolizer(a.so, addr2line)
    if not recs:
        sys.exit("empty core trace")
    t0 = recs[0][0] + int(a.skip_s * 1e9)
    # a bot run starts counting at its state load (the second line it runs)
    bot_id = next((k for k, v in names.items() if v == "bot_line"), None)
    bot = [r[0] for r in recs if r[2] == bot_id]
    if len(bot) >= 2:
        t0 = bot[1] + int(a.skip_s * 1e9)
    t_end = t0 + int(a.secs * 1e9) if a.secs else None
    if t_end:
        recs = [r for r in recs if r[0] < t_end]
    th = build(names, threads, recs, sym, core_base)

    print("core trace: %d records, %.1f s, threads: %s" % (
        len(recs), (recs[-1][0] - recs[0][0]) / 1e9,
        ", ".join("%s(%d)" % (T.name, T.tid) for T in th.values())))
    for T in th.values():
        d = T.counters.get("dropped")
        if d:
            print("  DROPPED records on %s: %d" % (T.name, d[-1][1]))

    emu = find_thread(th, "emu")
    vid = find_thread(th, "video")
    chd = find_thread(th, "chd")

    erows = frame_rows(emu, "emu_frame", ["start_render", "start_render_wait", "finish_render_wait", "chd_wait"],
                       ["vblank_in", "ta_render", "gd_read", "jit_compile", "jit_flush", "jit_fastmem_fault",
                        "chd_miss", "skip"]) if emu else []
    vrows = frame_rows(vid, "retro_run", ["begin_frame", "convert", "render", "end_frame", "end_frame_wait",
                                          "video_cb"], ["emu_sync"]) if vid else []
    erows = [r for r in erows if r["t"] >= t0]
    vrows = [r for r in vrows if r["t"] >= t0]

    # GPU time of each retro_run's begin_frame, keyed by the frame number
    gpu = {}
    if vid:
        gf, gn = vid.counters.get("gpu_frame", []), vid.counters.get("gpu_ns", [])
        for (t1, f), (t2, ns) in zip(gf, gn):
            gpu[f] = ns
    for r in vrows:
        r["gpu_ns"] = gpu.get(r["arg"])
        for k in ("verts", "surfs", "params"):
            r[k] = None
    if vid:
        for k in ("verts", "surfs", "params", "vid_src"):
            idx = Index(vid.counters.get(k, []))
            for r in vrows:
                hits = idx.between(r["t"], r["t"] + r["dur"])
                r[k] = sum(h[1] for h in hits) if hits else None

    zkeys = sorted({z for r in erows + vrows for z in r["zones"]})
    write_csv(os.path.join(a.out, "emu_frames.csv"), erows, zkeys)
    write_csv(os.path.join(a.out, "video_frames.csv"), vrows, zkeys)

    def stats(rows, key, title):
        v = sorted(r[key] for r in rows if r.get(key) is not None)
        if not v:
            return
        pct = lambda p: v[min(len(v) - 1, int(len(v) * p))]
        print("  %-26s n=%5d mean %6s p50 %6s p90 %6s p99 %6s max %6s ms  >16.7: %d" % (
            title, len(v), ms(sum(v) / len(v)), ms(pct(.5)), ms(pct(.9)), ms(pct(.99)), ms(v[-1]),
            sum(1 for x in v if x > BUDGET)))

    print("\n== core frame times")
    stats(erows, "dur", "emu_frame wall")
    stats(erows, "cpu_ns", "emu_frame cpu")
    stats(vrows, "dur", "retro_run wall")
    stats(vrows, "cpu_ns", "retro_run cpu")
    stats(vrows, "gpu_ns", "gpu (begin_frame)")
    for k in ("begin_frame", "convert", "render", "end_frame", "end_frame_wait", "video_cb"):
        stats(vrows, "sp:" + k, "video " + k)
    for k in ("start_render", "start_render_wait", "finish_render_wait"):
        stats(erows, "sp:" + k, "emu " + k)

    # emu work excluding the waits, the number that has to fit the budget
    for r in erows:
        w = sum(r["zones"].get(z, (0, 0))[0] for z in ("z_start_render_wait", "z_finish_render_wait", "z_chd_wait", "z_res_lock"))
        r["work"] = r["dur"] - w
    stats(erows, "work", "emu work (wall - waits)")

    print("\n== where the emu thread's time goes")
    zone_table(erows, "emu thread")
    print("\n== where the video thread's time goes")
    zone_table(vrows, "video thread")
    if chd:
        crow = [(b, e) for b, e, _, _ in chd.spans.get("chd_decode", []) if b >= t0]
        if crow:
            d = sorted(e - b for b, e in crow)
            print("\n== chd worker: %d hunk decodes, mean %s ms, max %s ms" % (len(d), ms(sum(d) / len(d)), ms(d[-1])))
    if emu:
        misses = [x for x in emu.instants.get("chd_miss", []) if x[0] >= t0]
        print("  chd misses on the emu thread: %d" % len(misses))

    # migrations / preemption
    for rows, nm in ((erows, "emu"), (vrows, "video")):
        cpus = collections.Counter(r.get("cpu") for r in rows)
        inv = sum(r.get("nivcsw") or 0 for r in rows)
        vol = sum(r.get("nvcsw") or 0 for r in rows)
        flt = sum(r.get("minflt") or 0 for r in rows)
        print("  %s: cpus %s, involuntary switches %d, voluntary %d, minor faults %d" % (nm, dict(cpus), inv, vol, flt))

    # ---- slot side: which presents showed a repeated picture, and why
    dips = []
    if a.slot:
        snames, sthreads, srecs = load(a.slot)
        if t_end:
            srecs = [r for r in srecs if r[0] < t_end]
        sth = build(snames, sthreads, srecs, sym, None)
        print("\nslot trace: %d records, threads: %s" % (len(srecs), ", ".join(
            "%s(%d)" % (T.name, T.tid) for T in sth.values())))
        pres = None
        for T in sth.values():
            if T.spans.get("present.swap"):
                pres = T
        semu = None
        for T in sth.values():
            if T.spans.get("emu.run_overlapped") or T.spans.get("emu.run_frame"):
                semu = T
        if pres:
            swaps = sorted(e for b, e, _, _ in pres.spans["present.swap"] if e >= t0)
            gaps = [(swaps[i], swaps[i] - swaps[i - 1]) for i in range(1, len(swaps))]
            g = sorted(x[1] for x in gaps)
            if g:
                print("  swap interval: mean %s p50 %s p99 %s max %s ms, >25ms: %d" % (
                    ms(sum(g) / len(g)), ms(g[len(g) // 2]), ms(g[int(len(g) * .99)]), ms(g[-1]),
                    sum(1 for x in g if x > 25e6)))
            rep = [t for t, _ in pres.instants.get("render.repeat", []) if t >= t0]
            frm = [t for t, _ in pres.instants.get("render.frame", []) if t >= t0]
            print("  presents with a new frame %d, repeated %d" % (len(frm), len(rep)))
            fps = [(t, v / 1000) for t, v in pres.counters.get("ui.fps_milli", []) if t >= t0]
            if fps:
                lo = [x for x in fps if x[1] < 59.5]
                print("  ui fps windows: %d, below 59.5: %d, min %.1f" % (len(fps), len(lo), min(f for _, f in fps)))
                with open(os.path.join(a.out, "fps.csv"), "w") as fp:
                    for t, f in fps:
                        fp.write("%.3f,%.2f\n" % ((t - t0) / 1e9, f))
            for t in rep:
                dips.append(("repeat", t))
        if semu:
            ov = semu.spans.get("emu.run_overlapped") or semu.spans.get("emu.run_frame")
            d = sorted(e - b for b, e, _, _ in ov if b >= t0)
            if d:
                print("  slot emu run call: mean %s p99 %s max %s ms" % (ms(sum(d) / len(d)), ms(d[int(len(d) * .99)]), ms(d[-1])))
            for nm in ("emu.ring_push", "emu.tick_wait", "emu.publish", "emu.take_audio"):
                s = sorted(e - b for b, e, _, _ in semu.spans.get(nm, []) if b >= t0)
                if s:
                    print("  slot %-18s mean %s p99 %s max %s ms" % (nm, ms(sum(s) / len(s)), ms(s[int(len(s) * .99)]), ms(s[-1])))
            gaps = [(t, v) for t, v in semu.counters.get("emu.tick_gap", []) if t >= t0 and v > 1]
            print("  emu missed ticks (tick_gap>1): %d" % len(gaps))
        for T in sth.values():
            for nm in ("ring.underrun", "audio.xrun"):
                n = [x for x in T.instants.get(nm, []) if x[0] >= t0]
                if n:
                    print("  %s on %s: %d" % (nm, T.name, len(n)))

    # Without slot, a dip is a retro_run past the budget.
    if not dips:
        dips = [("slow_run", r["t"] + r["dur"]) for r in vrows if r["dur"] > BUDGET]

    perf = load_perf(a.perf) if a.perf else []
    perf_t = [s[0] for s in perf]

    # attribute each dip: the core frames running in the 33 ms before it
    vidx = Index([(r["t"], r) for r in vrows])
    eidx = Index([(r["t"], r) for r in erows])
    slow_e, slow_v = [], []
    with open(os.path.join(a.out, "dips.txt"), "w") as fp:
        for kind, t in dips:
            vs = [r for _, r in vidx.between(t - 40_000_000, t)]
            es = [r for _, r in eidx.between(t - 40_000_000, t)]
            slow_e += es
            slow_v += vs
            fp.write("\n%s at %.3f s\n" % (kind, (t - t0) / 1e9))
            for r in es:
                fp.write("  emu   %.3f dur %s work %s cpu %s  %s\n" % (
                    (r["t"] - t0) / 1e9, ms(r["dur"]), ms(r["work"]), ms(r.get("cpu_ns") or 0),
                    "  ".join("%s=%s" % (z, ms(ns)) for z, (ns, _) in sorted(r["zones"].items(), key=lambda x: -x[1][0])[:10])))
                ev = ["%s:%d" % (k[3:], len(v)) for k, v in r.items() if k.startswith("in:") and v]
                fp.write("        events %s  nivcsw %s nvcsw %s cpu %s\n" % (" ".join(ev), r.get("nivcsw"), r.get("nvcsw"), r.get("cpu")))
            for r in vs:
                fp.write("  video %.3f dur %s gpu %s begin %s conv %s render %s endwait %s cb %s verts %s surfs %s\n" % (
                    (r["t"] - t0) / 1e9, ms(r["dur"]), ms(r["gpu_ns"] or 0), ms(r["sp:begin_frame"]),
                    ms(r["sp:convert"]), ms(r["sp:render"]), ms(r["sp:end_frame_wait"]), ms(r["sp:video_cb"]),
                    r.get("verts"), r.get("surfs")))
            if perf:
                i, j = bisect.bisect_left(perf_t, t - 33_000_000), bisect.bisect_left(perf_t, t)
                c = collections.Counter((s[2], s[3]) for s in perf[i:j])
                fp.write("  perf (33ms before): %s\n" % ", ".join("%s/%s %d" % (k[0], k[1], n) for k, n in c.most_common(12)))
    print("\n== %d dips, details in %s/dips.txt" % (len(dips), a.out))
    ids = {id(r) for r in slow_e}
    fast_e = [r for r in erows if id(r) not in ids]
    if slow_e:
        print("\n== emu zones in frames before a dip vs the rest")
        zone_diff(slow_e, fast_e)
    ids = {id(r) for r in slow_v}
    fast_v = [r for r in vrows if id(r) not in ids]
    if slow_v:
        print("\n== video zones in frames before a dip vs the rest")
        zone_diff(slow_v, fast_v)

    print("\n== slowest emu frames (work)")
    for r in sorted(erows, key=lambda r: -r["work"])[:a.worst]:
        print("  %8.3f s  wall %s work %s cpu %s  %s" % ((r["t"] - t0) / 1e9, ms(r["dur"]), ms(r["work"]),
                                                      ms(r.get("cpu_ns") or 0), "  ".join(
            "%s=%s" % (z, ms(ns)) for z, (ns, _) in sorted(r["zones"].items(), key=lambda x: -x[1][0])[:8])))
    print("\n== slowest retro_runs")
    for r in sorted(vrows, key=lambda r: -r["dur"])[:a.worst]:
        print("  %8.3f s  wall %s gpu %s  %s" % ((r["t"] - t0) / 1e9, ms(r["dur"]), ms(r["gpu_ns"] or 0), "  ".join(
            "%s=%s" % (z, ms(ns)) for z, (ns, _) in sorted(r["zones"].items(), key=lambda x: -x[1][0])[:8])))

    if perf:
        print("\n== perf, whole run, top symbols per thread")
        by = collections.defaultdict(collections.Counter)
        for s in perf:
            if s[0] >= t0:
                by[s[2]][s[3]] += 1
        for comm, c in sorted(by.items(), key=lambda x: -sum(x[1].values()))[:8]:
            tot = sum(c.values())
            print("  %s (%d samples)" % (comm, tot))
            for k, n in c.most_common(15):
                print("    %5.1f%% %s" % (100 * n / tot, k))


if __name__ == "__main__":
    main()
