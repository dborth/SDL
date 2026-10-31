/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2022 GaryOderNichts <garyodernichts@gmail.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_RENDER_WIIU

#include "../SDL_sysrender.h"
#include "../../video/SDL_sysvideo.h"
#include "../../video/wiiu/SDL_wiiuvideo.h"
#include "SDL_shaders_wiiu.h"

#include <malloc.h>

#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/resource.h>
#include <gx2r/surface.h>

/* The GX2 render driver.
 *
 * Everything is drawn as triangles (SDL3 turns fills and copies into geometry
 * when a driver has no QueueFillRects, QueueCopy and QueueCopyEx), points and
 * line strips. Vertices are written by the Queue* hooks into SDL's CPU buffer;
 * RunCommandQueue() copies the whole buffer into one MEM2 vertex buffer and
 * points GX2 into it for every draw.
 *
 * The window is drawn into the color buffer of the display the window is on
 * (owned by the video driver, MEM1, tiled). RenderPresent() scales that to the
 * other screen when the window is routed to both, and then presents with the
 * video driver's shared function. Textures and render targets are linear
 * surfaces in MEM2, so they survive the application losing the foreground. */

#define WIIU_VERTEX_ALIGNMENT   GX2_VERTEX_BUFFER_ALIGNMENT
#define WIIU_MIN_VERTEX_BUFFER  (64 * 1024)

typedef struct WIIU_PixFmt
{
    GX2SurfaceFormat fmt;
    uint32_t compMap;
    bool valid;
} WIIU_PixFmt;

typedef struct WIIU_TextureData
{
    GX2Texture texture;
    GX2ColorBuffer cbuf;   // render targets only; shares the texture's memory
    bool is_target;
    bool gpu_wrote;        // drawn into since it was last sampled: caches need flushing before it is sampled
    OSTime timestamp;      // the GPU is done with the texture once this has retired
    Uint32 last_run;       // RunCommandQueue() that last queued it, to list it only once per run
} WIIU_TextureData;

typedef struct WIIU_VertexSlot
{
    void *memory;
    size_t capacity;
    OSTime timestamp;
} WIIU_VertexSlot;

typedef struct WIIU_DrawState
{
    bool valid;
    GX2ColorBuffer *target;
    SDL_Rect viewport;
    bool viewport_dirty;
    SDL_Rect cliprect;
    bool cliprect_enabled;
    bool cliprect_dirty;
    SDL_BlendMode blend;
    int shader;
    const GX2Texture *texture;
    const GX2Sampler *sampler;
    float projection[4][4];
    bool projection_dirty;
} WIIU_DrawState;

typedef struct WIIU_RenderData
{
    SDL_VideoData *videodata;
    int home;                           // the output (display) the window is on
    GX2ContextState *context;
    GX2Sampler samplers[2][2][2];       // [linear][wrap u][wrap v]
    WIIU_VertexSlot slots[2];
    int current_slot;
    void *quad_vertices;                // two triangles covering 0..1, for scaling to the other screen
    GX2Texture output_textures[WIIU_NUM_OUTPUTS]; // the video driver's color buffers as textures
    WIIU_DrawState state;
    Uint32 run_id;
    WIIU_TextureData **used;            // textures queued in the current RunCommandQueue()
    int num_used, used_capacity;
    bool shaders_loaded;
    bool listener_added;
} WIIU_RenderData;

/* ---------------------------------------------------------------------- */
// Pixel formats and blend modes

static WIIU_PixFmt GetPixFmt(SDL_PixelFormat format)
{
    WIIU_PixFmt out;

    out.valid = true;
    out.compMap = 0;
    out.fmt = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;

    /* The CPU is big endian, so a packed SDL format has its channels in memory
     * from the most significant to the least significant. GX2 reads the bytes as
     * R, G, B, A; the component map says which GX2 channel is which SDL channel. */
    switch (format) {
    case SDL_PIXELFORMAT_RGBA8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        break;
    case SDL_PIXELFORMAT_RGBX8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_1);
        break;
    case SDL_PIXELFORMAT_ARGB8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_R);
        break;
    case SDL_PIXELFORMAT_BGRA8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A);
        break;
    case SDL_PIXELFORMAT_BGRX8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_1);
        break;
    case SDL_PIXELFORMAT_ABGR8888:
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_A, GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R);
        break;

    case SDL_PIXELFORMAT_ARGB4444:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_R);
        break;
    case SDL_PIXELFORMAT_RGBA4444:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        break;
    case SDL_PIXELFORMAT_ABGR4444:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_A, GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R);
        break;
    case SDL_PIXELFORMAT_BGRA4444:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R4_G4_B4_A4;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A);
        break;

    case SDL_PIXELFORMAT_ARGB1555:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_R);
        break;
    case SDL_PIXELFORMAT_ABGR1555:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_A, GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R);
        break;
    case SDL_PIXELFORMAT_RGBA5551:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        break;
    case SDL_PIXELFORMAT_BGRA5551:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A);
        break;

    case SDL_PIXELFORMAT_RGB565:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G6_B5;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        break;
    case SDL_PIXELFORMAT_BGR565:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R5_G6_B5;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_B, GX2_SQ_SEL_G, GX2_SQ_SEL_R, GX2_SQ_SEL_A);
        break;

    case SDL_PIXELFORMAT_ARGB2101010:
        out.fmt = GX2_SURFACE_FORMAT_UNORM_R10_G10_B10_A2;
        out.compMap = GX2_COMP_MAP(GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A, GX2_SQ_SEL_R);
        break;

    default:
        out.valid = false;
        break;
    }
    return out;
}

