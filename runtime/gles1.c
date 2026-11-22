/* The hand-written part of the GL layer (the generated runtime/gl.c includes this file; compiled on its own it
 * defines nothing, so it can be listed in a build without harm).
 *
 * What the generated shims cannot do by themselves:
 *
 *  - glGetString. The game's ES 2 driver (glitch COpenGLES2Driver) reads GL_VERSION, GL_SHADING_LANGUAGE_VERSION and
 *    GL_EXTENSIONS and decides everything from them (docs/notes/gl.md). The host's strings are passed through with
 *    three changes: on an ES 2+ host the version is "OpenGL ES 2.0" and the GLSL version "OpenGL ES GLSL ES 1.00"
 *    (what a 2013 phone said; the driver's GLSL version parser breaks on "ES 3.00 (ANGLE ...)" style strings and the
 *    game's shaders are GLSL ES 1.00), and the extension list gains the formats and calls emulated here:
 *      GL_AMD_compressed_ATC_texture  the game's textures are ATC (DDS files): decoded to RGBA8 when the host has no ATC
 *      GL_IMG_texture_compression_pvrtc  the six 2D "mipmap_*.pvr" files are PVRTC 2bpp: same
 *      GL_OES_mapbuffer               glMapBufferOES hands out a shadow block in game memory
 *      GL_OES_texture_3D              when the host can do 3D textures (ES 3 core)
 *    The returned string lives in game memory (the game keeps the pointer).
 *  - glCompressedTexImage2D / glCompressedTexSubImage2D: ATC and PVRTC levels are decoded in software and uploaded
 *    with glTexImage2D/glTexSubImage2D when the host does not list the extension; ETC1 goes to the host's ETC2 format
 *    when the host has no ETC1 (an ETC1 block is a valid ETC2 block); everything else passes through.
 *    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS / GL_COMPRESSED_TEXTURE_FORMATS) lists the emulated formats too.
 *  - glMapBufferOES / glUnmapBufferOES: a shadow block in game memory, uploaded with glBufferSubData on unmap (a
 *    host mapping would be a host pointer the 32-bit game cannot use).
 *  - the buffer bindings: while a buffer is bound to GL_ARRAY_BUFFER (GL_ELEMENT_ARRAY_BUFFER), the pointer of
 *    glVertexAttribPointer and the ES 1 *Pointer calls (the indices of glDrawElements) is an offset, otherwise a game
 *    address. Without a buffer a client array stays valid until the draw call because game memory is one fixed host
 *    mapping: the pointer given to GL is rt_mem + address.
 *  - glShaderSource: the strings are an array of game addresses.
 *
 * The decoders at the top are pure functions (no runtime dependencies); see build/gl/decode_test.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ATC (GL_AMD_compressed_ATC_texture) ---------------------------------------------------------------------------
 * 4x4 blocks. ATC_RGB (0x8C92): 8 bytes. ATC_RGBA_EXPLICIT (0x8C93): 8 bytes of 4-bit alpha (pixel 0 in the low
 * nibble, row by row), then the 8-byte colour block. ATC_RGBA_INTERPOLATED (0x87EE): the alpha block is DXT5's.
 * Colour block: colour0 (16 bits), colour1 (16 bits), 16 2-bit indices (pixel 0 in the low bits, row by row).
 * Colour0 is 5:5:5 plus a mode flag in bit 15, colour1 is 5:6:5. Mode 0: palette {c0, (5c0+3c1)/8, (3c0+5c1)/8, c1}.
 * Mode 1: {black, c1 - c0/4, c0, c1}. (This is DXT1's layout with a different palette; the explicit/interpolated
 * alpha blocks are DXT3's and DXT5's.) */
#define GL_ATC_RGB_AMD 0x8C92
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD 0x8C93
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD 0x87EE

static int is_atc(uint32_t f) { return f == GL_ATC_RGB_AMD || f == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD || f == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD; }

