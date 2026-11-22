#!/usr/bin/env python3
"""Generate runtime/gl.c: every OpenGL ES import of the game (ES 1.1 fixed function, ES 2, and the OES extensions),
forwarding to the host's GL with game-memory pointers translated.

The game links libGLESv1_CM.so and libGLESv2.so; the 'glitch' engine creates an ES 2 driver first and falls back
to ES 1 only if that fails (docs/notes/gl.md). Both sets of shims are generated here; the host context decides which
ones work. The hand-written parts (texture decoders, glGetString, buffer mapping, compressed uploads) are in
runtime/gles1.c, which the generated file includes.

Argument codes (one 32-bit argument word each, softfp ABI):
  e GLenum   u GLuint   i GLint   z GLsizei   x GLbitfield   b GLboolean/GLubyte   n GLintptr/GLsizeiptr
  f GLfloat (passed as its bits)   p pointer into game memory   s string   S array of strings
  A client-array pointer (an offset when an array buffer is bound; glVertexAttribPointer and the ES 1 *Pointer calls)
  I index pointer (an offset when an element buffer is bound)
Return codes: v void, e/u/i/b integer.

Table line: name ret args [core=<ES2 name>] [first=core|own] [hand] [extra]
  core=  the host name to use as a fallback (or first, see below) for an OES name: the framebuffer-object and
         blend-equation OES calls exist only on ES 1 hosts, the core names on ES 2+ hosts; the 3D texture OES calls
         exist on ES 2 hosts with the extension and as core calls on ES 3 hosts
  first= which of the two to look up first (core: when the host is not an ES 1 context)
  hand   the import is written by hand in gles1.c (this file only declares the host entry point and registers it)
  extra  a host entry point the hand-written code needs that the game does not import (no shim)

  py -3 tools/recomp/gen_gl.py > runtime/gl.c
  py -3 tools/recomp/gen_gl.py --check librainbowsix.so      # every gl* import of the library has a shim
"""
import sys