static int GetBlendFactor(SDL_BlendFactor factor)
{
    switch (factor) {
    case SDL_BLENDFACTOR_ZERO:
        return GX2_BLEND_MODE_ZERO;
    case SDL_BLENDFACTOR_ONE:
        return GX2_BLEND_MODE_ONE;
    case SDL_BLENDFACTOR_SRC_COLOR:
        return GX2_BLEND_MODE_SRC_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR:
        return GX2_BLEND_MODE_INV_SRC_COLOR;
    case SDL_BLENDFACTOR_SRC_ALPHA:
        return GX2_BLEND_MODE_SRC_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA:
        return GX2_BLEND_MODE_INV_SRC_ALPHA;
    case SDL_BLENDFACTOR_DST_COLOR:
        return GX2_BLEND_MODE_DST_COLOR;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR:
        return GX2_BLEND_MODE_INV_DST_COLOR;
    case SDL_BLENDFACTOR_DST_ALPHA:
        return GX2_BLEND_MODE_DST_ALPHA;
    case SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA:
        return GX2_BLEND_MODE_INV_DST_ALPHA;
    default:
        return -1;
    }
}

static int GetBlendCombine(SDL_BlendOperation operation)
{
    switch (operation) {
    case SDL_BLENDOPERATION_ADD:
        return GX2_BLEND_COMBINE_MODE_ADD;
    case SDL_BLENDOPERATION_SUBTRACT:
        return GX2_BLEND_COMBINE_MODE_SUB;
    case SDL_BLENDOPERATION_REV_SUBTRACT:
        return GX2_BLEND_COMBINE_MODE_REV_SUB;
    case SDL_BLENDOPERATION_MINIMUM:
        return GX2_BLEND_COMBINE_MODE_MIN;
    case SDL_BLENDOPERATION_MAXIMUM:
        return GX2_BLEND_COMBINE_MODE_MAX;
    default:
        return -1;
    }
}

static bool WIIU_SupportsBlendMode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    return GetBlendFactor(SDL_GetBlendModeSrcColorFactor(blendMode)) != -1 &&
           GetBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blendMode)) != -1 &&
           GetBlendFactor(SDL_GetBlendModeDstColorFactor(blendMode)) != -1 &&
           GetBlendFactor(SDL_GetBlendModeDstAlphaFactor(blendMode)) != -1 &&
           GetBlendCombine(SDL_GetBlendModeColorOperation(blendMode)) != -1 &&
           GetBlendCombine(SDL_GetBlendModeAlphaOperation(blendMode)) != -1;
}

/* ---------------------------------------------------------------------- */
// GPU progress tracking

static bool GPUHasRetired(OSTime timestamp)
{
    return timestamp == 0 || GX2GetRetiredTimeStamp() >= timestamp;
}

// Block until the GPU is done with something stamped by RunCommandQueue().
// Everything stamped was flushed, so the timestamp will come.
static void WaitForGPU(WIIU_RenderData *data, OSTime timestamp)
{
    if (data->videodata->has_foreground && !GPUHasRetired(timestamp)) {
        GX2WaitTimeStamp(timestamp);
    }
}

static void MarkTextureUsed(WIIU_RenderData *data, WIIU_TextureData *tdata)
{
    if (tdata->last_run == data->run_id) {
        return;
    }
    if (data->num_used == data->used_capacity) {
        const int capacity = data->used_capacity ? data->used_capacity * 2 : 16;
        WIIU_TextureData **used = (WIIU_TextureData **)SDL_realloc(data->used, capacity * sizeof(*used));
        if (!used) {
            // Cannot track it: be safe and let the end of the run wait for everything
            return;
        }
        data->used = used;
        data->used_capacity = capacity;
    }
    tdata->last_run = data->run_id;
    data->used[data->num_used++] = tdata;
}

/* ---------------------------------------------------------------------- */
// Textures

static bool WIIU_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    const WIIU_PixFmt fmt = GetPixFmt(texture->format);
    const bool is_target = (texture->access == SDL_TEXTUREACCESS_TARGET);
    WIIU_TextureData *tdata;
    GX2RResourceFlags flags;

    if (!fmt.valid) {
        return SDL_SetError("Unsupported texture format");
    }
    if (is_target && texture->format != SDL_PIXELFORMAT_RGBA8888 && texture->format != SDL_PIXELFORMAT_RGBX8888) {
        // The channels of a target are written in GX2's R, G, B, A order, which only matches these
        return SDL_SetError("Render target textures must be RGBA8888 or RGBX8888 on the Wii U");
    }

    tdata = (WIIU_TextureData *)SDL_calloc(1, sizeof(*tdata));
    if (!tdata) {
        return false;
    }
    tdata->is_target = is_target;

    tdata->texture.surface.width = (uint32_t)texture->w;
    tdata->texture.surface.height = (uint32_t)texture->h;
    tdata->texture.surface.depth = 1;
    tdata->texture.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    tdata->texture.surface.format = fmt.fmt;
    tdata->texture.surface.aa = GX2_AA_MODE1X;
    tdata->texture.surface.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    tdata->texture.surface.mipLevels = 1;
    tdata->texture.viewNumMips = 1;
    tdata->texture.viewNumSlices = 1;
    tdata->texture.compMap = fmt.compMap;
    GX2CalcSurfaceSizeAndAlignment(&tdata->texture.surface);
    GX2InitTextureRegs(&tdata->texture);

    // MEM2 for everything: it survives the application losing the foreground
    flags = GX2R_RESOURCE_BIND_TEXTURE | GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_CPU_READ |
            GX2R_RESOURCE_USAGE_GPU_READ | GX2R_RESOURCE_USAGE_FORCE_MEM2;
    if (is_target) {
        flags |= GX2R_RESOURCE_BIND_COLOR_BUFFER | GX2R_RESOURCE_USAGE_GPU_WRITE;
        tdata->cbuf.surface = tdata->texture.surface;
        tdata->cbuf.viewNumSlices = 1;
    }

    if (!GX2RCreateSurface(&tdata->texture.surface, flags)) {
        SDL_free(tdata);
        return SDL_SetError("Could not allocate a %dx%d texture", texture->w, texture->h);
    }
    if (is_target) {
        // A color buffer over the same memory
        if (!GX2RCreateSurfaceUserMemory(&tdata->cbuf.surface, tdata->texture.surface.image,
                                         tdata->texture.surface.mipmaps, tdata->texture.surface.resourceFlags)) {
            GX2RDestroySurfaceEx(&tdata->texture.surface, GX2R_RESOURCE_BIND_NONE);
            SDL_free(tdata);
            return SDL_SetError("Could not create the color buffer of a render target");
        }
        GX2InitColorBufferRegs(&tdata->cbuf);
    }

    (void)data;
    texture->internal = tdata;
    return true;
}

