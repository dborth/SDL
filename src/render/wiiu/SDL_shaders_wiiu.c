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

#include "SDL_shaders_wiiu.h"

#include <malloc.h>
#include <stddef.h>
#include <gfd.h>
#include <gx2/mem.h>
#include <gx2/utils.h>
#include <gx2r/buffer.h>

#include "shaders/colorShader.inc"
#include "shaders/textureShader.inc"

static WIIU_ShaderGroup s_groups[WIIU_NUM_SHADERS];
static int s_refcount;

/* Loading a GSH file with libgfd into GX2R backed program memory. This is the
 * sequence wut's own examples use, and the SDL2 port (shared by both shaders). */

static GX2VertexShader *LoadVertexShader(uint32_t index, const void *file)
{
    uint32_t headerSize, programSize;
    GX2VertexShader *shader;
    void *program;

    if (index >= GFDGetVertexShaderCount(file)) {
        return NULL;
    }
    headerSize = GFDGetVertexShaderHeaderSize(index, file);
    programSize = GFDGetVertexShaderProgramSize(index, file);
    if (!headerSize || !programSize) {
        return NULL;
    }
    shader = (GX2VertexShader *)memalign(0x40, headerSize); // This should NOT be SDL_memalign()
    if (!shader) {
        return NULL;
    }

    shader->gx2rBuffer.flags = GX2R_RESOURCE_BIND_SHADER_PROGRAM | GX2R_RESOURCE_USAGE_CPU_READ |
                               GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    shader->gx2rBuffer.elemSize = programSize;
    shader->gx2rBuffer.elemCount = 1;
    shader->gx2rBuffer.buffer = NULL;
    if (!GX2RCreateBuffer(&shader->gx2rBuffer)) {
        free(shader);
        return NULL;
    }

    program = GX2RLockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
    if (!GFDGetVertexShader(shader, program, index, file)) {
        GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE);
        GX2RDestroyBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
        free(shader);
        return NULL;
    }
    GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
    // wut's examples invalidate the program again after unlocking
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->program, shader->size);
    return shader;
}

static GX2PixelShader *LoadPixelShader(uint32_t index, const void *file)
{
    uint32_t headerSize, programSize;
    GX2PixelShader *shader;
    void *program;

    if (index >= GFDGetPixelShaderCount(file)) {
        return NULL;
    }
    headerSize = GFDGetPixelShaderHeaderSize(index, file);
    programSize = GFDGetPixelShaderProgramSize(index, file);
    if (!headerSize || !programSize) {
        return NULL;
    }
    shader = (GX2PixelShader *)memalign(0x40, headerSize); // This should NOT be SDL_memalign()
    if (!shader) {
        return NULL;
    }

    shader->gx2rBuffer.flags = GX2R_RESOURCE_BIND_SHADER_PROGRAM | GX2R_RESOURCE_USAGE_CPU_READ |
                               GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
    shader->gx2rBuffer.elemSize = programSize;
    shader->gx2rBuffer.elemCount = 1;
    shader->gx2rBuffer.buffer = NULL;
    if (!GX2RCreateBuffer(&shader->gx2rBuffer)) {
        free(shader);
        return NULL;
    }

    program = GX2RLockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
    if (!GFDGetPixelShader(shader, program, index, file)) {
        GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_DISABLE_CPU_INVALIDATE | GX2R_RESOURCE_DISABLE_GPU_INVALIDATE);
        GX2RDestroyBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
        free(shader);
        return NULL;
    }
    GX2RUnlockBufferEx(&shader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->program, shader->size);
    return shader;
}

static bool CreateFetchShader(WIIU_ShaderGroup *group, GX2AttribStream *attributes, uint32_t count)
{
    const uint32_t size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);

    if (!size) {
        return false;
    }
    group->fetchShaderProgram = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, size); // This should NOT be SDL_memalign()
    if (!group->fetchShaderProgram) {
        return false;
    }
    GX2InitFetchShaderEx(&group->fetchShader, (uint8_t *)group->fetchShaderProgram, count, attributes,
                         GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, group->fetchShaderProgram, size);
    return true;
}

