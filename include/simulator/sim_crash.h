#ifndef SIM_CRASH_H
#define SIM_CRASH_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// log.txt: moves the previous one to log.prev.txt and copies stdout and stderr into the new one, still passing
// them on to the terminal.
void SIM_Log_Open(void);
// The log file alone, without the terminal, or NULL when there is no log. Flush after writing.
FILE *SIM_Log_File(void);
// Writes the effective settings to the log now, and again once a watched settings file stops changing.
// sim_config.ini is always watched.
typedef void (*SIM_LogConfigPrinter)(FILE *out);
void SIM_Log_SetConfigPrinter(SIM_LogConfigPrinter printer);
void SIM_Log_WatchConfigFile(const char *path);

// Crash reports (signals, Windows exceptions) when handlers is set and, when stallSeconds > 0, a report whenever
// no frame is presented for that long. Call from the game thread.
void SIM_Crash_Install(int handlers, int stallSeconds);
// Once per presented frame
void SIM_Crash_Heartbeat(void);
// What the game thread is doing, for stall reports. Static strings only.
void SIM_Crash_SetPhase(const char *phase);
// Adds the game's own lines to crash and stall reports. Runs inside a signal handler or with the game
// thread stopped, so it may only call SIM_Crash_Printf and SIM_Crash_PrintAddress.
typedef void (*SIM_CrashContextWriter)(void);
void SIM_Crash_SetContextWriter(SIM_CrashContextWriter writer);
// Signal-safe printf to the log and the terminal: %s %c %d %u %x %p, with l/ll/z sizes, and a width for %x
void SIM_Crash_Printf(const char *fmt, ...);
// A code address as module+offset, for tools/scripts/symbolize_crash.py
void SIM_Crash_PrintAddress(const char *label, const void *address);

#ifdef __cplusplus
}
#endif

#endif