static void *LockTextureMemory(WIIU_RenderData *data, WIIU_TextureData *tdata)
{
    // The GPU may still be reading it from an earlier frame
    WaitForGPU(data, tdata->timestamp);
    return GX2RLockSurfaceEx(&tdata->texture.surface, 0, GX2R_RESOURCE_BIND_NONE);
}

static bool WIIU_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    WIIU_TextureData *tdata = (WIIU_TextureData *)texture->internal;
    const int bpp = SDL_BYTESPERPIXEL(texture->format);
    const int row_bytes = (int)(tdata->texture.surface.pitch * bpp);
    Uint8 *base = (Uint8 *)LockTextureMemory(data, tdata);

    if (!base) {
        return SDL_SetError("Could not lock the texture");
    }
    *pixels = base + (size_t)rect->y * row_bytes + (size_t)rect->x * bpp;
    *pitch = row_bytes;
    return true;
}

static void WIIU_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    WIIU_TextureData *tdata = (WIIU_TextureData *)texture->internal;

    // Flushes the CPU cache and invalidates the GPU's texture cache
    GX2RUnlockSurfaceEx(&tdata->texture.surface, 0, GX2R_RESOURCE_BIND_NONE);
}

static bool WIIU_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    WIIU_TextureData *tdata = (WIIU_TextureData *)texture->internal;
    const int bpp = SDL_BYTESPERPIXEL(texture->format);
    const size_t length = (size_t)rect->w * bpp;
    const Uint8 *src = (const Uint8 *)pixels;
    Uint8 *dst;
    int dst_pitch;

    if (!WIIU_LockTexture(renderer, texture, rect, (void **)&dst, &dst_pitch)) {
        return false;
    }
    if ((int)length == pitch && (int)length == dst_pitch) {
        SDL_memcpy(dst, src, length * rect->h);
    } else {
        for (int row = 0; row < rect->h; row++) {
            SDL_memcpy(dst, src, length);
            src += pitch;
            dst += dst_pitch;
        }
    }
    WIIU_UnlockTexture(renderer, texture);
    (void)tdata;
    return true;
}

static void WIIU_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    WIIU_TextureData *tdata = (WIIU_TextureData *)texture->internal;

    if (!tdata) {
        return;
    }
    if (data->videodata->has_foreground) {
        // Targets may be being drawn into, not only read
        if (tdata->is_target) {
            GX2DrawDone();
        } else {
            WaitForGPU(data, tdata->timestamp);
        }
    }
    if (data->state.texture == &tdata->texture) {
        data->state.texture = NULL;
    }
    if (data->state.target == &tdata->cbuf) {
        data->state.target = NULL;
        data->state.valid = false;
    }
    for (int i = 0; i < data->num_used; i++) {
        if (data->used[i] == tdata) {
            data->used[i] = data->used[--data->num_used];
            break;
        }
    }
    if (tdata->is_target) {
        GX2RDestroySurfaceEx(&tdata->cbuf.surface, GX2R_RESOURCE_BIND_NONE);
    }
    GX2RDestroySurfaceEx(&tdata->texture.surface, GX2R_RESOURCE_BIND_NONE);
    SDL_free(tdata);
    texture->internal = NULL;
}

/* ---------------------------------------------------------------------- */
// Queueing: vertices

static bool WIIU_QueueNoOp(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    return true;
}

static void PackColor(const SDL_FColor *color, float color_scale, Uint8 *out)
{
    out[0] = (Uint8)SDL_roundf(SDL_clamp(color->r * color_scale, 0.0f, 1.0f) * 255.0f);
    out[1] = (Uint8)SDL_roundf(SDL_clamp(color->g * color_scale, 0.0f, 1.0f) * 255.0f);
    out[2] = (Uint8)SDL_roundf(SDL_clamp(color->b * color_scale, 0.0f, 1.0f) * 255.0f);
    out[3] = (Uint8)SDL_roundf(SDL_clamp(color->a, 0.0f, 1.0f) * 255.0f);
}

static bool WIIU_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    WIIU_VertexSolid *verts = (WIIU_VertexSolid *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), WIIU_VERTEX_ALIGNMENT, &cmd->data.draw.first);
    Uint8 rgba[4];

    if (!verts) {
        return false;
    }
    PackColor(&cmd->data.draw.color, cmd->data.draw.color_scale, rgba);
    cmd->data.draw.count = count;
    for (int i = 0; i < count; i++) {
        verts[i].x = points[i].x + 0.5f; // the center of the pixel
        verts[i].y = points[i].y + 0.5f;
        verts[i].r = rgba[0];
        verts[i].g = rgba[1];
        verts[i].b = rgba[2];
        verts[i].a = rgba[3];
    }
    return true;
}

