#ifndef SIM_CONFIG_CHECK_H
#define SIM_CONFIG_CHECK_H

#include <nitro/types.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Growable text buffer: config writers fill one, then it is saved to disk or listed by --print-config
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} SIM_ConfigText;

void SIM_ConfigText_Printf(SIM_ConfigText *text, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
const char *SIM_ConfigText_Get(const SIM_ConfigText *text);
// Writes the text to path, replacing the file. Returns FALSE when it can't
BOOL SIM_ConfigText_Save(const SIM_ConfigText *text, const char *path);
void SIM_ConfigText_Free(SIM_ConfigText *text);

typedef struct {
    const char *section;
    const char *name;
} SIM_ConfigKeyName;

typedef struct {
    char *section;
    char *name;
    int line; // Line that set the value; 0 = default
    int rejectedLine; // Line whose value was invalid, so the default stayed
    const char *note; // Overrides the source shown by --print-config, e.g. a migrated legacy key
} SIM_ConfigCheckEntry;

// Records which keys a config file sets, so that mistakes are reported instead of ignored, and
// --print-config can say where each effective value came from
typedef struct {
    const char *path;
    SIM_ConfigCheckEntry *entries;
    int count;
    BOOL fileFound;
    BOOL quiet; // No warnings, for parsing text that isn't the file on disk
} SIM_ConfigCheck;

// The file's keys are the ones its writer emits (writerText), plus keys still read for compatibility
void SIM_ConfigCheck_Init(SIM_ConfigCheck *check, const char *path, const char *writerText,
    const SIM_ConfigKeyName *legacyKeys, int legacyCount);
void SIM_ConfigCheck_Free(SIM_ConfigCheck *check);

// Call for every key the parser reports. Returns FALSE, after a warning, when the key isn't one of the
// file's keys, e.g. because it is in the wrong section or misspelled. A handler then ignores it
BOOL SIM_ConfigCheck_Key(SIM_ConfigCheck *check, const char *section, const char *name, int line);
// Reports a value the handler couldn't use, so the setting keeps its previous value
void SIM_ConfigCheck_BadValue(SIM_ConfigCheck *check, const char *section, const char *name,
    const char *value, int line, const char *expected);
// Credits a key's value to another line, e.g. a setting derived from a legacy key
void SIM_ConfigCheck_SetSource(SIM_ConfigCheck *check, const char *section, const char *name, int line,
    const char *note);
// Reports the result of ini_parse: -1 = file missing, > 0 = first line that isn't valid ini. consequence
// finishes the sentence "not a key=value line, so ..."
void SIM_ConfigCheck_ParseResult(SIM_ConfigCheck *check, int result, const char *consequence);

// Lists writerText's keys with their values and where each came from, as ini text
void SIM_ConfigCheck_Print(const SIM_ConfigCheck *check, const char *writerText, FILE *out);

// Number of warnings printed so far, by any file
int SIM_ConfigCheck_WarningCount(void);

// Set by --print-config: load the configs without creating missing files, list them, then exit
void SIM_ConfigCheck_SetPrintOnly(BOOL printOnly);
BOOL SIM_ConfigCheck_PrintOnly(void);

#ifdef __cplusplus
}
#endif

#endif
