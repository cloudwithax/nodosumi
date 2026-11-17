#!/usr/bin/env bash
# Build the debug app first: cd android && ./gradlew :app:assembleDebug
set -euo pipefail
cd "$(dirname "$0")/../.."
sdk_dir=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
ndk_dir=${ANDROID_NDK_HOME:-$sdk_dir/ndk/27.2.12479018}
core_lib=${NODOSUMI_TEST_CORE:-$(rg --files --hidden --no-ignore android/app/build/intermediates/cxx/Debug | rg '/obj/arm64-v8a/libnodosumi_libretro.so$' | head -n 1)}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
"$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang" \
  -O2 -I deps/libretro/include -I src test/android/frontend_test.c \
  -L "$(dirname "$core_lib")" -lnodosumi_libretro \
  -lEGL -lGLESv3 -laaudio -landroid -llog -ldl -o "$test_dir/frontend-test"
adb shell mkdir -p /data/local/tmp/nodosumi-tests
adb push "$test_dir/frontend-test" "$core_lib" /data/local/tmp/nodosumi-tests/
adb shell 'cd /data/local/tmp/nodosumi-tests && LD_LIBRARY_PATH=. ./frontend-test'