static bool WIIU_QueueDrawLines(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    WIIU_VertexSolid *verts = (WIIU_VertexSolid *)SDL_AllocateRenderVertices(renderer, count * sizeof(*verts), WIIU_VERTEX_ALIGNMENT, &cmd->data.draw.first);
    Uint8 rgba[4];
    float prevx, prevy;

    if (!verts) {
        return false;
    }
    PackColor(&cmd->data.draw.color, cmd->data.draw.color_scale, rgba);
    cmd->data.draw.count = count;

    // Same as the OpenGL ES 2 renderer: pixel centers, and the end of each
    // segment pushed a quarter pixel along so the last pixel is not dropped.
    // Drawn as one line strip.
    prevx = points[0].x + 0.5f;
    prevy = points[0].y + 0.5f;
    verts[0].x = prevx;
    verts[0].y = prevy;
    for (int i = 1; i < count; i++) {
        const float xend = points[i].x + 0.5f;
        const float yend = points[i].y + 0.5f;
        const float angle = SDL_atan2f(yend - prevy, xend - prevx);
        prevx = xend + SDL_cosf(angle) * 0.25f;
        prevy = yend + SDL_sinf(angle) * 0.25f;
        verts[i].x = prevx;
        verts[i].y = prevy;
    }
    for (int i = 0; i < count; i++) {
        verts[i].r = rgba[0];
        verts[i].g = rgba[1];
        verts[i].b = rgba[2];
        verts[i].a = rgba[3];
    }
    return true;
}

static bool WIIU_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                               const float *xy, int xy_stride, const SDL_FColor *color, int color_stride, const float *uv, int uv_stride,
                               int num_vertices, const void *indices, int num_indices, int size_indices,
                               float scale_x, float scale_y)
{
    const int count = indices ? num_indices : num_vertices;
    const float color_scale = cmd->data.draw.color_scale;
    const size_t vertex_size = texture ? sizeof(WIIU_VertexTextured) : sizeof(WIIU_VertexSolid);
    Uint8 *verts;

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    verts = (Uint8 *)SDL_AllocateRenderVertices(renderer, count * vertex_size, WIIU_VERTEX_ALIGNMENT, &cmd->data.draw.first);
    if (!verts) {
        return false;
    }

    for (int i = 0; i < count; i++) {
        int j;
        const float *xy_;
        const SDL_FColor *col_;
        Uint8 rgba[4];

        if (size_indices == 4) {
            j = ((const Uint32 *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const Uint16 *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const Uint8 *)indices)[i];
        } else {
            j = i;
        }

        xy_ = (const float *)((const char *)xy + j * xy_stride);
        col_ = (const SDL_FColor *)((const char *)color + j * color_stride);
        PackColor(col_, color_scale, rgba);

        if (texture) {
            WIIU_VertexTextured *v = (WIIU_VertexTextured *)verts + i;
            const float *uv_ = (const float *)((const char *)uv + j * uv_stride);
            v->x = xy_[0] * scale_x;
            v->y = xy_[1] * scale_y;
            v->r = rgba[0];
            v->g = rgba[1];
            v->b = rgba[2];
            v->a = rgba[3];
            v->u = uv_[0];
            v->v = uv_[1];
        } else {
            WIIU_VertexSolid *v = (WIIU_VertexSolid *)verts + i;
            v->x = xy_[0] * scale_x;
            v->y = xy_[1] * scale_y;
            v->r = rgba[0];
            v->g = rgba[1];
            v->b = rgba[2];
            v->a = rgba[3];
        }
    }
    return true;
}

/* ---------------------------------------------------------------------- */
// Running the queue

static void ResetDrawState(WIIU_RenderData *data)
{
    WIIU_DrawState *state = &data->state;

    SDL_zerop(state);
    state->valid = true;
    state->blend = SDL_BLENDMODE_INVALID;
    state->shader = -1;
    state->viewport_dirty = true;
    state->cliprect_dirty = true;
    state->projection_dirty = true;
    state->projection[3][0] = -1.0f;
    state->projection[3][1] = 1.0f;
    state->projection[3][3] = 1.0f;
}

static void WIIU_InvalidateCachedState(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    data->state.valid = false;
}

// The context state starts out with GX2's defaults. These are the ones this renderer wants.
static void SetupContextDefaults(void)
{
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_NEVER);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE); // both windings are drawn
    GX2SetLineWidth(1.0f);
    GX2SetPointSize(1.0f, 1.0f);
}

static void BindTarget(WIIU_RenderData *data, GX2ColorBuffer *cbuf)
{
    WIIU_DrawState *state = &data->state;

    if (state->target != cbuf) {
        GX2SetColorBuffer(cbuf, GX2_RENDER_TARGET_0);
        state->target = cbuf;
        state->viewport_dirty = true;
        state->cliprect_dirty = true;
    }
}

static const GX2Sampler *PickSampler(WIIU_RenderData *data, const SDL_RenderCommand *cmd)
{
    const int linear = (cmd->data.draw.texture_scale_mode != SDL_SCALEMODE_NEAREST) ? 1 : 0;
    const int wrap_u = (cmd->data.draw.texture_address_mode_u == SDL_TEXTURE_ADDRESS_WRAP) ? 1 : 0;
    const int wrap_v = (cmd->data.draw.texture_address_mode_v == SDL_TEXTURE_ADDRESS_WRAP) ? 1 : 0;
    return &data->samplers[linear][wrap_u][wrap_v];
}