static void atc_palette(const uint8_t *b, uint8_t pal[4][3]) {
    uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    int r0 = (int)(c0 >> 10) & 31, g0 = (int)(c0 >> 5) & 31, b0 = (int)c0 & 31;
    int r1 = (int)(c1 >> 11) & 31, g1 = (int)(c1 >> 5) & 63, b1 = (int)c1 & 31;
    int a[3] = {(r0 << 3) | (r0 >> 2), (g0 << 3) | (g0 >> 2), (b0 << 3) | (b0 >> 2)};
    int d[3] = {(r1 << 3) | (r1 >> 2), (g1 << 2) | (g1 >> 4), (b1 << 3) | (b1 >> 2)};
    for (int k = 0; k < 3; k++) {
        if (c0 & 0x8000) {
            int v = d[k] - (a[k] >> 2);
            pal[0][k] = 0;
            pal[1][k] = (uint8_t)(v < 0 ? 0 : v);
            pal[2][k] = (uint8_t)a[k];
            pal[3][k] = (uint8_t)d[k];
        } else {
            pal[0][k] = (uint8_t)a[k];
            pal[1][k] = (uint8_t)((5 * a[k] + 3 * d[k]) >> 3);
            pal[2][k] = (uint8_t)((3 * a[k] + 5 * d[k]) >> 3);
            pal[3][k] = (uint8_t)d[k];
        }
    }
}

/* decode a w x h level into out (w*h RGBA8); 0 if the data is too short */
static int atc_decode(const uint8_t *src, uint32_t size, int w, int h, uint32_t format, uint8_t *out) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4, bs = format == GL_ATC_RGB_AMD ? 8 : 16;
    if (!src || w <= 0 || h <= 0 || (uint64_t)size < (uint64_t)bw * (uint64_t)bh * (uint64_t)bs) return 0;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)bs;
            const uint8_t *cb = bs == 8 ? blk : blk + 8;
            uint8_t pal[4][3], alpha[16];
            atc_palette(cb, pal);
            uint32_t idx = (uint32_t)cb[4] | (uint32_t)cb[5] << 8 | (uint32_t)cb[6] << 16 | (uint32_t)cb[7] << 24;
            if (format == GL_ATC_RGB_AMD) {
                for (int k = 0; k < 16; k++) alpha[k] = 255;
            } else if (format == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD) {
                for (int k = 0; k < 16; k++) {
                    int v = (blk[k >> 1] >> (4 * (k & 1))) & 15;
                    alpha[k] = (uint8_t)(v * 17);
                }
            } else {
                int a0 = blk[0], a1 = blk[1];
                int tab[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                if (a0 > a1) {
                    for (int k = 1; k < 7; k++) tab[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                } else {
                    for (int k = 1; k < 5; k++) tab[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                    tab[6] = 0; tab[7] = 255;
                }
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++) bits |= (uint64_t)blk[2 + k] << (8 * k);
                for (int k = 0; k < 16; k++) alpha[k] = (uint8_t)tab[(bits >> (3 * k)) & 7];
            }
            for (int y = 0; y < 4; y++) {
                int py = by * 4 + y;
                if (py >= h) break;
                for (int x = 0; x < 4; x++) {
                    int px = bx * 4 + x;
                    if (px >= w) break;
                    int k = y * 4 + x, i = (int)((idx >> (2 * k)) & 3);
                    uint8_t *o = out + 4 * ((size_t)py * (size_t)w + (size_t)px);
                    o[0] = pal[i][0]; o[1] = pal[i][1]; o[2] = pal[i][2]; o[3] = alpha[k];
                }
            }
        }
    return 1;
}

/* ---- PVRTC (GL_IMG_texture_compression_pvrtc) -----------------------------------------------------------------------
 * Format (as in the PowerVR SDK's PVRTDecompress): 64-bit blocks of 4x4 (4bpp) or 8x4 (2bpp) pixels, at least 2x2
 * blocks, stored in Morton order (y in the low bit; for non-square sizes the larger axis keeps its extra high bits
 * on top). Block = modulation word (bytes 0-3), then colour word (bytes 4-7). Colour word: bit 0 mode; bits 1-15
 * colour A (bit 15 set: RGB 5-5-4, else ARGB 3-4-4-3); bits 16-31 colour B (bit 31 set: RGB 5-5-5, else ARGB
 * 3-4-4-4). Colours A and B are upscaled bilinearly between block centres (wrapping at the edges), then mixed per
 * pixel by the modulation weight (out of 8):
 *   4bpp: 2 bits per pixel, weights {0,3,5,8}; mode 1 = punch-through {0,4,4 with alpha 0,8}.
 *   2bpp: mode 0: 1 bit per pixel, weights {0,8}; mode 1: 2-bit weights on a checkerboard, the other pixels
 *         averaged from their neighbours (modulation bit 0 / bit 20 choose H+V, H only or V only).
 * Adapted from NOVAHD_sc/runtime/gles1.c (the same decoder). */
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG 0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG 0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03

