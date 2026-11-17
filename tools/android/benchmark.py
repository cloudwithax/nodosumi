#!/usr/bin/env python3
"""Run a bot in the optimized benchmark APK and collect an ADB trace.

Build/install :app:assembleBenchmark first. See test/android/README.md.
The benchmark activity isolates VMU/flash/shader files from the normal app.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import shlex
import statistics
import struct
import subprocess
import time

PACKAGE = 'io.github.cloudwithax.nodosumi'
RECORD = struct.Struct('<QIHBBq')


def adb(*args, **kwargs):
    return subprocess.run(['adb', *args], check=True, stdout=subprocess.PIPE, **kwargs).stdout


def shell(*args, **kwargs):
    return adb('shell', shlex.join(args), **kwargs)


def summarize(trace, first_frame=420, last_frame=None):
    names = {}
    for line in Path(str(trace) + '.names').read_text().splitlines():
        fields = line.split()
        if fields and fields[0].isdigit():
            names[int(fields[0])] = fields[1]
    stacks = collections.defaultdict(list)
    spans = collections.defaultdict(list)
    counters = collections.defaultdict(list)
    data = trace.read_bytes()
    for offset in range(0, len(data) - RECORD.size + 1, RECORD.size):
        ts, tid, event, phase, _, value = RECORD.unpack_from(data, offset)
        name = names.get(event, str(event))
        if phase == 1:
            stacks[tid, event].append((ts, value))
        elif phase == 2 and stacks[tid, event]:
            begin, arg = stacks[tid, event].pop()
            spans[name].append((begin, ts, arg, value))
        elif phase == 3:
            counters[name].append((ts, value))
    frames = sorted(s for s in spans['retro_run'] if s[2] >= first_frame and (last_frame is None or s[2] < last_frame))
    if not frames:
        raise RuntimeError('No measured core frames; inspect logcat.txt for a startup error')
    if last_frame is not None and (frames[0][2] != first_frame or frames[-1][2] != last_frame - 1):
        raise RuntimeError('Incomplete measured frame range; increase --seconds')
    start, end = frames[0][0], frames[-1][1]

    def stats(values):
        values = sorted(values)
        if not values:
            return None
        return {'mean_ms': round(statistics.mean(values), 3),
                'p50_ms': round(statistics.median(values), 3),
                'p95_ms': round(values[int((len(values) - 1) * .95)], 3),
                'p99_ms': round(values[int((len(values) - 1) * .99)], 3)}

    result = {'measured_seconds': round((end-start)/1e9, 3),
              'core_frames': len(frames),
              'emulation_fps': round((len(frames)-1)*1e9/(frames[-1][0]-start), 2),
              'first_frame': frames[0][2], 'last_frame': frames[-1][2]}
    for name in ('retro_run', 'emu_frame', 'android_frame', 'android_present', 'begin_frame'):
        samples = [s for s in spans[name] if start <= s[0] <= end]
        result[name] = stats([(s[1]-s[0])/1e6 for s in samples])
    display = sorted(s[0] for s in spans['android_frame'] if start <= s[0] <= end)
    intervals = [(b-a)/1e6 for a, b in zip(display, display[1:])]
    result['display_intervals'] = stats(intervals)
    result['display_intervals_over_25ms'] = sum(v > 25 for v in intervals)
    for counter in ('gl_draws', 'verts', 'vid_src'):
        samples = [v for ts, v in counters[counter] if start <= ts <= end]
        result[counter] = round(statistics.mean(samples), 2) if samples else None
    result['gpu_draw'] = stats([v/1e6 for ts, v in counters['gpu_ns'] if start <= ts <= end and v >= 0])
    result['dropped_trace_events'] = sum(v for _, v in counters['dropped'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True)
    parser.add_argument('--bot', type=Path, required=True)
    parser.add_argument('--resolution', default='1280x960')
    parser.add_argument('--aspect', default='4:3', choices=['4:3', '16:9', 'stretch'])
    parser.add_argument('--wide', action='store_true')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--seconds', type=int, default=45)
    parser.add_argument('--first-frame', type=int, default=420)
    parser.add_argument('--last-frame', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    shell('am', 'force-stop', PACKAGE)
    shell('run-as', PACKAGE, 'mkdir', '-p', 'files/benchmark')
    bot = args.bot.read_bytes()
    shell('run-as', PACKAGE, 'sh', '-c', 'cat > files/benchmark/bot.txt', input=bot)
    (args.output/'bot.txt').write_bytes(bot)
    (args.output/'battery-before.txt').write_bytes(shell('dumpsys', 'battery'))
    logger = subprocess.Popen(['adb', 'logcat', '-v', 'threadtime', '-T', '1'],
                              stdout=(args.output/'logcat.txt').open('wb'))
    try:
        launch = shell('am', 'start', '-W', '-n', PACKAGE+'/.BenchmarkActivity',
                       '--es', 'game', args.game, '--es', 'resolution', args.resolution,
                       '--es', 'aspect', args.aspect, '--ez', 'wide', str(args.wide).lower(),
                       '--ez', 'baseline', str(args.baseline).lower())
        print(launch.decode(), flush=True)
        for elapsed in range(args.seconds):
            time.sleep(1)
            if (elapsed + 1) % 15 == 0:
                print(f'{elapsed+1}/{args.seconds}s', flush=True)
        (args.output/'screen.png').write_bytes(adb('exec-out', 'screencap', '-p'))
        shell('input', 'keyevent', 'KEYCODE_HOME')
        time.sleep(1)
        for name in ('trace.bin', 'trace.bin.names'):
            (args.output/name).write_bytes(adb('exec-out', 'run-as', PACKAGE, 'cat', 'files/benchmark/'+name))
        (args.output/'battery-after.txt').write_bytes(shell('dumpsys', 'battery'))
    finally:
        logger.terminate()
        logger.wait()
    result = summarize(args.output/'trace.bin', args.first_frame, args.last_frame)
    result['configuration'] = {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}
    (args.output/'summary.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