static void SetDrawState(WIIU_RenderData *data, const SDL_RenderCommand *cmd)
{
    WIIU_DrawState *state = &data->state;
    const WIIU_ShaderType shader = cmd->data.draw.texture ? WIIU_SHADER_TEXTURE : WIIU_SHADER_COLOR;
    WIIU_ShaderGroup *group = WIIU_GetShaderGroup(shader);
    const SDL_BlendMode blend = cmd->data.draw.blend;
    SDL_Texture *texture = cmd->data.draw.texture;
    bool shader_changed = false;

    if (state->viewport_dirty) {
        const SDL_Rect *viewport = &state->viewport;
        GX2SetViewport((float)viewport->x, (float)viewport->y, (float)viewport->w, (float)viewport->h, 0.0f, 1.0f);
        if (viewport->w && viewport->h) {
            state->projection[0][0] = 2.0f / viewport->w;
            state->projection[1][1] = -2.0f / viewport->h;
            state->projection_dirty = true;
        }
        state->viewport_dirty = false;
    }

    if (state->cliprect_dirty) {
        const SDL_Rect *viewport = &state->viewport;
        SDL_Rect rect = state->cliprect;
        SDL_Rect scissor;

        if (!state->cliprect_enabled) {
            rect.x = 0;
            rect.y = 0;
            rect.w = viewport->w;
            rect.h = viewport->h;
        }
        // The scissor must never be larger than the color buffer: clamp it to the viewport
        scissor.x = SDL_clamp(viewport->x + rect.x, viewport->x, viewport->x + viewport->w);
        scissor.y = SDL_clamp(viewport->y + rect.y, viewport->y, viewport->y + viewport->h);
        scissor.w = SDL_clamp(rect.w, 0, (viewport->x + viewport->w) - scissor.x);
        scissor.h = SDL_clamp(rect.h, 0, (viewport->y + viewport->h) - scissor.y);
        GX2SetScissor((uint32_t)scissor.x, (uint32_t)scissor.y, (uint32_t)scissor.w, (uint32_t)scissor.h);
        state->cliprect_dirty = false;
    }

    if (blend != state->blend) {
        if (blend == SDL_BLENDMODE_NONE) {
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
        } else {
            GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xFF, FALSE, TRUE);
            GX2SetBlendControl(GX2_RENDER_TARGET_0,
                               (GX2BlendMode)GetBlendFactor(SDL_GetBlendModeSrcColorFactor(blend)),
                               (GX2BlendMode)GetBlendFactor(SDL_GetBlendModeDstColorFactor(blend)),
                               (GX2BlendCombineMode)GetBlendCombine(SDL_GetBlendModeColorOperation(blend)),
                               TRUE,
                               (GX2BlendMode)GetBlendFactor(SDL_GetBlendModeSrcAlphaFactor(blend)),
                               (GX2BlendMode)GetBlendFactor(SDL_GetBlendModeDstAlphaFactor(blend)),
                               (GX2BlendCombineMode)GetBlendCombine(SDL_GetBlendModeAlphaOperation(blend)));
        }
        state->blend = blend;
    }

    if ((int)shader != state->shader) {
        WIIU_SelectShader(shader);
        state->shader = (int)shader;
        shader_changed = true;
    }

    if (shader_changed || state->projection_dirty) {
        GX2SetVertexUniformReg(group->vertexShader->uniformVars[0].offset, 16, state->projection);
        state->projection_dirty = false;
    }

    if (texture) {
        WIIU_TextureData *tdata = (WIIU_TextureData *)texture->internal;
        const GX2Sampler *sampler = PickSampler(data, cmd);

        if (tdata->gpu_wrote) {
            // It was drawn into: flush the color cache and drop the texture cache before sampling it
            GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE,
                          tdata->texture.surface.image, tdata->texture.surface.imageSize);
            tdata->gpu_wrote = false;
        }
        if (state->texture != &tdata->texture || state->sampler != sampler) {
            const uint32_t location = group->pixelShader->samplerVars[0].location;
            GX2SetPixelTexture(&tdata->texture, location);
            GX2SetPixelSampler(sampler, location);
            state->texture = &tdata->texture;
            state->sampler = sampler;
        }
        MarkTextureUsed(data, tdata);
    }
}

static void DrawVertices(GX2PrimitiveMode mode, const Uint8 *vertex_base, const SDL_RenderCommand *cmd, uint32_t stride)
{
    GX2SetAttribBuffer(0, (uint32_t)(cmd->data.draw.count * stride), stride, (void *)(vertex_base + cmd->data.draw.first));
    GX2DrawEx(mode, (uint32_t)cmd->data.draw.count, 0, 1);
}