typedef struct { int r, g, b, a; } pvr_col; /* r, g, b 5 bits; a 4 bits */

static pvr_col pvr_colour_a(uint32_t w) {
    pvr_col k;
    if (w & 0x8000) {
        k.r = (int)((w & 0x7c00) >> 10);
        k.g = (int)((w & 0x3e0) >> 5);
        k.b = (int)(w & 0x1e) | (int)((w & 0x1e) >> 4);
        k.a = 0xf;
    } else {
        k.r = (int)((w & 0xf00) >> 7) | (int)((w & 0xf00) >> 11);
        k.g = (int)((w & 0xf0) >> 3) | (int)((w & 0xf0) >> 7);
        k.b = (int)((w & 0xe) << 1) | (int)((w & 0xe) >> 2);
        k.a = (int)((w & 0x7000) >> 11);
    }
    return k;
}
static pvr_col pvr_colour_b(uint32_t w) {
    pvr_col k;
    if (w & 0x80000000u) {
        k.r = (int)((w & 0x7c000000) >> 26);
        k.g = (int)((w & 0x3e00000) >> 21);
        k.b = (int)((w & 0x1f0000) >> 16);
        k.a = 0xf;
    } else {
        k.r = (int)((w & 0xf000000) >> 23) | (int)((w & 0xf000000) >> 27);
        k.g = (int)((w & 0xf00000) >> 19) | (int)((w & 0xf00000) >> 23);
        k.b = (int)((w & 0xf0000) >> 15) | (int)((w & 0xf0000) >> 19);
        k.a = (int)((w & 0x70000000) >> 27);
    }
    return k;
}

static uint32_t pvr_twiddle(uint32_t xs, uint32_t ys, uint32_t x, uint32_t y) {
    uint32_t minax = xs < ys ? xs : ys, out = 0, bit = 1, dst = 1;
    int shift = 0;
    while (bit < minax) {
        if (y & bit) out |= dst;
        if (x & bit) out |= dst << 1;
        bit <<= 1; dst <<= 2; shift++;
    }
    out |= ((xs > ys ? x : y) >> shift) << (2 * shift);
    return out;
}

typedef struct {
    const uint8_t *data;
    int nbx, nby, bw; /* blocks across/down, block width in pixels (height is 4) */
    int bpp4;
} pvr_img;

static void pvr_block(const pvr_img *im, int bx, int by, uint32_t *mod, uint32_t *col) {
    uint32_t x = (uint32_t)((bx % im->nbx + im->nbx) % im->nbx);
    uint32_t y = (uint32_t)((by % im->nby + im->nby) % im->nby);
    const uint8_t *p = im->data + 8u * pvr_twiddle((uint32_t)im->nbx, (uint32_t)im->nby, x, y);
    *mod = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    *col = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
}

static const int pvr_rep[4] = {0, 3, 5, 8};

/* 2bpp: the weight stored for a pixel (wrapping); asked only for pixels that store one */
static int pvr_stored2(const pvr_img *im, int px, int py) {
    int W = im->nbx * 8, H = im->nby * 4;
    px = (px % W + W) % W; py = (py % H + H) % H;
    uint32_t mod, col;
    pvr_block(im, px / 8, py / 4, &mod, &col);
    int x = px % 8, y = py % 4;
    if (!(col & 1)) return (mod >> (y * 8 + x)) & 1 ? 8 : 0;
    /* bits 0 and 20 are flags: those pixels repeat their other bit */
    if (mod & 1) { if (mod & (1u << 21)) mod |= 1u << 20; else mod &= ~(1u << 20); }
    if (mod & 2) mod |= 1; else mod &= ~1u;
    return pvr_rep[(mod >> (2 * ((y * 8 + x) / 2))) & 3];
}

