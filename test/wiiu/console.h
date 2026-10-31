/*
  On-screen console for the Wii U test tools that do not use SDL video.

  A tool defines tool_main() instead of main(); console.c provides main(). It
  starts the console, runs tool_main(), shows the result and waits for HOME.

  The screen has:
    - rows 0 .. status_rows-1: a pinned status panel (Console_SetStatus)
    - the rows below, down to the footer: the newest lines of the log, which is
      everything sent through SDL_Log (also still sent to OSReport)
    - the last row: a footer that also shows the size this module assumes
*/
#ifndef WIIU_TOOL_CONSOLE_H
#define WIIU_TOOL_CONSOLE_H

#include <stdbool.h>

#ifndef CONSOLE_COLS
#define CONSOLE_COLS 80
#endif
#ifndef CONSOLE_ROWS
#define CONSOLE_ROWS 24
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Every tool defines this; the return value is the exit code (0 = all passed).
int tool_main(int argc, char *argv[]);

/* Process ProcUI messages and redraw (at most about 30 times a second).
   Returns false once the user has asked to exit */
bool Console_Update(void);

// The number of rows reserved at the top for the status panel (default 0).
void Console_SetStatusRows(int rows);

// Set one line of the status panel (printf style). Text past CONSOLE_COLS is cut.
void Console_SetStatus(int row, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