static bool WIIU_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    SDL_VideoData *videodata = data->videodata;
    WIIU_DrawState *state;
    WIIU_VertexSlot *slot = NULL;
    GX2ColorBuffer *target;
    const Uint8 *vertex_base = NULL;
    bool drew = false;

    // The queue still runs while hidden; there is no GPU to draw with in the background
    if (!videodata->has_foreground) {
        return true;
    }

    data->run_id++;
    data->num_used = 0;

    // The vertices: one MEM2 buffer per run, two in rotation so the GPU can still read the last one
    if (vertsize > 0) {
        slot = &data->slots[data->current_slot];
        data->current_slot ^= 1;
        WaitForGPU(data, slot->timestamp);
        if (slot->capacity < vertsize) {
            size_t capacity = SDL_max((size_t)WIIU_MIN_VERTEX_BUFFER, slot->capacity);
            while (capacity < vertsize) {
                capacity *= 2;
            }
            free(slot->memory); // This should NOT be SDL_free()
            slot->memory = memalign(WIIU_VERTEX_ALIGNMENT, capacity); // This should NOT be SDL_memalign()
            slot->capacity = slot->memory ? capacity : 0;
            if (!slot->memory) {
                return SDL_OutOfMemory();
            }
        }
        SDL_memcpy(slot->memory, vertices, vertsize);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, slot->memory, (uint32_t)vertsize);
        vertex_base = (const Uint8 *)slot->memory;
    }

    GX2SetContextState(data->context);
    if (!data->state.valid) {
        ResetDrawState(data);
    }
    state = &data->state;

    if (renderer->target) {
        WIIU_TextureData *tdata = (WIIU_TextureData *)renderer->target->internal;
        target = &tdata->cbuf;
        tdata->gpu_wrote = true;
        MarkTextureUsed(data, tdata);
    } else {
        target = &videodata->outputs[data->home].color_buffer;
    }
    BindTarget(data, target);

    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT: {
            const SDL_Rect *rect = &cmd->data.viewport.rect;
            if (SDL_memcmp(&state->viewport, rect, sizeof(*rect)) != 0) {
                state->viewport = *rect;
                state->viewport_dirty = true;
                state->cliprect_dirty = true; // the scissor is relative to the viewport
            }
            break;
        }

        case SDL_RENDERCMD_SETCLIPRECT: {
            const SDL_Rect *rect = &cmd->data.cliprect.rect;
            if (state->cliprect_enabled != cmd->data.cliprect.enabled || SDL_memcmp(&state->cliprect, rect, sizeof(*rect)) != 0) {
                state->cliprect_enabled = cmd->data.cliprect.enabled;
                state->cliprect = *rect;
                state->cliprect_dirty = true;
            }
            break;
        }

        case SDL_RENDERCMD_SETDRAWCOLOR:
            break;

        case SDL_RENDERCMD_CLEAR: {
            const float scale = cmd->data.color.color_scale;
            GX2ClearColor(target, cmd->data.color.color.r * scale, cmd->data.color.color.g * scale,
                          cmd->data.color.color.b * scale, cmd->data.color.color.a);
            // Clearing changes the context state; restore ours
            GX2SetContextState(data->context);
            drew = true;
            break;
        }

        case SDL_RENDERCMD_DRAW_POINTS:
            SetDrawState(data, cmd);
            DrawVertices(GX2_PRIMITIVE_MODE_POINTS, vertex_base, cmd, sizeof(WIIU_VertexSolid));
            drew = true;
            break;

        case SDL_RENDERCMD_DRAW_LINES:
            SetDrawState(data, cmd);
            DrawVertices(GX2_PRIMITIVE_MODE_LINE_STRIP, vertex_base, cmd, sizeof(WIIU_VertexSolid));
            drew = true;
            break;

        case SDL_RENDERCMD_GEOMETRY:
            SetDrawState(data, cmd);
            DrawVertices(GX2_PRIMITIVE_MODE_TRIANGLES, vertex_base, cmd,
                         cmd->data.draw.texture ? sizeof(WIIU_VertexTextured) : sizeof(WIIU_VertexSolid));
            drew = true;
            break;

        // Fills and copies are turned into geometry by SDL because we do not queue them
        case SDL_RENDERCMD_FILL_RECTS:
        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
        case SDL_RENDERCMD_NO_OP:
        default:
            break;
        }
        cmd = cmd->next;
    }

    // Submit now, so that the timestamp of everything this run used exists and
    // CPU access to it later can wait on the right thing.
    GX2Flush();
    if (drew) {
        const OSTime timestamp = GX2GetLastSubmittedTimeStamp();
        if (slot) {
            slot->timestamp = timestamp;
        }
        for (int i = 0; i < data->num_used; i++) {
            data->used[i]->timestamp = timestamp;
        }
    }
    if (renderer->target) {
        // The next sampling of the target has to see these writes
        ((WIIU_TextureData *)renderer->target->internal)->gpu_wrote = true;
    }
    return true;
}

static bool WIIU_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    // The target is bound at the start of the next RunCommandQueue()
    return true;
}

/* ---------------------------------------------------------------------- */
// Reading pixels

static SDL_Surface *WIIU_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    SDL_VideoData *videodata = data->videodata;
    SDL_Surface *surface;
    GX2Surface *source;
    GX2Surface linear;
    SDL_PixelFormat format;
    int src_width, src_height;

    if (!videodata->has_foreground) {
        SDL_SetError("Cannot read pixels while the application is in the background");
        return NULL;
    }

    /* Both the window's color buffer (tiled, MEM1) and a render target (linear,
     * MEM2) are read the same way: the GPU copies them into a linear surface
     * and the CPU reads that. Reading a target's memory directly after
     * GX2DrawDone() returned zeros under Cemu, which does not write GPU
     * rendered textures back to RAM on its own; GX2CopySurface does reach RAM. */
    if (renderer->target) {
        WIIU_TextureData *tdata = (WIIU_TextureData *)renderer->target->internal;
        source = &tdata->texture.surface;
        format = renderer->target->format;
        src_width = renderer->target->w;
        src_height = renderer->target->h;
    } else {
        source = &videodata->outputs[data->home].color_buffer.surface;
        format = SDL_PIXELFORMAT_RGBA8888;
        src_width = (int)source->width;
        src_height = (int)source->height;
    }
    if (rect->x < 0 || rect->y < 0 || rect->x + rect->w > src_width || rect->y + rect->h > src_height) {
        SDL_SetError("Tried to read outside of the render target");
        return NULL;
    }

    SDL_zero(linear);
    linear.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    linear.width = source->width;
    linear.height = source->height;
    linear.depth = 1;
    linear.mipLevels = 1;
    linear.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    linear.aa = GX2_AA_MODE1X;
    linear.use = GX2_SURFACE_USE_TEXTURE;
    linear.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
    GX2CalcSurfaceSizeAndAlignment(&linear);
    linear.image = memalign(linear.alignment, linear.imageSize); // This should NOT be SDL_memalign()
    if (!linear.image) {
        SDL_OutOfMemory();
        return NULL;
    }

    // What was drawn has to be visible to the copy
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE, source->image, source->imageSize);
    GX2SetContextState(data->context);
    GX2CopySurface(source, 0, 0, &linear, 0, 0);
    GX2DrawDone();
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU, linear.image, linear.imageSize);
    data->state.valid = false; // the copy changed the GPU state

    surface = SDL_CreateSurface(rect->w, rect->h, format);
    if (surface) {
        const Uint8 *src = (const Uint8 *)linear.image;
        const int pitch = (int)(linear.pitch * 4);
        for (int row = 0; row < rect->h; row++) {
            SDL_memcpy((Uint8 *)surface->pixels + (size_t)row * surface->pitch,
                       src + (size_t)(rect->y + row) * pitch + (size_t)rect->x * 4,
                       (size_t)rect->w * 4);
        }
    }
    free(linear.image); // This should NOT be SDL_free()
    return surface;
}