/* modulation weight 0..8 of a pixel; *punch set for a punch-through (alpha 0) pixel */
static int pvr_weight(const pvr_img *im, int px, int py, int *punch) {
    uint32_t mod, col;
    int bw = im->bw;
    pvr_block(im, px / bw, py / 4, &mod, &col);
    int x = px % bw, y = py % 4;
    *punch = 0;
    if (im->bpp4) {
        int v = (int)((mod >> (2 * (y * 4 + x))) & 3);
        if (!(col & 1)) return pvr_rep[v];
        if (v == 2) *punch = 1;
        return v == 0 ? 0 : v == 3 ? 8 : 4;
    }
    if (!(col & 1)) return (mod >> (y * 8 + x)) & 1 ? 8 : 0;
    if (((x ^ y) & 1) == 0) return pvr_stored2(im, px, py);
    if (!(mod & 1)) /* horizontal and vertical */
        return (pvr_stored2(im, px, py - 1) + pvr_stored2(im, px, py + 1) + pvr_stored2(im, px - 1, py) +
                pvr_stored2(im, px + 1, py) + 2) / 4;
    if (mod & (1u << 20)) /* vertical only */
        return (pvr_stored2(im, px, py - 1) + pvr_stored2(im, px, py + 1) + 1) / 2;
    return (pvr_stored2(im, px - 1, py) + pvr_stored2(im, px + 1, py) + 1) / 2; /* horizontal only */
}

static int pvr_floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static int is_pvrtc(uint32_t f) { return f >= GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG && f <= GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG; }

/* decode a w x h level into out (w*h RGBA8); 0 if the data is too short */
static int pvr_decode(const uint8_t *data, uint32_t size, int w, int h, uint32_t format, uint8_t *out) {
    pvr_img im;
    im.data = data;
    im.bpp4 = format == GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG || format == GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
    im.bw = im.bpp4 ? 4 : 8;
    im.nbx = (w + im.bw - 1) / im.bw; if (im.nbx < 2) im.nbx = 2;
    im.nby = (h + 3) / 4; if (im.nby < 2) im.nby = 2;
    if (!data || size < (uint32_t)im.nbx * (uint32_t)im.nby * 8u) return 0;
    int opaque = format == GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG || format == GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG;
    int bw = im.bw, bh = 4;
    for (int py = 0; py < h; py++) {
        int fy = py - bh / 2, by = pvr_floordiv(fy, bh), wy = fy - by * bh;
        for (int px = 0; px < w; px++) {
            int fx = px - bw / 2, bx = pvr_floordiv(fx, bw), wx = fx - bx * bw;
            uint32_t m, c[4];
            pvr_block(&im, bx, by, &m, &c[0]);         /* P (top left) */
            pvr_block(&im, bx + 1, by, &m, &c[1]);     /* Q (top right) */
            pvr_block(&im, bx, by + 1, &m, &c[2]);     /* R (bottom left) */
            pvr_block(&im, bx + 1, by + 1, &m, &c[3]); /* S (bottom right) */
            int f[4] = {(bw - wx) * (bh - wy), wx * (bh - wy), (bw - wx) * wy, wx * wy};
            int ca[4] = {0, 0, 0, 0}, cb[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                pvr_col a = pvr_colour_a(c[k]), b = pvr_colour_b(c[k]);
                ca[0] += a.r * f[k]; ca[1] += a.g * f[k]; ca[2] += a.b * f[k]; ca[3] += a.a * f[k];
                cb[0] += b.r * f[k]; cb[1] += b.g * f[k]; cb[2] += b.b * f[k]; cb[3] += b.a * f[k];
            }
            /* to 8 bits: the sums are 5-bit (alpha 4-bit) values times bw*bh (16 or 32) */
            for (int k = 0; k < 4; k++) {
                if (im.bpp4) {
                    ca[k] = k < 3 ? (ca[k] >> 6) + (ca[k] >> 1) : (ca[k] >> 4) + ca[k];
                    cb[k] = k < 3 ? (cb[k] >> 6) + (cb[k] >> 1) : (cb[k] >> 4) + cb[k];
                } else {
                    ca[k] = k < 3 ? (ca[k] >> 7) + (ca[k] >> 2) : (ca[k] >> 5) + (ca[k] >> 1);
                    cb[k] = k < 3 ? (cb[k] >> 7) + (cb[k] >> 2) : (cb[k] >> 5) + (cb[k] >> 1);
                }
            }
            int punch, wt = pvr_weight(&im, px, py, &punch);
            uint8_t *o = out + 4 * ((size_t)py * (size_t)w + (size_t)px);
            for (int k = 0; k < 4; k++) o[k] = (uint8_t)((ca[k] * (8 - wt) + cb[k] * wt) / 8);
            if (punch) o[3] = 0;
            if (opaque) o[3] = 255;
        }
    }
    return 1;
}

