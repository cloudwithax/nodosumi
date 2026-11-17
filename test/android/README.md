Android frontend regression checks
=================================

Build the debug app, then run the native checks on a connected arm64 Android
device. This does not install the APK or replace the installed app.

```sh
(cd android && ./gradlew :app:assembleDebug)
ANDROID_SERIAL=<adb-device-id> test/android/run.sh
```

The runner uses Android SDK/NDK paths from `ANDROID_HOME`, `ANDROID_SDK_ROOT`,
and `ANDROID_NDK_HOME`, falling back to `~/Android/Sdk` and the app's pinned NDK.

The tests exercise JNI trigger pressure and digital fallback on all four ports,
rumble strengths and stop state, the real TA widescreen projection, unchanged
RTT projection, RTT depth/stencil invalidation, framebuffer resizing, and pixel
comparisons against a reference blit in landscape and portrait at 1x, 2x and 4x.
A 2560x1440 target also covers the trimmed 16:9 framebuffer. Comparisons poison
the destination first to detect incomplete presentation and stale borders.

These tests verify native state and rendering, not physical vibration motors or
Android controller key/axis events. Check partial trigger travel, simultaneous
shoulder/trigger input, disconnects, menu/background cancellation, and long
rumble effects on actual controllers before release.

Bot benchmarks in the installed app
-----------------------------------

`benchmark` is an optimized, debuggable APK signed with the release key when
configured. Its exported `BenchmarkActivity` exists only in this source set.
Installing with `adb install -r` preserves normal app data. The activity uses
`files/benchmark/system` for BIOS/flash/VMUs/shader caches; normal settings are
not changed. BIOS files are copied from the normal app at launch.

```sh
(cd android && ./gradlew :app:assembleBenchmark :app:assembleRelease)
adb install -r android/app/build/outputs/apk/benchmark/app-benchmark.apk
ANDROID_SERIAL=<device> tools/android/benchmark.py \
  --game '<device-path-to-game>' --bot drive.bot --resolution 2560x1920 \
  --aspect 16:9 --seconds 60 --first-frame 420 --last-frame 2220 \
  --output /tmp/nodosumi-results/4x-optimized
```

Use `--baseline` to disable the framebuffer optimizations in the same binary.
Use the same save state and bot for each run. Match measured frame ranges and
check the screenshots to ensure the bot is in gameplay. Bot scripts use core
frames, not wall-clock time; `r=0.5` applies half trigger pressure. `load` and
`save` accept paths in the benchmark app's own files directory.

Results include raw frame traces, configuration, battery temperatures, a
screenshot, logcat, and a JSON summary. GPU timing covers core draws; Android
presentation spans measure CPU submission time. Emulation FPS is capped by the
normal frontend pacing. Debugger-capable ART and tracing add overhead equally
to both cases, so small differences need repeats and should not be treated as
release FPS gains. After benchmarking, reinstall `app-release.apk` to remove
the benchmark entry point while retaining app data.