/* ---------------------------------------------------------------------- */
// Presenting

// Draw the home output's color buffer, scaled to fit, into another output's color buffer
static void BlitToOutput(WIIU_RenderData *data, int dst_index)
{
    SDL_VideoData *videodata = data->videodata;
    WIIU_Output *src = &videodata->outputs[data->home];
    WIIU_Output *dst = &videodata->outputs[dst_index];
    WIIU_ShaderGroup *group = WIIU_GetShaderGroup(WIIU_SHADER_TEXTURE);
    float projection[4][4];
    float scale, vw, vh;
    uint32_t location;

    // Keep the aspect ratio; the rest stays black
    scale = SDL_min((float)dst->width / (float)src->width, (float)dst->height / (float)src->height);
    vw = (float)src->width * scale;
    vh = (float)src->height * scale;

    GX2SetContextState(data->context);
    GX2SetColorBuffer(&dst->color_buffer, GX2_RENDER_TARGET_0);
    if (vw < (float)dst->width - 0.5f || vh < (float)dst->height - 0.5f) {
        GX2ClearColor(&dst->color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
        GX2SetContextState(data->context);
    }
    GX2SetViewport(((float)dst->width - vw) / 2.0f, ((float)dst->height - vh) / 2.0f, vw, vh, 0.0f, 1.0f);
    GX2SetScissor(0, 0, dst->width, dst->height);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);

    SDL_zero(projection);
    projection[0][0] = 2.0f; // the quad is 0..1
    projection[1][1] = -2.0f;
    projection[3][0] = -1.0f;
    projection[3][1] = 1.0f;
    projection[3][3] = 1.0f;

    WIIU_SelectShader(WIIU_SHADER_TEXTURE);
    GX2SetVertexUniformReg(group->vertexShader->uniformVars[0].offset, 16, projection);

    // What was drawn into the color buffer has to be visible to the texture units
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE,
                  src->color_buffer.surface.image, src->color_buffer.surface.imageSize);
    location = group->pixelShader->samplerVars[0].location;
    GX2SetPixelTexture(&data->output_textures[data->home], location);
    GX2SetPixelSampler(&data->samplers[1][0][0], location);

    GX2SetAttribBuffer(0, 6 * sizeof(WIIU_VertexTextured), sizeof(WIIU_VertexTextured), data->quad_vertices);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 6, 0, 1);

    data->state.valid = false; // everything above bypassed the cached state
}

static bool WIIU_RenderPresent(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    SDL_VideoData *videodata = data->videodata;
    SDL_WindowData *windowdata = renderer->window ? renderer->window->internal : NULL;
    const uint32_t targets = windowdata ? windowdata->target : (1u << data->home);

    if (!videodata->has_foreground) {
        return true; // the frame is dropped, like the window framebuffer path
    }

    // The window was drawn into the color buffer of its own display. Other
    // screens it is routed to get a scaled copy.
    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        if (i != data->home && (targets & (1u << i))) {
            BlitToOutput(data, i);
        }
    }
    if (!(targets & (1u << data->home))) {
        // Routed away from its own display: that screen must not show the frame
        GX2SetContextState(data->context);
        GX2ClearColor(&videodata->outputs[data->home].color_buffer, 0.0f, 0.0f, 0.0f, 1.0f);
        data->state.valid = false;
    }

    if (windowdata && windowdata->prevent_swap) {
        GX2Flush();
        return true; // the next window update that swaps will show this too
    }
    WIIU_PresentOutputs(videodata); // flushes
    return true;
}

static bool WIIU_SetVSync(SDL_Renderer *renderer, int vsync)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;
    return WIIU_SetSwapInterval(data->videodata, vsync);
}

/* ---------------------------------------------------------------------- */
// Create and destroy

// The video driver's color buffers as textures, for scaling to the other screen
static void BuildOutputTextures(WIIU_RenderData *data)
{
    for (int i = 0; i < WIIU_NUM_OUTPUTS; i++) {
        GX2Texture *texture = &data->output_textures[i];
        SDL_zerop(texture);
        texture->surface = data->videodata->outputs[i].color_buffer.surface;
        texture->viewNumMips = 1;
        texture->viewNumSlices = 1;
        texture->compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
        GX2InitTextureRegs(texture);
    }
}

static void WIIU_ForegroundChanged(bool acquired, void *userdata)
{
    WIIU_RenderData *data = (WIIU_RenderData *)userdata;

    // Either way the GPU state we cached is gone. After an acquire the color
    // buffers are at new addresses, so the textures made from them are redone.
    // Textures and render targets are in MEM2 and are not affected.
    data->state.valid = false;
    if (acquired) {
        BuildOutputTextures(data);
    }
}