/* does a space-separated extension list contain that name? */
static int gl_list_has(const char *list, const char *name) {
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)) != NULL; p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return 1;
    return 0;
}

#ifdef RT_GL_UNIT
/* ---- the shims ------------------------------------------------------------------------------------------------------ */
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_BUFFER_SIZE 0x8764
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3
#define GL_ETC1_RGB8_OES 0x8D64
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

static const char *host_string(uint32_t name) { return HOST(glGetString) ? p_glGetString(name) : NULL; }

/* the host's GL_EXTENSIONS contains `ext`: 1 yes, 0 no, -1 cannot tell (no current context) */
static int host_ext(const char *ext) {
    const char *s = host_string(GL_EXTENSIONS);
    return s ? gl_list_has(s, ext) : -1;
}

static int host_major(void) {
    const char *v = host_string(GL_VERSION);
    if (!v) return 0;
    if (strstr(v, "ES-C")) return 1;
    while (*v && (*v < '0' || *v > '9')) v++;
    return *v ? atoi(v) : 0;
}

/* does the host need our decoder for this compressed format? (re-asked until a context is current) */
static int need_decode(uint32_t fmt) {
    static int atc = -1, pvr = -1;
    if (is_atc(fmt)) {
        if (atc < 0) {
            atc = host_ext("GL_AMD_compressed_ATC_texture");
            if (atc == 0) rt_log("gl: host GL has no ATC textures; decoding ATC in software");
        }
        return atc == 0;
    }
    if (is_pvrtc(fmt)) {
        if (pvr < 0) {
            pvr = host_ext("GL_IMG_texture_compression_pvrtc");
            if (pvr == 0) rt_log("gl: host GL has no PVRTC; decoding PVRTC in software");
        }
        return pvr == 0;
    }
    return 0;
}

/* ---- glGetString ---- */
static void add_ext(char *buf, size_t cap, const char *ext) {
    if (gl_list_has(buf, ext)) return;
    size_t len = strlen(buf);
    if (len + strlen(ext) + 2 > cap) return;
    if (len && buf[len - 1] != ' ') buf[len++] = ' ';
    strcpy(buf + len, ext);
}

static void imp_glGetString(cpu_t *c) {
    static struct { uint32_t name, addr; } cache[8];
    uint32_t name = rt_arg(c, 0);
    for (int k = 0; k < 8; k++)
        if (cache[k].addr && cache[k].name == name) { rt_ret(c, cache[k].addr); return; }
    const char *s = host_string(name);
    if (!s) { rt_ret(c, 0); return; } /* no context yet: the game checks for NULL; not cached */
    int major = host_major();
    uint32_t a;
    if (name == GL_VERSION && major >= 2) {
        a = rt_static_string("OpenGL ES 2.0");
    } else if (name == GL_SHADING_LANGUAGE_VERSION && major >= 2) {
        a = rt_static_string("OpenGL ES GLSL ES 1.00");
    } else if (name == GL_EXTENSIONS) {
        size_t cap = strlen(s) + 1024;
        char *buf = malloc(cap);
        if (!buf) { rt_ret(c, 0); return; }
        strcpy(buf, s);
        add_ext(buf, cap, "GL_AMD_compressed_ATC_texture");
        add_ext(buf, cap, "GL_IMG_texture_compression_pvrtc");
        add_ext(buf, cap, "GL_OES_mapbuffer");
        if (major >= 3 || gl_list_has(s, "GL_OES_texture_3D")) add_ext(buf, cap, "GL_OES_texture_3D");
        if (major >= 3) { /* core in ES 3 */
            static const char *const core3[] = {"GL_OES_depth24", "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8",
                                                "GL_OES_element_index_uint", "GL_OES_texture_npot"};
            for (size_t k = 0; k < sizeof core3 / sizeof core3[0]; k++) add_ext(buf, cap, core3[k]);
        }
        a = rt_static_string(buf);
        free(buf);
    } else {
        a = rt_static_string(s);
    }
    if (rt_verbose) rt_log("gl: glGetString(0x%x) -> %s", name, name == GL_EXTENSIONS ? "(extension list)" : s);
    for (int k = 0; k < 8; k++)
        if (!cache[k].addr) { cache[k].name = name; cache[k].addr = a; break; }
    rt_ret(c, a);
}

