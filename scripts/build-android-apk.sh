#!/bin/sh
# Packs the Android build (rexglue/build-android) into an APK, without Gradle:
# the Android SDK's own tools do each step, and the JDK only compiles SDL's
# Java activity, so any JDK from 17 on will do.
#
#   ./scripts/build-android-apk.sh            -> rexglue/build-android/burnout.apk
#   adb install -r rexglue/build-android/burnout.apk
#
# ANDROID_HOME (default /opt/android-sdk) needs build-tools and a platform;
# ANDROID_NDK_HOME (default /opt/android-ndk) gives libc++_shared.so.
# VALIDATION_LAYER, the path to an arm64 libVkLayer_khronos_validation.so, packs
# Khronos' validation layer in too (switched on by env.PLUME_VALIDATION = 1).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
project="$root/rexglue"
app="$project/android"
build="$project/build-android"
sdl_java="$root/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java"
runtime_libs="$root/rexglue-sdk/out/android-arm64"

sdk=${ANDROID_HOME:-/opt/android-sdk}
ndk=${ANDROID_NDK_HOME:-/opt/android-ndk}
tools=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
platform=$(ls -d "$sdk"/platforms/android-* | sort -V | tail -1)
android_jar="$platform/android.jar"
min_sdk=29
target_sdk=34

out="$build/apk"
rm -rf "$out"
mkdir -p "$out/gen" "$out/classes" "$out/dex" "$out/pack/lib/arm64-v8a"

echo "== resources and manifest ($(basename "$tools"), $(basename "$platform"))"
"$tools/aapt2" compile --dir "$app/res" -o "$out/res.zip"
"$tools/aapt2" link -o "$out/base.apk" -I "$android_jar" \
    --manifest "$app/AndroidManifest.xml" --java "$out/gen" \
    --min-sdk-version $min_sdk --target-sdk-version $target_sdk "$out/res.zip"

echo "== java"
find "$sdl_java" "$app/java" "$out/gen" -name '*.java' > "$out/sources.txt"
javac -nowarn -Xlint:none -source 17 -target 17 -encoding UTF-8 \
    -classpath "$android_jar" -d "$out/classes" @"$out/sources.txt" 2>&1 |
    grep -v "^warning: \[options\]\|^Note: \|^1 warning$\|^[0-9]* warnings$" || true
[ -n "$(find "$out/classes" -name '*.class' | head -1)" ] || { echo "javac produced nothing" >&2; exit 1; }
"$tools/d8" --release --min-api $min_sdk --lib "$android_jar" --output "$out/dex" \
    $(find "$out/classes" -name '*.class')

echo "== native libraries"
for lib in "$build/libmain.so" "$build/librexgpu-plume.so" "$runtime_libs/librexruntime.so" \
           "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"; do
    [ -f "$lib" ] || { echo "missing: $lib" >&2; exit 1; }
    cp "$lib" "$out/pack/lib/arm64-v8a/"
done
# The installer's native half, and libadrenotools' hooks (they must lie in
# nativeLibraryDir: it loads them from there to open a custom GPU driver).
for lib in "$build/libxerenge_installer.so" \
           "$build/adrenotools/src/hook/libhook_impl.so" \
           "$build/adrenotools/src/hook/libmain_hook.so" \
           "$build/adrenotools/src/hook/libfile_redirect_hook.so" \
           "$build/adrenotools/src/hook/libgsl_alloc_hook.so"; do
    [ -f "$lib" ] || { echo "missing: $lib" >&2; exit 1; }
    cp "$lib" "$out/pack/lib/arm64-v8a/"
done
[ -f "$runtime_libs/libTracyClient.so" ] && cp "$runtime_libs/libTracyClient.so" "$out/pack/lib/arm64-v8a/"
if [ -n "$VALIDATION_LAYER" ]; then
    cp "$VALIDATION_LAYER" "$out/pack/lib/arm64-v8a/libVkLayer_khronos_validation.so"
fi
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$out/pack/lib/arm64-v8a/"*.so

echo "== packing"
cp "$out/base.apk" "$out/unaligned.apk"
cp "$out/dex/classes.dex" "$out/pack/"
(cd "$out/pack" && zip -q -r "$out/unaligned.apk" classes.dex lib)
"$tools/zipalign" -f 4 "$out/unaligned.apk" "$out/aligned.apk"

# A debug key of our own, made once; the same key must sign every build or
# Android refuses the update.
keystore="$HOME/.android/xerenge-debug.keystore"
if [ ! -f "$keystore" ]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias xerenge -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Xerenge debug" > /dev/null
fi
"$tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --out "$build/burnout.apk" "$out/aligned.apk"
ls -la "$build/burnout.apk"
