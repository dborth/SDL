/*
  On-screen console for the Wii U test tools. See console.h.

  The OSScreen and ProcUI handling follows wut. Buffers come from
  the MEM1 frame heap, are given back when the application loses the foreground
  and are allocated again when it returns.
*/
#include "console.h"

#include <SDL3/SDL.h>

#include <coreinit/cache.h>
#include <coreinit/core.h>
#include <coreinit/foreground.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/screen.h>
#include <coreinit/spinlock.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LINES              256
#define MAX_STATUS_ROWS        CONSOLE_ROWS
#define CONSOLE_FRAME_HEAP_TAG 0x0005D13A
#define BACKGROUND_COLOR       0x101830FF
#define REDRAW_INTERVAL_MS     33

static char sLog[LOG_LINES][CONSOLE_COLS + 1];
static int sLogCount; // total lines ever added
static char sStatus[MAX_STATUS_ROWS][CONSOLE_COLS + 1];
static int sStatusRows;

static OSSpinLock sLock;
static OSThread *sMainThread;
static void *sBufferTV, *sBufferDRC;
static uint32_t sSizeTV, sSizeDRC;
static BOOL sHasForeground;
static BOOL sExiting;
static bool sReady;
static OSTime sLastDraw;
static SDL_LogOutputFunction sPrevOutput;
static void *sPrevUserdata;

// ------------------------------------------------------------------- ProcUI

/* The same as WHBProcInit, WHBProcIsRunning and WHBProcShutdown. Run
   from the Homebrew Launcher, the normal ProcUI loop does not work, so 
   the HOME menu is disabled */
#define HBL_TITLE_ID           0x0005000013374842ull
#define MII_MAKER_JPN_TITLE_ID 0x000500101004A000ull
#define MII_MAKER_USA_TITLE_ID 0x000500101004A100ull
#define MII_MAKER_EUR_TITLE_ID 0x000500101004A200ull

static uint32_t sMainCore;
static BOOL sRunning;
static BOOL sFromHBL;

static uint32_t onSave(void *context)
{
    OSSavesDone_ReadyToRelease();
    return 0;
}

static uint32_t onHomeButtonDenied(void *context)
{
    if (sFromHBL) {
        sRunning = FALSE;
    }
    return 0;
}

static void procInit(void)
{
    uint64_t titleID = OSGetTitleID();

    if (titleID == HBL_TITLE_ID || titleID == MII_MAKER_JPN_TITLE_ID ||
        titleID == MII_MAKER_USA_TITLE_ID || titleID == MII_MAKER_EUR_TITLE_ID) {
        // Must come before ProcUIInitEx
        OSEnableHomeButtonMenu(FALSE);
        sFromHBL = TRUE;
    }

    sMainCore = OSGetCoreId();
    sRunning = TRUE;
    ProcUIInitEx(onSave, NULL);

    if (sFromHBL) {
        ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, onHomeButtonDenied, NULL, 100);
    }
}

static BOOL procIsRunning(void)
{
    ProcUIStatus status;

    if (sMainCore != OSGetCoreId()) {
        ProcUISubProcessMessages(TRUE);
        return sRunning;
    }

    status = ProcUIProcessMessages(TRUE);
    if (status == PROCUI_STATUS_EXITING) {
        sRunning = FALSE;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        ProcUIDrawDoneRelease();
    }

    if (!sRunning) {
        ProcUIShutdown();
    }
    return sRunning;
}

static void procShutdown(void)
{
    sRunning = FALSE;

    // From the Homebrew Launcher, going back to it takes a relaunch
    if (sFromHBL) {
        SYSRelaunchTitle(0, NULL);
    }
}

// ------------------------------------------------------------------ buffers

static uint32_t onAcquired(void *context)
{
    MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);

    MEMRecordStateForFrmHeap(heap, CONSOLE_FRAME_HEAP_TAG);
    sBufferTV = sSizeTV ? MEMAllocFromFrmHeapEx(heap, sSizeTV, 4) : NULL;
    sBufferDRC = sSizeDRC ? MEMAllocFromFrmHeapEx(heap, sSizeDRC, 4) : NULL;
    sHasForeground = TRUE;
    OSScreenSetBufferEx(SCREEN_TV, sBufferTV);
    OSScreenSetBufferEx(SCREEN_DRC, sBufferDRC);
    return 0;
}

static uint32_t onReleased(void *context)
{
    MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1), CONSOLE_FRAME_HEAP_TAG);
    sHasForeground = FALSE;
    return 0;
}

// --------------------------------------------------------------------- text

// Copy at most CONSOLE_COLS printable characters; tabs and control characters become spaces
static void copyLine(char *dst, const char *src, size_t length)
{
    size_t i;

    if (length > CONSOLE_COLS) {
        length = CONSOLE_COLS;
    }
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 32 || c > 126) ? ' ' : (char)c;
    }
    dst[length] = '\0';
}

