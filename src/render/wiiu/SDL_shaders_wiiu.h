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

#ifndef SDL_shaders_wiiu_h_
#define SDL_shaders_wiiu_h_

#ifdef SDL_VIDEO_RENDER_WIIU

#include <gx2/shaders.h>

// Vertex formats the shaders' fetch shaders read. Packed, no padding.
typedef struct WIIU_VertexSolid
{
    float x, y;
    Uint8 r, g, b, a;
} WIIU_VertexSolid; // 12 bytes

typedef struct WIIU_VertexTextured
{
    float x, y;
    Uint8 r, g, b, a;
    float u, v;
} WIIU_VertexTextured; // 20 bytes

typedef enum
{
    WIIU_SHADER_COLOR,
    WIIU_SHADER_TEXTURE,
    WIIU_NUM_SHADERS
} WIIU_ShaderType;

typedef struct WIIU_ShaderGroup
{
    GX2VertexShader *vertexShader;
    GX2PixelShader *pixelShader;
    GX2FetchShader fetchShader;
    void *fetchShaderProgram;
} WIIU_ShaderGroup;

// Reference counted: the shaders are shared by all renderers
extern bool WIIU_CreateShaders(void);
extern void WIIU_DestroyShaders(void);
extern void WIIU_SelectShader(WIIU_ShaderType shader);
extern WIIU_ShaderGroup *WIIU_GetShaderGroup(WIIU_ShaderType shader);

#endif // SDL_VIDEO_RENDER_WIIU

#endif // SDL_shaders_wiiu_h_