/* ---- buffers ---- */
static void imp_glBindBuffer(cpu_t *c) {
    uint32_t target = rt_arg(c, 0), buf = rt_arg(c, 1);
    if (target == GL_ARRAY_BUFFER) array_buffer = buf;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) element_buffer = buf;
    if (HOST(glBindBuffer)) p_glBindBuffer(target, buf);
}

static void imp_glDeleteBuffers(cpu_t *c) {
    int32_t n = (int32_t)rt_arg(c, 0);
    uint32_t names = rt_arg(c, 1);
    for (int32_t k = 0; names && k < n; k++) { /* deleting the bound buffer unbinds it */
        uint32_t id = rt_ld32(names + 4 * (uint32_t)k);
        if (id == array_buffer) array_buffer = 0;
        if (id == element_buffer) element_buffer = 0;
    }
    if (HOST(glDeleteBuffers)) p_glDeleteBuffers(n, G(names));
}

/* glMapBufferOES: a shadow block in game memory, uploaded by glUnmapBufferOES (write-only mapping; one per target) */
static struct { uint32_t addr, size; } gl_map[2];

static void imp_glMapBufferOES(cpu_t *c) {
    uint32_t target = rt_arg(c, 0);
    int k = target == GL_ELEMENT_ARRAY_BUFFER;
    if (gl_map[k].addr) { rt_ret(c, gl_map[k].addr); return; }
    int32_t size = 0;
    if (HOST(glGetBufferParameteriv)) p_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    if (size <= 0) { rt_ret(c, 0); return; }
    gl_map[k].addr = rt_calloc(1, (uint32_t)size);
    gl_map[k].size = gl_map[k].addr ? (uint32_t)size : 0;
    rt_ret(c, gl_map[k].addr);
}

static void imp_glUnmapBufferOES(cpu_t *c) {
    uint32_t target = rt_arg(c, 0);
    int k = target == GL_ELEMENT_ARRAY_BUFFER;
    if (!gl_map[k].addr) { rt_ret(c, 0); return; }
    if (HOST(glBufferSubData)) p_glBufferSubData(target, 0, (intptr_t)gl_map[k].size, G(gl_map[k].addr));
    rt_free(gl_map[k].addr);
    gl_map[k].addr = gl_map[k].size = 0;
    rt_ret(c, 1);
}

/* ---- glShaderSource: an array of game addresses of strings ---- */
static void imp_glShaderSource(cpu_t *c) {
    int32_t n = (int32_t)rt_arg(c, 1);
    uint32_t strings = rt_arg(c, 2), lengths = rt_arg(c, 3);
    if (!HOST(glShaderSource)) return;
    const char **v = malloc(sizeof(char *) * (size_t)(n > 0 ? n : 1));
    if (!v) return;
    for (int32_t k = 0; k < n; k++) v[k] = (const char *)G(rt_ld32(strings + 4 * (uint32_t)k));
    p_glShaderSource(rt_arg(c, 0), n, v, G(lengths));
    free(v);
}

