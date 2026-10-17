#!/usr/bin/env python3
"""Generate i386 cdecl -> Win32 APIENTRY thunks for native OpenGL and GLU.

Windows SDK declarations are the signature authority, cross-checked against the
native x86 DLL exports. Missing public signatures are errors, never guessed
stack-cleanup thunks. GLU object/callback ownership lives in glu_callbacks.c;
game default-framebuffer state wrappers live in presentation.c.
The three GL 1.3 entry points are specified by Khronos and resolved only after
wglMakeCurrent.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


def split_parameters(name, text):
    """Split only outer commas; callback arguments can contain their own."""
    text = re.sub(r"\s+", " ", text).strip()
    if text in ("", "void"):
        return []
    parameters = []
    nesting = []
    start = 0
    for index, char in enumerate(text):
        if char in "([":
            nesting.append(char)
        elif char in ")]":
            if not nesting or nesting.pop() != {")": "(", "]": "["}[char]:
                raise SystemExit(f"Unbalanced parameter declaration: {name}: {text}")
        elif char == "," and not nesting:
            parameters.append(text[start:index].strip())
            start = index + 1
    if nesting:
        raise SystemExit(f"Unbalanced parameter declaration: {name}: {text}")
    parameters.append(text[start:].strip())
    return parameters


CALLBACK_PARAMETER = re.compile(
    r"\(\s*(?:(?:APIENTRY|CALLBACK|__cdecl|__stdcall)\s*)?"
    r"\*\s*([A-Za-z_]\w*)\s*\)"
)


def parameter_name(function, parameter):
    callback = CALLBACK_PARAMETER.search(parameter)
    if callback:
        return callback[1]
    argument = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^]]*\]\s*)*$", parameter)
    if not argument:
        raise SystemExit(f"Cannot extract parameter name: {function}: {parameter}")
    return argument[1]


def sdk_declarations(path):
    text = path.read_text(encoding="utf-8")
    # Comments contain sample prototypes; macro aliases are not DLL functions.
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    text = re.sub(r"^[ \t]*#.*$", "", text, flags=re.M)
    prototypes = {}
    for declaration in re.finditer(
        r"^[ \t]*(?:WINGDIAPI\s+)?"
        r"(?P<return>[A-Za-z_][\w\s*]*?)\s+APIENTRY\s+"
        r"(?P<name>gl[A-Za-z0-9_]+)\s*\((?P<params>[^;]*)\)\s*;",
        text,
        re.M,
    ):
        name = declaration["name"]
        signature = (
            re.sub(r"\s+", " ", declaration["return"]).strip(),
            split_parameters(name, declaration["params"]),
        )
        if name in prototypes and prototypes[name] != signature:
            raise SystemExit(f"Conflicting SDK declarations: {name}")
        prototypes[name] = signature
    return prototypes, text


def native_exports(tool, path):
    try:
        text = subprocess.check_output(
            [str(tool), "--coff-exports", str(path)], text=True
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Cannot inspect native exports from {path}: {error}") from error
    if not re.search(r"^Format: COFF-i386\s*$", text, re.M):
        raise SystemExit(f"Expected a native x86 PE DLL: {path}")
    names = set(re.findall(r"^[ \t]+Name:[ \t]+(\S+)[ \t]*$", text, re.M))
    if not names:
        raise SystemExit(f"No named native exports found: {path}")
    return names


def require_coverage(library, exports, prototypes):
    missing = exports - prototypes.keys()
    if missing:
        raise SystemExit(f"{library} exports missing SDK signatures: {sorted(missing)}")
    unavailable = prototypes.keys() - exports
    if unavailable:
        raise SystemExit(f"{library} SDK APIs missing native exports: {sorted(unavailable)}")


def glu_object_types(text):
    types = {"GLUtesselator", "GLUquadric", "GLUnurbs"}
    aliases = re.findall(
        r"\btypedef\s+(?:(?:struct|class)\s+)?([A-Za-z_]\w*)"
        r"\s+([A-Za-z_]\w*)\s*;",
        text,
    )
    while True:
        added = {alias for source, alias in aliases if source in types} - types
        if not added:
            return types
        types.update(added)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk_gl", type=Path, help="Windows SDK um/gl directory")
    system_dlls = Path(os.environ.get("SystemRoot", "C:/Windows")) / "SysWOW64"
    parser.add_argument("--opengl32", type=Path, default=system_dlls / "opengl32.dll")
    parser.add_argument("--glu32", type=Path, default=system_dlls / "glu32.dll")
    parser.add_argument(
        "--llvm-readobj",
        type=Path,
        default=shutil.which("llvm-readobj")
        or "C:/msys64/clangarm64/bin/llvm-readobj.exe",
        help="llvm-readobj executable used to cross-check native x86 DLL exports",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    gl, _ = sdk_declarations(args.sdk_gl / "GL.h")
    glu, glu_text = sdk_declarations(args.sdk_gl / "GLU.h")
    opengl_exports = native_exports(args.llvm_readobj, args.opengl32)
    glu_exports = native_exports(args.llvm_readobj, args.glu32)
    # glDebugEntry is a diagnostic export without an SDK GL declaration, not a
    # GL 1.1 API. WGL and Glmf exports are separate namespaces, not GL functions.
    internal_gl = {"glDebugEntry"}
    public_gl = {name for name in opengl_exports if name.startswith("gl")} - internal_gl
    require_coverage("opengl32", public_gl, gl)
    require_coverage("glu32", glu_exports, glu)
    prototypes = {**gl, **glu}
    extensions = {
        "glActiveTexture": ("void", ["GLenum texture"]),
        "glClientActiveTexture": ("void", ["GLenum texture"]),
        "glCompressedTexImage2D": (
            "void",
            split_parameters(
                "glCompressedTexImage2D",
                "GLenum target, GLint level, GLenum internalformat, GLsizei width, "
                "GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data",
            ),
        ),
    }
    custom = {
        "gluNewTess", "gluDeleteTess", "gluTessCallback",
        "gluNewQuadric", "gluDeleteQuadric", "gluQuadricCallback",
        "gluNewNurbsRenderer", "gluDeleteNurbsRenderer", "gluNurbsCallback",
        "glReadBuffer", "glDrawBuffer", "glGetIntegerv", "glGetFloatv",
        "glGetDoublev", "glGetBooleanv",
        "glPushAttrib", "glPopAttrib",
    }
    callbacks = {"gluTessCallback", "gluQuadricCallback", "gluNurbsCallback"}
    missing_custom = custom - prototypes.keys()
    if missing_custom:
        raise SystemExit(f"Custom GL/GLU APIs missing SDK signatures: {sorted(missing_custom)}")
    prototypes.update(extensions)
    names = sorted(prototypes)
    object_types = "|".join(sorted(glu_object_types(glu_text)))
    object_parameter = re.compile(
        rf"^((?:const\s+)?(?:{object_types})\s*\*)\s*([A-Za-z_]\w*)$"
    )
    object_token = re.compile(rf"\b(?:{object_types})\b")
    lines = [
        "/* Generated by gen_gl.py from Windows SDK declarations and native x86 exports. */",
        "#pragma once",
        "#include <GL/gl.h>",
        "#include <GL/glu.h>",
        '#include "glu_callbacks.h"',
        "",
    ]
    for name in names:
        ret, parameters = prototypes[name]
        guest_parameters = parameters
        if name in callbacks:
            if len(parameters) != 3 or not CALLBACK_PARAMETER.search(parameters[2]):
                raise SystemExit(f"Unexpected GLU callback registration signature: {name}")
            guest_parameters = parameters[:2] + [
                f"gfx_glu_callback {parameter_name(name, parameters[2])}"
            ]
        elif any(CALLBACK_PARAMETER.search(parameter) for parameter in parameters):
            raise SystemExit(f"Callback API needs a custom implementation: {name}")
        params = ", ".join(guest_parameters) or "void"
        lines.append(f"{ret} __cdecl lx_{name}({params});")
    lines += ["", "#define LX_GL_SYMBOLS(X) \\"]
    for index, name in enumerate(names):
        lines.append(f"    X({name})" + (" \\" if index != len(names) - 1 else ""))
    lines += ["", "#ifdef LX_GFX_IMPLEMENTATION"]
    for name in names:
        if name in custom:
            continue
        ret, parameters = prototypes[name]
        params = ", ".join(parameters) or "void"
        call_args = []
        objects = []
        for parameter in parameters:
            argument = parameter_name(name, parameter)
            obj = object_parameter.fullmatch(parameter) if name in glu else None
            if obj:
                objects.append(argument)
                call_args.append(f"({obj[1]})gfx_glu_native({argument})")
            else:
                if name in glu and object_token.search(parameter):
                    raise SystemExit(f"Unsupported GLU object parameter: {name}: {parameter}")
                call_args.append(argument)
        if len(objects) > 1:
            raise SystemExit(f"GLU API needs multiple object callback contexts: {name}")
        if name in extensions:
            lines += [
                f"typedef {ret} (APIENTRY *gfx_{name}_fn)({params});",
                f"static gfx_{name}_fn gfx_p_{name};",
            ]
        lines += [f"{ret} __cdecl lx_{name}({params})", "{"]
        if name in extensions:
            lines.append(f'    if (!gfx_p_{name}) gfx_missing_extension("{name}");')
        callee = f"gfx_p_{name}" if name in extensions else name
        call = f"{callee}({', '.join(call_args)})"
        if objects:
            lines.append(f"    void *gfx_previous = gfx_glu_enter({objects[0]});")
            if ret == "void":
                lines.append(f"    {call};")
            else:
                lines.append(f"    {ret} gfx_result = {call};")
            lines.append("    gfx_glu_leave(gfx_previous);")
            if ret != "void":
                lines.append("    return gfx_result;")
        else:
            lines.append(f"    {'return ' if ret != 'void' else ''}{call};")
        lines.append("}")
    lines += ["void gfx_load_extensions(void)", "{"]
    for name in extensions:
        lines += [
            f'    gfx_p_{name} = (gfx_{name}_fn)gfx_extension("{name}");',
            f'    if (!gfx_p_{name}) gfx_p_{name} = (gfx_{name}_fn)gfx_extension("{name}ARB");',
        ]
    lines += ["}", "#endif", ""]
    (root / "bridges/gfx/gl_generated.h").write_text(
        "\n".join(lines), encoding="utf-8", newline="\n"
    )
    print(
        f"Generated {len(names) - len(custom)} cdecl wrappers and "
        f"{len(custom)} custom entries "
        f"({len(gl)} GL, {len(glu)} GLU, {len(extensions)} WGL-resolved extensions)"
    )


if __name__ == "__main__":
    main()