GL = """
# ---- OpenGL ES 2 (shared names also exist in libGLESv1_CM and work on either kind of context) ----
glActiveTexture v e
glAttachShader v uu
glBindBuffer v eu hand
glBindFramebuffer v eu
glBindRenderbuffer v eu
glBindTexture v eu
glBlendColor v ffff
glBlendEquation v e
glBlendEquationSeparate v ee
glBlendFunc v ee
glBlendFuncSeparate v eeee
glBufferData v enpe
glBufferSubData v ennp
glCheckFramebufferStatus e e
glClear v x
glClearColor v ffff
glClearDepthf v f
glClearStencil v i
glColorMask v bbbb
glCompileShader v u
glCompressedTexImage2D v eiezzizp hand
glCompressedTexSubImage2D v eiiizzezp hand
glCopyTexSubImage2D v eiiiiizz
glCreateProgram u -
glCreateShader u e
glCullFace v e
glDeleteBuffers v zp hand
glDeleteFramebuffers v zp
glDeleteProgram v u
glDeleteRenderbuffers v zp
glDeleteShader v u
glDeleteTextures v zp
glDepthFunc v e
glDepthMask v b
glDepthRangef v ff
glDisable v e
glDisableVertexAttribArray v u
glDrawArrays v eiz
glDrawElements v ezeI
glEnable v e
glEnableVertexAttribArray v u
glFlush v -
glFramebufferRenderbuffer v eeeu
glFramebufferTexture2D v eeeui
glFrontFace v e
glGenBuffers v zp
glGenFramebuffers v zp
glGenRenderbuffers v zp
glGenTextures v zp
glGenerateMipmap v e
glGetActiveAttrib v uuzpppp
glGetActiveUniform v uuzpppp
glGetAttribLocation i us
glGetError e -
glGetFloatv v ep
glGetIntegerv v ep hand
glGetProgramInfoLog v uzpp
glGetProgramiv v uep
glGetShaderInfoLog v uzpp
glGetShaderSource v uzpp
glGetShaderiv v uep
glGetString s e hand
glGetUniformLocation i us
glLineWidth v f
glLinkProgram v u
glPixelStorei v ei
glPolygonOffset v ff
glReadPixels v iizzeep
glRenderbufferStorage v eezz
glSampleCoverage v fb
glScissor v iizz
glShaderSource v uzSp hand
glStencilFunc v eiu
glStencilMask v u
glStencilOp v eee
glTexImage2D v eiizzieep
glTexParameterf v eef
glTexParameteri v eei
glTexSubImage2D v eiiizzeep
glUniform1f v if
glUniform1fv v izp
glUniform1i v ii
glUniform1iv v izp
glUniform2fv v izp
glUniform2iv v izp
glUniform3fv v izp
glUniform3iv v izp
glUniform4fv v izp
glUniform4iv v izp
glUniformMatrix4fv v izbp
glUseProgram v u
glVertexAttrib4f v uffff
glVertexAttribPointer v uiebzA
glViewport v iizz

# ---- OpenGL ES 1.1 fixed function (libGLESv1_CM only; an ES 1 context) ----
glAlphaFunc v ef
glClientActiveTexture v e
glClipPlanef v ep
glColor4f v ffff
glColor4ub v bbbb
glColorPointer v iezA
glCopyTexImage2D v eieiizzi
glDisableClientState v e
glEnableClientState v e
glFogf v ef
glFogfv v ep
glHint v ee
glLightf v eef
glLightfv v eep
glLightModelf v ef
glLightModelfv v ep
glLoadIdentity v -
glLoadMatrixf v p
glMaterialf v eef
glMaterialfv v eep
glMatrixMode v e
glMultMatrixf v p
glMultiTexCoord4f v effff
glNormal3f v fff
glNormalPointer v ezA
glPointParameterf v ef
glPointParameterfv v ep
glPointSize v f
glPopMatrix v -
glPushMatrix v -
glShadeModel v e
glTexCoordPointer v iezA
glTexEnvf v eef
glTexEnvfv v eep
glTexEnvi v eei
glVertexPointer v iezA

# ---- OES extensions: framebuffer objects and blend equation (ES 1 names; core names on ES 2+ hosts) ----
glBlendEquationOES v e core=glBlendEquation first=core
glGenerateMipmapOES v e core=glGenerateMipmap first=core
glBindFramebufferOES v eu core=glBindFramebuffer first=core
glBindRenderbufferOES v eu core=glBindRenderbuffer first=core
glCheckFramebufferStatusOES e e core=glCheckFramebufferStatus first=core
glDeleteFramebuffersOES v zp core=glDeleteFramebuffers first=core
glDeleteRenderbuffersOES v zp core=glDeleteRenderbuffers first=core
glFramebufferRenderbufferOES v eeeu core=glFramebufferRenderbuffer first=core
glFramebufferTexture2DOES v eeeui core=glFramebufferTexture2D first=core
glGenFramebuffersOES v zp core=glGenFramebuffers first=core
glGenRenderbuffersOES v zp core=glGenRenderbuffers first=core
glRenderbufferStorageOES v eezz core=glRenderbufferStorage first=core

# ---- OES_texture_3D (extension name first; the ES 3 core call as fallback) ----
glTexImage3DOES v eiezzzieep core=glTexImage3D first=own
glTexSubImage3DOES v eiiiizzzeep core=glTexSubImage3D first=own
glCompressedTexImage3DOES v eiezzzizp core=glCompressedTexImage3D first=own
glCompressedTexSubImage3DOES v eiiiizzzezp core=glCompressedTexSubImage3D first=own

# ---- host entry points the hand-written code needs that the game does not import ----
glGetBufferParameteriv v eep extra
"""

# imports with a hand-written shim that are not host calls (the mapping is emulated with a shadow buffer)
HAND_ONLY = ["glMapBufferOES", "glUnmapBufferOES"]

CTYPE = {"e": "uint32_t", "u": "uint32_t", "i": "int32_t", "z": "int32_t", "x": "uint32_t", "b": "uint8_t",
         "n": "intptr_t", "f": "float", "p": "void *", "s": "const char *", "S": "const char *const *",
         "A": "const void *", "I": "const void *"}
