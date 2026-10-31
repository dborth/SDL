/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

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

#ifdef SDL_VIDEO_DRIVER_WIIU

#include "SDL_wiiu_gfx_heap.h"

#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>

#include <malloc.h>

// Tag for the frame heap state recorded before MEM1 is taken. 'SDL3'
#define WIIU_FRM_HEAP_STATE_TAG 0x53444C33

static MEMHeapHandle s_mem1_heap;
static MEMHeapHandle s_foreground_heap;

// Take everything that is left in a frame heap and turn it into an expanding heap.
static MEMHeapHandle CreateExpHeapFromFrmHeap(MEMHeapHandle frm)
{
    uint32_t size = MEMGetAllocatableSizeForFrmHeapEx(frm, 4);
    void *base;
    MEMHeapHandle heap;

    if (!size) {
        SDL_SetError("No memory left in the frame heap");
        return NULL;
    }
    base = MEMAllocFromFrmHeapEx(frm, size, 4);
    if (!base) {
        SDL_SetError("Could not take 0x%x bytes from the frame heap", size);
        return NULL;
    }
    heap = MEMCreateExpHeapEx(base, size, 0);
    if (!heap) {
        SDL_SetError("MEMCreateExpHeapEx failed");
        MEMFreeToFrmHeap(frm, MEM_FRM_HEAP_FREE_ALL);
        return NULL;
    }
    return heap;
}

bool WIIU_GfxHeap_MEM1Init(void)
{
    MEMHeapHandle frm = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    if (s_mem1_heap) {
        return true;
    }
    if (!MEMRecordStateForFrmHeap(frm, WIIU_FRM_HEAP_STATE_TAG)) {
        return SDL_SetError("MEMRecordStateForFrmHeap failed for MEM1");
    }
    s_mem1_heap = CreateExpHeapFromFrmHeap(frm);
    if (!s_mem1_heap) {
        MEMFreeByStateToFrmHeap(frm, WIIU_FRM_HEAP_STATE_TAG);
        return false;
    }
    return true;
}

void WIIU_GfxHeap_MEM1Destroy(void)
{
    MEMHeapHandle frm = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    if (s_mem1_heap) {
        MEMDestroyExpHeap(s_mem1_heap);
        s_mem1_heap = NULL;
        MEMFreeByStateToFrmHeap(frm, WIIU_FRM_HEAP_STATE_TAG);
    }
}

bool WIIU_GfxHeap_ForegroundInit(void)
{
    if (s_foreground_heap) {
        return true;
    }
    s_foreground_heap = CreateExpHeapFromFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG));
    return s_foreground_heap != NULL;
}

void WIIU_GfxHeap_ForegroundDestroy(void)
{
    if (s_foreground_heap) {
        MEMDestroyExpHeap(s_foreground_heap);
        s_foreground_heap = NULL;
        MEMFreeToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG), MEM_FRM_HEAP_FREE_ALL);
    }
}

void *WIIU_GfxHeap_MEM1Alloc(uint32_t size, uint32_t align)
{
    if (!s_mem1_heap) {
        return NULL;
    }
    return MEMAllocFromExpHeapEx(s_mem1_heap, size, align < 4 ? 4 : (int32_t)align);
}

void WIIU_GfxHeap_MEM1Free(void *block)
{
    if (s_mem1_heap && block) {
        MEMFreeToExpHeap(s_mem1_heap, block);
    }
}

void *WIIU_GfxHeap_ForegroundAlloc(uint32_t size, uint32_t align)
{
    if (!s_foreground_heap) {
        return NULL;
    }
    return MEMAllocFromExpHeapEx(s_foreground_heap, size, align < 4 ? 4 : (int32_t)align);
}

void WIIU_GfxHeap_ForegroundFree(void *block)
{
    if (s_foreground_heap && block) {
        MEMFreeToExpHeap(s_foreground_heap, block);
    }
}

static bool GX2RWantsMEM1(GX2RResourceFlags flags)
{
    // Color, depth and scan buffers belong in MEM1, unless MEM2 is forced.
    return (flags & (GX2R_RESOURCE_BIND_COLOR_BUFFER | GX2R_RESOURCE_BIND_DEPTH_BUFFER |
                     GX2R_RESOURCE_BIND_SCAN_BUFFER | GX2R_RESOURCE_USAGE_FORCE_MEM1)) &&
           !(flags & GX2R_RESOURCE_USAGE_FORCE_MEM2);
}

void *WIIU_GfxHeap_GX2RAlloc(GX2RResourceFlags flags, uint32_t size, uint32_t alignment)
{
    if (GX2RWantsMEM1(flags)) {
        return WIIU_GfxHeap_MEM1Alloc(size, alignment);
    }
    return memalign(alignment, size); // This should NOT be SDL_memalign()
}

void WIIU_GfxHeap_GX2RFree(GX2RResourceFlags flags, void *block)
{
    if (GX2RWantsMEM1(flags)) {
        WIIU_GfxHeap_MEM1Free(block);
    } else {
        free(block); // This should NOT be SDL_free()
    }
}

#endif // SDL_VIDEO_DRIVER_WIIU
