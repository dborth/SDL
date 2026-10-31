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

#ifdef SDL_FILESYSTEM_WIIU

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
// System dependent filesystem routines for the Wii U (wut).
//
// File, directory and stat operations come from the POSIX fsops over wut's
// newlib "fs" devoptab. wut's startup code (__init_wut_devoptab, called from
// __init_wut before main) already mounts the SD card at /vol/external01 with
// FSAMount, registers the "fs" device as the default device and does
// chdir("fs:/vol/external01"). SDL therefore mounts nothing and unmounts
// nothing, and it never calls WHBMountSdCard (a separate FSClient mount that
// registers no devoptab path).
//
// Paths are always written with the explicit "fs:" device so that they do not
// depend on the default device or the current directory.

#include "../SDL_sysfilesystem.h"

#include <coreinit/dynload.h>

#include <sys/stat.h>

#define WIIU_SD_ROOT   "fs:/vol/external01/"
#define WIIU_APPS_DIR  WIIU_SD_ROOT "wiiu/apps/"
#define WIIU_SAVE_DIR  WIIU_SD_ROOT "wiiu/save/"

static bool IsDirectory(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

// Name of the running rpx without directory or ".rpx" extension, or NULL.
// OSDynLoad_GetModuleName() with module -1 names the main executable
static char *GetRPXName(void)
{
    char buf[256];
    int32_t size = (int32_t)sizeof(buf);

    SDL_zeroa(buf);
    if (OSDynLoad_GetModuleName((OSDynLoad_Module)-1, buf, &size) != OS_DYNLOAD_OK) {
        return NULL;
    }
    buf[sizeof(buf) - 1] = '\0';

    const char *name = buf;
    for (const char *p = buf; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            name = p + 1;
        }
    }

    char *result = SDL_strdup(name);
    if (!result) {
        return NULL;
    }

    const size_t len = SDL_strlen(result);
    if (len > 4 && SDL_strcasecmp(result + len - 4, ".rpx") == 0) {
        result[len - 4] = '\0';
    }
    if (!*result) {
        SDL_free(result);
        return NULL;
    }
    return result;
}

// FAT cannot store these in a file name, and ':' would be read as a device name.
static void AppendSanitized(char *dst, size_t dstlen, const char *src)
{
    size_t len = SDL_strlen(dst);
    for (; *src && len + 1 < dstlen; src++) {
        const char ch = *src;
        const bool bad = (ch == '\\') || (ch == '/') || (ch == ':') || (ch == '*') || (ch == '?') ||
                         (ch == '"') || (ch == '<') || (ch == '>') || (ch == '|') || ((unsigned char)ch < 0x20);
        dst[len++] = bad ? '_' : ch;
    }
    dst[len] = '\0';
}

// The directory the application was started from.
// this is a guess that is checked with stat(): the conventional install folder
// <SD>/wiiu/apps/<rpx name>/ if that exists, otherwise the SD card root.
char *SDL_SYS_GetBasePath(void)
{
    char *name = GetRPXName();
    if (name) {
        char *dir = NULL;
        if (SDL_asprintf(&dir, WIIU_APPS_DIR "%s", name) > 0 && dir && IsDirectory(dir)) {
            char *base = NULL;
            SDL_asprintf(&base, "%s/", dir);
            SDL_free(dir);
            SDL_free(name);
            return base;  // NULL (out of memory) is reported by SDL_asprintf
        }
        SDL_free(dir);
        SDL_free(name);
    }
    return SDL_strdup(WIIU_SD_ROOT);
}

char *SDL_SYS_GetExeName(void)
{
    char *name = GetRPXName();
    if (!name) {
        SDL_Unsupported();
    }
    return name;
}

// <SD>/wiiu/save/<org>/<app>/ (or <SD>/wiiu/save/<app>/ with no organization).
//
// Saves live apart from the application folder so that replacing or deleting
// <SD>/wiiu/apps/<app>/ does not take the player's data with it, and the
// organization level keeps two programs with the same app name apart.
char *SDL_SYS_GetPrefPath(const char *org, const char *app)
{
    char path[512];

    SDL_strlcpy(path, WIIU_SAVE_DIR, sizeof(path));
    if (org && *org) {
        AppendSanitized(path, sizeof(path), org);
        SDL_strlcat(path, "/", sizeof(path));
    }
    AppendSanitized(path, sizeof(path), app);
    SDL_strlcat(path, "/", sizeof(path));

    if (SDL_strlen(path) >= sizeof(path) - 1) {
        SDL_SetError("Preference path is too long");
        return NULL;
    }
    if (!SDL_CreateDirectory(path)) {
        return NULL;
    }
    return SDL_strdup(path);
}

// There are no per-user media folders on the Wii U. The SD card root is the
// home folder; everything else is unsupported.
char *SDL_SYS_GetUserFolder(SDL_Folder folder)
{
    if (folder == SDL_FOLDER_HOME) {
        return SDL_strdup(WIIU_SD_ROOT);
    }
    SDL_Unsupported();
    return NULL;
}

#endif // SDL_FILESYSTEM_WIIU