RTYPE = {"v": "void", "e": "uint32_t", "u": "uint32_t", "i": "int32_t", "b": "uint8_t", "s": "const char *"}


def arg_expr(code, k):
    w = f"rt_arg(c, {k})"
    return {"e": w, "u": w, "x": w, "i": f"(int32_t){w}", "z": f"(int32_t){w}", "b": f"(uint8_t){w}",
            "n": f"(intptr_t)(int32_t){w}", "f": f"rt_farg(c, {k})", "p": f"G({w})", "s": f"(const char *)G({w})",
            "A": f"(array_buffer ? (const void *)(uintptr_t){w} : (const void *)G({w}))",
            "I": f"(element_buffer ? (const void *)(uintptr_t){w} : (const void *)G({w}))"}[code]


def parse():
    funcs = []
    for line in GL.splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        w = line.split()
        name, ret, args = w[0], w[1], ("" if w[2] == "-" else w[2])
        opts = {"core": None, "first": "own", "hand": False, "extra": False}
        for o in w[3:]:
            if o == "hand":
                opts["hand"] = True
            elif o == "extra":
                opts["extra"] = True
            elif o.startswith("core="):
                opts["core"] = o[5:]
            elif o.startswith("first="):
                opts["first"] = o[6:]
            else:
                sys.exit(f"bad option {o} for {name}")
        funcs.append((name, ret, args, opts))
    return funcs