// Add a message to the log, wrapped at CONSOLE_COLS. Caller holds the lock.
static void addMessage(const char *message)
{
    size_t length = strlen(message);

    do {
        size_t part = length > CONSOLE_COLS ? CONSOLE_COLS : length;

        copyLine(sLog[sLogCount % LOG_LINES], message, part);
        ++sLogCount;
        message += part;
        length -= part;
    } while (length > 0);
}

void Console_SetStatusRows(int rows)
{
    if (rows < 0) {
        rows = 0;
    }
    if (rows > CONSOLE_ROWS - 2) {
        rows = CONSOLE_ROWS - 2;
    }
    sStatusRows = rows;
}

void Console_SetStatus(int row, const char *fmt, ...)
{
    char text[256];
    va_list ap;

    if (row < 0 || row >= MAX_STATUS_ROWS) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    OSUninterruptibleSpinLock_Acquire(&sLock);
    copyLine(sStatus[row], text, strlen(text));
    OSUninterruptibleSpinLock_Release(&sLock);
}

// -------------------------------------------------------------------- draw

static void putLine(int row, const char *text)
{
    OSScreenPutFontEx(SCREEN_TV, 0, row, text);
    OSScreenPutFontEx(SCREEN_DRC, 0, row, text);
}

static void draw(void)
{
    char footer[CONSOLE_COLS + 1];
    int logRows = CONSOLE_ROWS - 1 - sStatusRows;
    int first, i;

    if (!sHasForeground) {
        return;
    }

    OSScreenClearBufferEx(SCREEN_TV, BACKGROUND_COLOR);
    OSScreenClearBufferEx(SCREEN_DRC, BACKGROUND_COLOR);

    OSUninterruptibleSpinLock_Acquire(&sLock);
    for (i = 0; i < sStatusRows; ++i) {
        putLine(i, sStatus[i]);
    }
    first = sLogCount > logRows ? sLogCount - logRows : 0;
    for (i = first; i < sLogCount && i - first < logRows; ++i) {
        putLine(sStatusRows + (i - first), sLog[i % LOG_LINES]);
    }
    OSUninterruptibleSpinLock_Release(&sLock);

    snprintf(footer, sizeof(footer), "HOME = exit    [last row %d of %d, %d columns]", CONSOLE_ROWS - 1, CONSOLE_ROWS, CONSOLE_COLS);
    putLine(CONSOLE_ROWS - 1, footer);

    DCFlushRange(sBufferTV, sSizeTV);
    DCFlushRange(sBufferDRC, sSizeDRC);
    OSScreenFlipBuffersEx(SCREEN_TV);
    OSScreenFlipBuffersEx(SCREEN_DRC);
    sLastDraw = OSGetSystemTime();
}

bool Console_Update(void)
{
    if (!sReady) {
        return true;
    }
    if (!procIsRunning()) {
        sExiting = TRUE;
        return false;
    }
    if (OSTicksToMilliseconds(OSGetSystemTime() - sLastDraw) >= REDRAW_INTERVAL_MS) {
        draw();
    }
    return true;
}

// ------------------------------------------------------------------- logging

/* SDL_Log goes to OSReport as before, and into the log. The main thread also
   gets the screen refreshed (and ProcUI serviced) here, so a test that logs
   progress without calling Console_Update() still shows it. */
static void SDLCALL logOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    if (sPrevOutput) {
        sPrevOutput(sPrevUserdata, category, priority, message);
    }
    OSUninterruptibleSpinLock_Acquire(&sLock);
    addMessage(message);
    OSUninterruptibleSpinLock_Release(&sLock);

    if (OSGetCurrentThread() == sMainThread) {
        Console_Update();
    }
}

// --------------------------------------------------------------- entry point

int main(int argc, char *argv[])
{
    int result;

    sMainThread = OSGetCurrentThread();
    OSInitSpinLock(&sLock);

    procInit();
    OSScreenInit();
    sSizeTV = OSScreenGetBufferSizeEx(SCREEN_TV);
    sSizeDRC = OSScreenGetBufferSizeEx(SCREEN_DRC);
    onAcquired(NULL);
    OSScreenEnableEx(SCREEN_TV, 1);
    OSScreenEnableEx(SCREEN_DRC, 1);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, onAcquired, NULL, 100);
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, onReleased, NULL, 100);

    SDL_GetLogOutputFunction(&sPrevOutput, &sPrevUserdata);
    SDL_SetLogOutputFunction(logOutput, NULL);
    sReady = true;
    draw();

    result = tool_main(argc, argv);

    SDL_Log("Finished, result %d. Press HOME to exit.", result);
    while (Console_Update()) {
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    sReady = false;
    SDL_SetLogOutputFunction(sPrevOutput, sPrevUserdata);
    if (sHasForeground) {
        OSScreenShutdown();
        onReleased(NULL);
    }
    procShutdown();
    return result;
}