static void WIIU_DestroyRenderer(SDL_Renderer *renderer)
{
    WIIU_RenderData *data = (WIIU_RenderData *)renderer->internal;

    if (!data) {
        return;
    }
    if (data->listener_added) {
        WIIU_RemoveForegroundListener(WIIU_ForegroundChanged, data);
    }
    if (data->videodata && data->videodata->has_foreground) {
        GX2DrawDone();
    }
    for (int i = 0; i < 2; i++) {
        free(data->slots[i].memory); // This should NOT be SDL_free()
    }
    free(data->quad_vertices); // This should NOT be SDL_free()
    free(data->context);       // This should NOT be SDL_free()
    if (data->shaders_loaded) {
        WIIU_DestroyShaders();
    }
    SDL_free(data->used);
    SDL_free(data);
}

static bool WIIU_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    WIIU_RenderData *data;
    SDL_VideoDevice *device = SDL_GetVideoDevice();
    SDL_DisplayData *displaydata;
    WIIU_VertexTextured *quad;
    static const float quad_xy[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

    SDL_SetupRendererColorspace(renderer, create_props);
    if (renderer->output_colorspace != SDL_COLORSPACE_SRGB) {
        return SDL_SetError("Unsupported output colorspace");
    }
    if (!window || !device || SDL_strcmp(device->name, "wiiu") != 0 || !window->internal) {
        return SDL_SetError("The GX2 renderer needs a window of the Wii U video driver");
    }
    displaydata = SDL_GetDisplayDriverDataForWindow(window);
    if (!displaydata) {
        return SDL_SetError("The window has no display");
    }
    if (!((SDL_VideoData *)device->internal)->has_foreground) {
        return SDL_SetError("Cannot create the GX2 renderer in the background");
    }

    data = (WIIU_RenderData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return false;
    }
    data->videodata = (SDL_VideoData *)device->internal;
    data->home = displaydata->output;
    renderer->internal = data;

    if (!WIIU_CreateShaders()) {
        WIIU_DestroyRenderer(renderer);
        renderer->internal = NULL;
        return false;
    }
    data->shaders_loaded = true;

    // Samplers: [linear][wrap u][wrap v]
    for (int linear = 0; linear < 2; linear++) {
        for (int wu = 0; wu < 2; wu++) {
            for (int wv = 0; wv < 2; wv++) {
                GX2Sampler *sampler = &data->samplers[linear][wu][wv];
                GX2InitSampler(sampler, GX2_TEX_CLAMP_MODE_CLAMP, linear ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
                GX2InitSamplerClamping(sampler, wu ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP,
                                       wv ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_CLAMP_MODE_CLAMP);
            }
        }
    }

    // A quad covering 0..1 for scaling one screen's picture to the other
    data->quad_vertices = memalign(WIIU_VERTEX_ALIGNMENT, 6 * sizeof(WIIU_VertexTextured)); // This should NOT be SDL_memalign()
    if (!data->quad_vertices) {
        WIIU_DestroyRenderer(renderer);
        renderer->internal = NULL;
        return SDL_OutOfMemory();
    }
    quad = (WIIU_VertexTextured *)data->quad_vertices;
    for (int i = 0; i < 6; i++) {
        quad[i].x = quad_xy[i][0];
        quad[i].y = quad_xy[i][1];
        quad[i].r = quad[i].g = quad[i].b = quad[i].a = 255;
        quad[i].u = quad_xy[i][0];
        quad[i].v = quad_xy[i][1];
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, data->quad_vertices, 6 * sizeof(WIIU_VertexTextured));

    // Our own context state, so that the video driver's contexts are left alone
    data->context = (GX2ContextState *)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState)); // This should NOT be SDL_memalign()
    if (!data->context) {
        WIIU_DestroyRenderer(renderer);
        renderer->internal = NULL;
        return SDL_OutOfMemory();
    }
    SDL_memset(data->context, 0, sizeof(GX2ContextState));
    GX2SetupContextStateEx(data->context, TRUE);
    GX2SetContextState(data->context);
    SetupContextDefaults();

    BuildOutputTextures(data);
    ResetDrawState(data);
    if (WIIU_AddForegroundListener(WIIU_ForegroundChanged, data)) {
        data->listener_added = true;
    }

    renderer->WindowEvent = NULL;
    renderer->SupportsBlendMode = WIIU_SupportsBlendMode;
    renderer->CreateTexture = WIIU_CreateTexture;
    renderer->UpdateTexture = WIIU_UpdateTexture;
    renderer->LockTexture = WIIU_LockTexture;
    renderer->UnlockTexture = WIIU_UnlockTexture;
    renderer->SetRenderTarget = WIIU_SetRenderTarget;
    renderer->QueueSetViewport = WIIU_QueueNoOp;
    renderer->QueueSetDrawColor = WIIU_QueueNoOp;
    renderer->QueueDrawPoints = WIIU_QueueDrawPoints;
    renderer->QueueDrawLines = WIIU_QueueDrawLines;
    renderer->QueueGeometry = WIIU_QueueGeometry;
    renderer->InvalidateCachedState = WIIU_InvalidateCachedState;
    renderer->RunCommandQueue = WIIU_RunCommandQueue;
    renderer->RenderReadPixels = WIIU_RenderReadPixels;
    renderer->RenderPresent = WIIU_RenderPresent;
    renderer->DestroyTexture = WIIU_DestroyTexture;
    renderer->DestroyRenderer = WIIU_DestroyRenderer;
    renderer->SetVSync = WIIU_SetVSync;
    renderer->window = window;
    renderer->name = WIIU_RenderDriver.name;

    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBX8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRX8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGB565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGR565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB4444);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA4444);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR4444);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA4444);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB1555);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR1555);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA5551);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA5551);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB2101010);
    SDL_SetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);

    return true;
}

SDL_RenderDriver WIIU_RenderDriver = {
    WIIU_CreateRenderer, "gx2"
};

#endif // SDL_VIDEO_RENDER_WIIU
