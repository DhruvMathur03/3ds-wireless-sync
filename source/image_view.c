#include "image_view.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image.h"

#define BOT_W 320
#define BOT_H 240

/* Refuse to even attempt decoding a JPEG bigger than this in either
 * dimension - guards against a huge photo exhausting the 3DS's heap
 * during decode, before we ever get a chance to downscale it. */
#define IV_MAX_SRC_DIM   4000

/* The decoded image is downscaled (nearest-neighbour) to fit within
 * this box before it's ever turned into a GPU texture - there's no
 * point keeping more resolution than the bottom screen can show. */
#define IV_TARGET_MAX_W  300
#define IV_TARGET_MAX_H  208

static C3D_Tex           s_tex;
static Tex3DS_SubTexture s_subtex;
static C2D_Image         s_img;
static bool              s_open = false;
static float             s_drawW = 0, s_drawH = 0;

static int next_pot(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

/* Nearest-neighbour resize of a tightly-packed RGBA8 buffer. Good
 * enough for a quick preview thumbnail; no need for interpolation. */
static u8 *nn_resize(const u8 *src, int sw, int sh, int dw, int dh) {
    u8 *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        const u8 *srow = src + (size_t)sy * sw * 4;
        u8 *drow = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            memcpy(drow + x * 4, srow + sx * 4, 4);
        }
    }
    return dst;
}

/* PICA200 (the 3DS GPU) stores textures in 8x8 Z-order ("Morton
 * order") tiles rather than plain row-major order, and GPU_RGBA8
 * pixels are packed in memory as A,B,G,R rather than R,G,B,A. This
 * converts a normal top-down RGBA buffer of size w x h into a tiled
 * buffer sized texW x texH (texW/texH must be the POT texture's
 * actual dimensions, since that's what determines row stride). Any
 * area outside w x h within the texture is left untouched by this
 * function - the caller should zero it first. */
static void tile_rgba8(u8 *dst, const u8 *src, int w, int h, int texW) {
    static const u8 xlut[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const u8 ylut[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    int blocksPerRow = (texW + 7) >> 3;

    for (int y = 0; y < h; y++) {
        int blockRow = y >> 3;
        int tileY = ylut[y & 7];
        for (int x = 0; x < w; x++) {
            int blockCol = x >> 3;
            int blockIndex = blockRow * blocksPerRow + blockCol;
            int tileIndex = xlut[x & 7] + tileY;
            int dstPixel = blockIndex * 64 + tileIndex;

            const u8 *p = src + ((size_t)y * w + x) * 4;
            u8 *o = dst + (size_t)dstPixel * 4;
            /* stb gives bytes as R,G,B,A; GPU_RGBA8 wants A,B,G,R */
            o[0] = p[3];
            o[1] = p[2];
            o[2] = p[1];
            o[3] = p[0];
        }
    }
}

bool iv_open(const char *path) {
    iv_close();

    int srcW, srcH, srcComp;
    if (!stbi_info(path, &srcW, &srcH, &srcComp)) return false;
    if (srcW <= 0 || srcH <= 0 || srcW > IV_MAX_SRC_DIM || srcH > IV_MAX_SRC_DIM)
        return false;

    int w, h, comp;
    u8 *pixels = stbi_load(path, &w, &h, &comp, 4);
    if (!pixels) return false;

    float scale = 1.0f;
    if (w > IV_TARGET_MAX_W || h > IV_TARGET_MAX_H)
        scale = fminf((float)IV_TARGET_MAX_W / w, (float)IV_TARGET_MAX_H / h);
    int dw = (int)(w * scale); if (dw < 1) dw = 1;
    int dh = (int)(h * scale); if (dh < 1) dh = 1;

    u8 *scaled = pixels;
    bool ownsScaled = false;
    if (dw != w || dh != h) {
        scaled = nn_resize(pixels, w, h, dw, dh);
        stbi_image_free(pixels);
        if (!scaled) return false;
        ownsScaled = true;
    }

    int texW = next_pot(dw);
    int texH = next_pot(dh);

    if (!C3D_TexInit(&s_tex, texW, texH, GPU_RGBA8)) {
        if (ownsScaled) free(scaled); else stbi_image_free(scaled);
        return false;
    }
    C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    memset(s_tex.data, 0, (size_t)texW * texH * 4);
    tile_rgba8((u8 *)s_tex.data, scaled, dw, dh, texW);
    C3D_TexFlush(&s_tex);

    if (ownsScaled) free(scaled); else stbi_image_free(scaled);

    s_subtex.width  = dw;
    s_subtex.height = dh;
    s_subtex.left   = 0.0f;
    s_subtex.top    = 1.0f;
    s_subtex.right  = (float)dw / texW;
    s_subtex.bottom = 1.0f - (float)dh / texH;

    s_img.tex    = &s_tex;
    s_img.subtex = &s_subtex;

    s_drawW = (float)dw;
    s_drawH = (float)dh;
    s_open = true;
    return true;
}

void iv_close(void) {
    if (s_open) {
        C3D_TexDelete(&s_tex);
        s_open = false;
    }
}

bool iv_is_open(void) {
    return s_open;
}

static void iv_text(C2D_TextBuf buf, const char *str, float x, float y, float scale, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, buf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void iv_draw(C2D_TextBuf textBuf, const char *filename, bool canSend) {
    if (!s_open) return;

    float x = (BOT_W - s_drawW) / 2.0f;
    float y = (BOT_H - s_drawH) / 2.0f - 10.0f;
    C2D_DrawImageAt(s_img, x, y, 0.5f, NULL, 1.0f, 1.0f);

    iv_text(textBuf, filename, 8, BOT_H - 34, 0.36f, C2D_Color32(0xd8, 0xda, 0xe4, 0xff));
    iv_text(textBuf, canSend ? "B: close  Up/Down: browse  A: send" : "B: close   Up/Down: browse",
            8, BOT_H - 18, 0.36f, C2D_Color32(0x9a, 0x9d, 0xb0, 0xff));
}
