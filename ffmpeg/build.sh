#!/usr/bin/env bash
# Run in WSL. Prerequisites: clang, lld, make, binutils-i686-linux-gnu, curl, xz-utils, python3; focal sysroot/.
# Shipped .V2 files are VP9/WebM, including 29 with Opus audio. Separate .A files use libmad.
# FFmpeg 4.2 i386 NASM and inline MMX contain absolute constant relocations even with PIC enabled.
# Use the portable C paths rather than permit TEXTREL; -z text enforces the constraint.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION=4.2.11
SHA256=d53f0ec8ebb8b03a9ca1efcb93f38ba3f1137b3dcc4ad3239809d511553c97f8
ARCHIVE="$ROOT/ffmpeg/ffmpeg-$VERSION.tar.xz"
SOURCE="$ROOT/ffmpeg/ffmpeg-$VERSION"
BUILD="$ROOT/ffmpeg/build"
OUTPUT="$ROOT/out/linux/ffmpeg"
SYSROOT="$ROOT/sysroot"
GCC="$SYSROOT/usr/lib/gcc/i686-linux-gnu/10"
mkdir -p "$BUILD" "$OUTPUT"
if [[ ! -f "$ARCHIVE" ]]; then
    curl -fL --retry 3 "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "$ARCHIVE"
fi
printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c -
if [[ ! -f "$SOURCE/configure" ]]; then
    tar -xJf "$ARCHIVE" -C "$ROOT/ffmpeg"
fi
CC="clang --target=i386-linux-gnu --sysroot=$SYSROOT -fuse-ld=lld --rtlib=libgcc --unwindlib=libgcc -B$GCC -L$GCC"
cd "$BUILD"
"$SOURCE/configure" \
    --target-os=linux --arch=x86_32 --cpu=i686 --enable-cross-compile \
    --cc="$CC" --host-cc=cc --ar=i686-linux-gnu-ar --nm=i686-linux-gnu-nm \
    --ranlib=i686-linux-gnu-ranlib --strip=i686-linux-gnu-strip \
    --disable-everything --disable-programs --disable-doc \
    --enable-shared --disable-static --disable-autodetect --enable-pic --disable-asm \
    --disable-avdevice --disable-avfilter --disable-postproc \
    --disable-network --disable-iconv --enable-pthreads \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    --enable-demuxer=matroska --enable-decoder=vp9,opus --enable-parser=vp9,opus \
    --enable-protocol=file \
    --extra-cflags=-fno-stack-protector \
    --extra-ldflags='-static-libgcc -Wl,-z,text' \
    --prefix="$OUTPUT" | tee "$OUTPUT/configure.log"
make -j"${JOBS:-4}" | tee "$OUTPUT/build.log"
# Do not install symlinks: the Windows relinker consumes ordinary files by SONAME.
for library in avcodec avformat avutil swscale swresample; do
    source "$BUILD/lib$library/lib$library.version"
    version_variable="lib${library}_VERSION"
    filename="lib$library.so.${!version_variable}"
    soname="${filename%.*.*}"
    real="$BUILD/lib$library/$soname"
    cp "$real" "$OUTPUT/$filename"
    cp "$real" "$OUTPUT/$soname"
    cp "$real" "$OUTPUT/lib$library.so"
    printf '%s (%s bytes; SONAME %s)\n' "$filename" "$(stat -c %s "$real")" "$soname"
done
python3 "$ROOT/ffmpeg/verify.py"
