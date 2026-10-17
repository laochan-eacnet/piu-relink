# Run build scripts with MSYS2 bash (C:/msys64/usr/bin/bash.exe); plain `bash` on PATH is WSL.
export MSYS2_ARG_CONV_EXCL='*'   # keep -Wl,/debug etc. away from MSYS path conversion
case "$(uname -s)" in MSYS*|MINGW*) export PATH="/usr/bin:$PATH" ;; esac  # not C:/Windows/system32/sort.exe
# Source from the Windows-side shell: . tools/env.sh
# Defines compiler flag sets for the two targets used by this repo.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSVC_DIR="${MSVC_DIR:-$(ls -d "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/"* | sort | tail -1)}"
WINSDK_DIR="${WINSDK_DIR:-C:/Program Files (x86)/Windows Kits/10}"
WINSDK_VER="${WINSDK_VER:-10.0.26100.0}"

# MSVC/SDK headers must come *after* clang's resource headers (clang's vadefs.h overrides
# MSVC's address-of-parameter va_start), so pass them via INCLUDE, never -isystem.
export INCLUDE="$MSVC_DIR/include;$WINSDK_DIR/Include/$WINSDK_VER/ucrt;$WINSDK_DIR/Include/$WINSDK_VER/um;$WINSDK_DIR/Include/$WINSDK_VER/shared"
export LIB="$MSVC_DIR/lib/x86;$WINSDK_DIR/Lib/$WINSDK_VER/ucrt/x86;$WINSDK_DIR/Lib/$WINSDK_VER/um/x86"

# Native Win32 (lxrt, bridges): i686 MSVC ABI, 16-byte stack realignment for guest callbacks.
WIN_CC=(clang --target=i686-pc-windows-msvc -fuse-ld=lld --rtlib=platform -Wno-unused-command-line-argument
  -mstackrealign -mstack-alignment=16 -fno-stack-protector -O2 -g -gcodeview
  -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
WIN_LDFLAGS=(-Wl,/debug)

# Guest i386 Linux (test programs, rebuilt libraries).
SYSROOT="$ROOT/sysroot"
GCCVER=10
LNX_CC=(clang --target=i386-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld
  --rtlib=libgcc --unwindlib=libgcc
  -B "$SYSROOT/usr/lib/gcc/i686-linux-gnu/$GCCVER"
  -L "$SYSROOT/usr/lib/gcc/i686-linux-gnu/$GCCVER")
LNX_CXX=("${LNX_CC[@]}" --driver-mode=g++ -stdlib=libstdc++)
