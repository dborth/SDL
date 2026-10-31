/*
  filesystem tests - base path, pref path, file I/O, directory
  enumeration, stat, rename and remove
  All output goes through SDL_Log (OSReport on Wii U). Returns 0 if every
  check passed. Everything is created under SDL_GetPrefPath() and removed
  again.
*/
#include <SDL3/SDL.h>

#include "console.h"

#include <unistd.h>

static int failures;

static void check(const char *what, bool ok)
{
    SDL_Log("%s: %s", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

#define SEC_NS(x) ((SDL_Time)SDL_SECONDS_TO_NS(x))

static const char *join(char *out, size_t len, const char *dir, const char *name)
{
    SDL_snprintf(out, len, "%s%s", dir, name);
    return out;
}

typedef struct
{
    int count;
    bool seen_a, seen_b, seen_sub, saw_dot;
} EnumState;

static SDL_EnumerationResult SDLCALL enum_cb(void *userdata, const char *dirname, const char *fname)
{
    EnumState *st = (EnumState *)userdata;
    (void)dirname;
    st->count++;
    if (SDL_strcmp(fname, "a.txt") == 0) {
        st->seen_a = true;
    } else if (SDL_strcmp(fname, "b.bin") == 0) {
        st->seen_b = true;
    } else if (SDL_strcmp(fname, "sub") == 0) {
        st->seen_sub = true;
    } else if (SDL_strcmp(fname, ".") == 0 || SDL_strcmp(fname, "..") == 0) {
        st->saw_dot = true;
    }
    return SDL_ENUM_CONTINUE;
}

static void test_paths(int argc, char *argv[])
{
    char cwd[512];

    SDL_Log("INFO argc=%d argv[0]=%s", argc, (argc > 0 && argv && argv[0]) ? argv[0] : "(none)");
    SDL_Log("INFO getcwd=%s", getcwd(cwd, sizeof(cwd)) ? cwd : "(failed)");

    const char *base = SDL_GetBasePath();
    SDL_Log("INFO SDL_GetBasePath=%s", base ? base : "(null)");
    check("SDL_GetBasePath returns a path ending in '/'", base && base[0] && base[SDL_strlen(base) - 1] == '/');

    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    SDL_Log("INFO SDL_GetUserFolder(HOME)=%s", home ? home : "(null)");
    check("SDL_GetUserFolder(HOME)", home != NULL);
    check("SDL_GetUserFolder(MUSIC) is unsupported", SDL_GetUserFolder(SDL_FOLDER_MUSIC) == NULL);
    SDL_ClearError();

    char *cd = SDL_GetCurrentDirectory();
    SDL_Log("INFO SDL_GetCurrentDirectory=%s", cd ? cd : "(null)");
    SDL_free(cd);
}

int tool_main(int argc, char *argv[])
{
    SDL_Log("SDL filesystem test (P4)");
    check("SDL_Init(0)", SDL_Init(0));

    test_paths(argc, argv);

    // ------------------------------------------------------------ pref path
    char *pref = SDL_GetPrefPath("sdl3-wiiu", "files-test");
    SDL_Log("INFO SDL_GetPrefPath=%s", pref ? pref : "(null)");
    check("SDL_GetPrefPath", pref != NULL);
    if (!pref) {
        SDL_Log("%d check(s) failed", failures + 1);
        return 1;
    }
    check("pref path ends in '/'", pref[SDL_strlen(pref) - 1] == '/');

    SDL_PathInfo info;
    check("pref path exists and is a directory", SDL_GetPathInfo(pref, &info) && info.type == SDL_PATHTYPE_DIRECTORY);

    char p1[640], p2[640], p3[640];

    // ------------------------------------------- write, read back, size
    const char text[] = "hello from the Wii U\n";
    join(p1, sizeof(p1), pref, "a.txt");
    check("SDL_SaveFile", SDL_SaveFile(p1, text, sizeof(text) - 1));

    size_t got = 0;
    char *data = (char *)SDL_LoadFile(p1, &got);
    check("SDL_LoadFile size and content", data && got == sizeof(text) - 1 && SDL_memcmp(data, text, got) == 0);
    SDL_free(data);

    check("SDL_GetPathInfo: file", SDL_GetPathInfo(p1, &info) && info.type == SDL_PATHTYPE_FILE && info.size == sizeof(text) - 1);

    // Times. wut converts the 1980 FS epoch for us; the log lets us see what the SD card reports.
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_Log("INFO now=%lld modify=%lld create=%lld access=%lld (ns since 1970)",
            (long long)now, (long long)info.modify_time, (long long)info.create_time, (long long)info.access_time);
    check("modify time is after 2000 and not more than a day in the future",
          info.modify_time > SEC_NS(946684800LL) && info.modify_time < now + SEC_NS(86400));
    check("modify time is within 10 minutes of the clock (a fresh write)",
          info.modify_time > now - SEC_NS(600) && info.modify_time < now + SEC_NS(600));

    // ------------------------------------------- SDL_IOStream: modes, seek
    join(p2, sizeof(p2), pref, "b.bin");
    SDL_IOStream *io = SDL_IOFromFile(p2, "wb");
    check("SDL_IOFromFile \"wb\"", io != NULL);
    if (io) {
        Uint8 buf[1000];
        for (int i = 0; i < (int)sizeof(buf); i++) {
            buf[i] = (Uint8)(i * 7);
        }
        check("SDL_WriteIO 1000 bytes", SDL_WriteIO(io, buf, sizeof(buf)) == sizeof(buf));
        check("SDL_CloseIO (write)", SDL_CloseIO(io));
    }
    io = SDL_IOFromFile(p2, "ab");
    check("SDL_IOFromFile \"ab\"", io != NULL);
    if (io) {
        check("append 4 bytes", SDL_WriteIO(io, "tail", 4) == 4);
        check("SDL_CloseIO (append)", SDL_CloseIO(io));
    }
    check("size after append is 1004", SDL_GetPathInfo(p2, &info) && info.size == 1004);

    io = SDL_IOFromFile(p2, "rb");
    check("SDL_IOFromFile \"rb\"", io != NULL);
    if (io) {
        Uint8 b[8];
        check("SDL_GetIOSize == 1004", SDL_GetIOSize(io) == 1004);
        check("SDL_SeekIO to 1000", SDL_SeekIO(io, 1000, SDL_IO_SEEK_SET) == 1000);
        check("read the appended tail", SDL_ReadIO(io, b, 4) == 4 && SDL_memcmp(b, "tail", 4) == 0);
        check("SDL_SeekIO to 10 and read", SDL_SeekIO(io, 10, SDL_IO_SEEK_SET) == 10 && SDL_ReadIO(io, b, 1) == 1 && b[0] == (Uint8)(10 * 7));
        SDL_CloseIO(io);
    }
    io = SDL_IOFromFile(p2, "r+b");
    check("SDL_IOFromFile \"r+b\" (update in place)", io != NULL);
    if (io) {
        check("seek, overwrite 2 bytes", SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) == 0 && SDL_WriteIO(io, "XY", 2) == 2);
        SDL_CloseIO(io);
        check("size unchanged by r+b", SDL_GetPathInfo(p2, &info) && info.size == 1004);
    }
    check("opening a missing file for reading fails", SDL_IOFromFile(join(p3, sizeof(p3), pref, "nope.txt"), "rb") == NULL);
    SDL_ClearError();

    // -------------------------------------- nested directories (parents)
    char nested[640];
    SDL_snprintf(nested, sizeof(nested), "%ssub/deeper/deepest", pref);
    check("SDL_CreateDirectory makes missing parents", SDL_CreateDirectory(nested));
    check("SDL_CreateDirectory on an existing directory succeeds", SDL_CreateDirectory(nested));
    check("nested directory exists", SDL_GetPathInfo(nested, &info) && info.type == SDL_PATHTYPE_DIRECTORY);

    // ------------------------------------------------------- enumerate
    EnumState st;
    SDL_zero(st);
    check("SDL_EnumerateDirectory", SDL_EnumerateDirectory(pref, enum_cb, &st));
    SDL_Log("INFO enumerated %d entries", st.count);
    check("enumeration found a.txt, b.bin and sub", st.seen_a && st.seen_b && st.seen_sub);
    check("enumeration skips . and ..", !st.saw_dot);
    check("enumeration with a trailing slash missing also works", ({
        char noslash[640];
        SDL_strlcpy(noslash, pref, sizeof(noslash));
        noslash[SDL_strlen(noslash) - 1] = '\0';
        SDL_zero(st);
        SDL_EnumerateDirectory(noslash, enum_cb, &st) && st.seen_a;
    }));

    int gcount = 0;
    char **glob = SDL_GlobDirectory(pref, "*.txt", 0, &gcount);
    check("SDL_GlobDirectory \"*.txt\" finds one file", glob && gcount == 1 && SDL_strcmp(glob[0], "a.txt") == 0);
    SDL_free(glob);
    glob = SDL_GlobDirectory(pref, NULL, 0, &gcount);
    SDL_Log("INFO recursive glob found %d entries", gcount);
    check("recursive glob finds the nested directories", glob && gcount >= 5);
    SDL_free(glob);

    // ----------------------------------------------------- rename, copy
    join(p3, sizeof(p3), pref, "renamed.txt");
    check("SDL_RenamePath", SDL_RenamePath(p1, p3));
    check("old name is gone", !SDL_GetPathInfo(p1, NULL));
    SDL_ClearError();
    check("new name has the content", ({
        size_t n = 0;
        char *d = (char *)SDL_LoadFile(p3, &n);
        bool ok = d && n == sizeof(text) - 1 && SDL_memcmp(d, text, n) == 0;
        SDL_free(d);
        ok;
    }));
    char p4[640];
    join(p4, sizeof(p4), pref, "copy.txt");
    check("SDL_CopyFile", SDL_CopyFile(p3, p4) && SDL_GetPathInfo(p4, &info) && info.size == sizeof(text) - 1);

    // ---------------------------------------------------------- remove
    check("SDL_RemovePath (file)", SDL_RemovePath(p3) && SDL_RemovePath(p4) && SDL_RemovePath(p2));
    check("SDL_RemovePath on a missing path succeeds", SDL_RemovePath(p3));
    check("SDL_RemovePath (nested empty directories, innermost first)", ({
        char d3[640], d2[640], d1[640];
        SDL_snprintf(d3, sizeof(d3), "%ssub/deeper/deepest", pref);
        SDL_snprintf(d2, sizeof(d2), "%ssub/deeper", pref);
        SDL_snprintf(d1, sizeof(d1), "%ssub", pref);
        SDL_RemovePath(d3) && SDL_RemovePath(d2) && SDL_RemovePath(d1);
    }));
    SDL_zero(st);
    check("directory is empty again", SDL_EnumerateDirectory(pref, enum_cb, &st) && st.count == 0);

    // The pref directory itself is left in place: removing it also checks directory removal.
    char *prefdir = SDL_strdup(pref);
    prefdir[SDL_strlen(prefdir) - 1] = '\0';
    check("SDL_RemovePath (pref directory)", SDL_RemovePath(prefdir));
    check("pref directory is gone", !SDL_GetPathInfo(prefdir, NULL));
    SDL_ClearError();
    SDL_free(prefdir);
    SDL_free(pref);

    SDL_Quit();
    if (failures) {
        SDL_Log("%d check(s) failed", failures);
        return 1;
    }
    SDL_Log("all checks passed");
    return 0;
}