static bool LoadGroup(WIIU_ShaderGroup *group, const void *gsh, GX2AttribStream *attributes, uint32_t count)
{
    group->vertexShader = LoadVertexShader(0, gsh);
    group->pixelShader = LoadPixelShader(0, gsh);
    return group->vertexShader && group->pixelShader && CreateFetchShader(group, attributes, count);
}

static void FreeGroup(WIIU_ShaderGroup *group)
{
    if (group->vertexShader) {
        GX2RDestroyBufferEx(&group->vertexShader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
        free(group->vertexShader);
    }
    if (group->pixelShader) {
        GX2RDestroyBufferEx(&group->pixelShader->gx2rBuffer, GX2R_RESOURCE_BIND_NONE);
        free(group->pixelShader);
    }
    free(group->fetchShaderProgram);
    SDL_zerop(group);
}

static GX2AttribStream MakeAttrib(uint32_t location, uint32_t offset, GX2AttribFormat format, uint32_t mask)
{
    GX2AttribStream attrib;
    SDL_zero(attrib);
    attrib.location = location;
    attrib.buffer = 0;
    attrib.offset = offset;
    attrib.format = format;
    attrib.type = GX2_ATTRIB_INDEX_PER_VERTEX;
    attrib.aluDivisor = 0;
    attrib.mask = mask;
    attrib.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
    return attrib;
}

bool WIIU_CreateShaders(void)
{
    GX2AttribStream attributes[3];
    const uint32_t xy01 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
    const uint32_t xyzw = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);

    if (s_refcount++ > 0) {
        return true;
    }

    // a_position, a_color
    attributes[0] = MakeAttrib(0, offsetof(WIIU_VertexSolid, x), GX2_ATTRIB_FORMAT_FLOAT_32_32, xy01);
    attributes[1] = MakeAttrib(1, offsetof(WIIU_VertexSolid, r), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8, xyzw);
    if (!LoadGroup(&s_groups[WIIU_SHADER_COLOR], colorShader_gsh, attributes, 2)) {
        goto fail;
    }

    // a_position, a_color, a_texcoord
    attributes[0] = MakeAttrib(0, offsetof(WIIU_VertexTextured, x), GX2_ATTRIB_FORMAT_FLOAT_32_32, xy01);
    attributes[1] = MakeAttrib(1, offsetof(WIIU_VertexTextured, r), GX2_ATTRIB_FORMAT_UNORM_8_8_8_8, xyzw);
    attributes[2] = MakeAttrib(2, offsetof(WIIU_VertexTextured, u), GX2_ATTRIB_FORMAT_FLOAT_32_32, xy01);
    if (!LoadGroup(&s_groups[WIIU_SHADER_TEXTURE], textureShader_gsh, attributes, 3)) {
        goto fail;
    }
    return true;

fail:
    FreeGroup(&s_groups[WIIU_SHADER_COLOR]);
    FreeGroup(&s_groups[WIIU_SHADER_TEXTURE]);
    s_refcount = 0;
    return SDL_SetError("Could not load the GX2 shaders");
}

void WIIU_DestroyShaders(void)
{
    if (s_refcount > 0 && --s_refcount == 0) {
        FreeGroup(&s_groups[WIIU_SHADER_COLOR]);
        FreeGroup(&s_groups[WIIU_SHADER_TEXTURE]);
    }
}

void WIIU_SelectShader(WIIU_ShaderType shader)
{
    WIIU_ShaderGroup *group = &s_groups[shader];

    GX2SetFetchShader(&group->fetchShader);
    GX2SetVertexShader(group->vertexShader);
    GX2SetPixelShader(group->pixelShader);
}

WIIU_ShaderGroup *WIIU_GetShaderGroup(WIIU_ShaderType shader)
{
    return &s_groups[shader];
}

#endif // SDL_VIDEO_RENDER_WIIU
