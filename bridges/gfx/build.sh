#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/env.sh
mkdir -p out/win
python bridges/gfx/gen_gl.py "$WINSDK_DIR/Include/$WINSDK_VER/um/gl"
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror -shared bridges/gfx/gfx.c bridges/gfx/presentation.c bridges/gfx/input.c bridges/gfx/glu_callbacks.c \
  -o out/win/lxgfx.dll "${WIN_LDFLAGS[@]}" -luser32 -lgdi32 -lopengl32 -lglu32
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror bridges/gfx/test_gfx.c \
  -o out/test_gfx.exe "${WIN_LDFLAGS[@]}" -luser32 -lgdi32 -lopengl32
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror bridges/gfx/test_display.c \
  -o out/test_display.exe "${WIN_LDFLAGS[@]}" -luser32 -lgdi32 -lopengl32
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror bridges/gfx/wgltest.c \
  -o out/wgltest.exe "${WIN_LDFLAGS[@]}" -luser32 -lgdi32 -lopengl32
"${WIN_CC[@]}" -std=c11 -Wall -Wextra -Werror bridges/gfx/test_glu.c \
  -o out/test_glu.exe "${WIN_LDFLAGS[@]}" -lkernel32