def check(path):
    from elftools.elf.elffile import ELFFile
    with open(path, "rb") as f:
        dyn = ELFFile(f).get_section_by_name(".dynsym")
        imports = sorted({s.name for s in dyn.iter_symbols() if s["st_shndx"] == "SHN_UNDEF" and s.name.startswith("gl")})
    have = {n for n, _, _, o in parse() if not o["extra"]} | set(HAND_ONLY)
    missing = [n for n in imports if n not in have]
    print(f"{len(imports)} gl* imports, {len(have)} shims, imports without a shim: {missing or 'none'}", file=sys.stderr)
    return 1 if missing else 0


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2]))
    funcs = parse()
    names = [f[0] for f in funcs]
    if len(set(names)) != len(names):
        sys.exit("duplicate names in the table")
    out = ['/* Generated by tools/recomp/gen_gl.py: the OpenGL ES 1.1 / ES 2 / OES imports. Do not edit by hand.',
           ' * The hand-written parts are in gles1.c, included below. */',
           '#include <stdint.h>', '#include <stdio.h>', '#include <stdlib.h>', '#include <string.h>',
           '#if !defined(_WIN32)', '#include <dlfcn.h>', '#endif', '#include "rt.h"', '',
           '#define RT_GL_UNIT 1', '#if defined(_WIN32) && !defined(_WIN64)', '#define GLAPI __stdcall', '#else',
           '#define GLAPI', '#endif', '',
           '/* the buffers bound to GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER: while one is bound, a client-array',
           ' * pointer argument is an offset into it, not a game address (the game is single-context: one render thread) */',
           'static uint32_t array_buffer, element_buffer;', '']
    # function pointer slots
    for name, ret, args, o in funcs:
        params = ", ".join(CTYPE[a] for a in args) or "void"
        out.append(f"static {RTYPE[ret]} (GLAPI *p_{name})({params});")
    out.append("")
    out.append("enum {")
    for name, _, _, _ in funcs:
        out.append(f"    F_{name},")
    out.append("    F_COUNT")
    out.append("};")
    out += ["",
            "typedef struct { const char *name, *core; void **slot; unsigned char core_first; } glfn_t;",
            "static const glfn_t glfns[F_COUNT] = {"]
    for name, _, _, o in funcs:
        core = f'"{o["core"]}"' if o["core"] else "NULL"
        out.append(f'    {{"{name}", {core}, (void **)&p_{name}, {1 if o["first"] == "core" else 0}}},')
    out += ["};", ""]
    out.append(r'''/* ---- locating the host entry points ----
 * rt_gl_getproc (the platform layer: GetProcAddress on ANGLE's libGLESv2.dll then eglGetProcAddress, or dlsym on
 * libGLESv2 then eglGetProcAddress) comes first. On Android the ES 1 calls live in libGLESv1_CM.so, which the
 * platform layer does not open, so the lookup falls back to dlopen of both libraries. Entry points are resolved on
 * first use, when the context is current. */
void *(*rt_gl_getproc)(const char *name);

static void *gl_sym(const char *name) {
    void *p = rt_gl_getproc ? rt_gl_getproc(name) : NULL;
#if !defined(_WIN32)
    if (!p) {
        static void *lib[2];
        static const char *const libname[2] = {"libGLESv2.so", "libGLESv1_CM.so"};
        for (int k = 0; k < 2 && !p; k++) {
            if (!lib[k]) lib[k] = dlopen(libname[k], RTLD_NOW);
            if (lib[k]) p = dlsym(lib[k], name);
        }
    }
#endif
    return p;
}

/* is the current host context an ES 1.x one ("OpenGL ES-CM 1.1")? unknown (no context yet) counts as ES 2+ */
static int gl_es1_known = -1;
static int gl_host_is_es1(void) {
    if (gl_es1_known >= 0) return gl_es1_known;
    const char *(GLAPI *gs)(uint32_t) = (const char *(GLAPI *)(uint32_t))gl_sym("glGetString");
    const char *v = gs ? gs(0x1F02 /* GL_VERSION */) : NULL;
    if (!v) return 0;
    gl_es1_known = strstr(v, "ES-C") != NULL; /* "OpenGL ES-CM 1.1" / "OpenGL ES-CL 1.1" */
    return gl_es1_known;
}

static int gl_resolve(int idx) {
    const glfn_t *f = &glfns[idx];
    void *p = NULL;
    if (f->core && f->core_first && !gl_host_is_es1()) p = gl_sym(f->core);
    if (!p) p = gl_sym(f->name);
    if (!p && f->core) p = gl_sym(f->core);
    if (p) { *f->slot = p; return 1; }
    static unsigned char seen[F_COUNT];
    if (!seen[idx]) {
        seen[idx] = 1;
        rt_log("GL: %s is not available on this host (calls ignored)", f->name);
    }
    return 0;
}
#define HOST(name) (p_##name || gl_resolve(F_##name))
''')
    out += ['#include "gles1.c"', '']
    out.append(r'''/* The platform layer calls this after it has created a new EGL context (Android destroys the context with the
 * surface; the game then re-creates its GL objects): the host entry points are looked up again, and the tracked
 * buffer bindings and any buffer mapping are forgotten. */
void rt_gl_context_changed(void) {
    for (int k = 0; k < F_COUNT; k++) *glfns[k].slot = NULL;
    gl_es1_known = -1;
    array_buffer = element_buffer = 0;
    for (int k = 0; k < 2; k++) {
        if (gl_map[k].addr) rt_free(gl_map[k].addr);
        gl_map[k].addr = gl_map[k].size = 0;
    }
}
''')
    # generated shims
    for name, ret, args, o in funcs:
        if o["hand"] or o["extra"]:
            continue
        call_args = ", ".join(arg_expr(a, k) for k, a in enumerate(args))
        guard = f"if (!HOST({name})) {{ {'rt_ret(c, 0); ' if ret != 'v' else ''}return; }} "
        body = f"p_{name}({call_args});" if ret == "v" else f"rt_ret(c, (uint32_t)p_{name}({call_args}));"
        out.append(f"static void imp_{name}(cpu_t *c) {{ {guard}{body} }}")
    out += ["", "void rt_gl_register(void) {"]
    for name, ret, args, o in funcs:
        if o["extra"]:
            continue
        out.append(f'    rt_import_register("{name}", (rt_import_fn)imp_{name});')
    for name in HAND_ONLY:
        out.append(f'    rt_import_register("{name}", (rt_import_fn)imp_{name});')
    out.append("}")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