/* ---- compressed textures ---- */
static void gl_compressed_2d(cpu_t *c, int sub) {
    uint32_t target = rt_arg(c, 0), fmt, size, data;
    int32_t level = (int32_t)rt_arg(c, 1), xo = 0, yo = 0, w, h, border = 0;
    if (sub) {
        xo = (int32_t)rt_arg(c, 2); yo = (int32_t)rt_arg(c, 3); w = (int32_t)rt_arg(c, 4); h = (int32_t)rt_arg(c, 5);
        fmt = rt_arg(c, 6); size = rt_arg(c, 7); data = rt_arg(c, 8);
    } else {
        fmt = rt_arg(c, 2); w = (int32_t)rt_arg(c, 3); h = (int32_t)rt_arg(c, 4); border = (int32_t)rt_arg(c, 5);
        size = rt_arg(c, 6); data = rt_arg(c, 7);
    }
    if (need_decode(fmt)) {
        if (!HOST(glTexImage2D) || !HOST(glTexSubImage2D)) return;
        uint8_t *rgba = NULL;
        if (data && w > 0 && h > 0) {
            rgba = malloc((size_t)w * (size_t)h * 4);
            int ok = rgba && (is_atc(fmt) ? atc_decode((const uint8_t *)G(data), size, w, h, fmt, rgba)
                                          : pvr_decode((const uint8_t *)G(data), size, w, h, fmt, rgba));
            if (!ok) {
                static int warned;
                if (!warned++) rt_log("gl: compressed level %d (%dx%d, format 0x%x) has only %u bytes (ignored)", level, w, h, fmt, size);
                free(rgba);
                return;
            }
        }
        if (sub) p_glTexSubImage2D(target, level, xo, yo, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        else p_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        free(rgba);
        return;
    }
    if (fmt == GL_ETC1_RGB8_OES) {
        static int etc2 = -1;
        if (etc2 < 0) etc2 = host_ext("GL_OES_compressed_ETC1_RGB8_texture") == 0 && host_ext("GL_OES_compressed_ETC2_RGB8_texture") == 1;
        if (etc2 > 0) fmt = GL_COMPRESSED_RGB8_ETC2;
    }
    if (sub) {
        if (HOST(glCompressedTexSubImage2D)) p_glCompressedTexSubImage2D(target, level, xo, yo, w, h, fmt, (int32_t)size, G(data));
    } else if (HOST(glCompressedTexImage2D)) {
        p_glCompressedTexImage2D(target, level, fmt, w, h, border, (int32_t)size, G(data));
    }
}
static void imp_glCompressedTexImage2D(cpu_t *c) { gl_compressed_2d(c, 0); }
static void imp_glCompressedTexSubImage2D(cpu_t *c) { gl_compressed_2d(c, 1); }

/* GL_NUM_COMPRESSED_TEXTURE_FORMATS / GL_COMPRESSED_TEXTURE_FORMATS also list the formats decoded here */
static int emulated_formats(int32_t *out) {
    int n = 0;
    static const int32_t atc[] = {GL_ATC_RGB_AMD, GL_ATC_RGBA_EXPLICIT_ALPHA_AMD, GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD};
    static const int32_t pvr[] = {GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG,
                                  GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG};
    if (need_decode(GL_ATC_RGB_AMD)) for (int k = 0; k < 3; k++) out[n++] = atc[k];
    if (need_decode(GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG)) for (int k = 0; k < 4; k++) out[n++] = pvr[k];
    return n;
}

static void imp_glGetIntegerv(cpu_t *c) {
    uint32_t pname = rt_arg(c, 0), out = rt_arg(c, 1);
    if (!out) return;
    if (!HOST(glGetIntegerv)) { rt_st32(out, 0); return; }
    if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS || pname == GL_COMPRESSED_TEXTURE_FORMATS) {
        int32_t extra[8], host[256];
        int ne = emulated_formats(extra), nh = 0;
        p_glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nh);
        if (nh < 0) nh = 0;
        if (nh > 256) nh = 256;
        if (pname == GL_NUM_COMPRESSED_TEXTURE_FORMATS) { rt_st32(out, (uint32_t)(nh + ne)); return; }
        if (nh) p_glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, host);
        for (int k = 0; k < nh; k++) rt_st32(out + 4 * (uint32_t)k, (uint32_t)host[k]);
        for (int k = 0; k < ne; k++) rt_st32(out + 4 * (uint32_t)(nh + k), (uint32_t)extra[k]);
        return;
    }
    p_glGetIntegerv(pname, G(out));
}
#endif /* RT_GL_UNIT */
