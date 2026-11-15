#include <simulator/config/config_check.h>
#include <simulator/config/ini.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int sWarningCount;
static BOOL sPrintOnly;

void SIM_ConfigText_Printf(SIM_ConfigText *text, const char *format, ...)
{
    va_list args, copy;

    va_start(args, format);
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed >= 0 && text->length + needed + 1 > text->capacity) {
        size_t capacity = text->capacity ? text->capacity : 1024;
        while (capacity < text->length + needed + 1) {
            capacity *= 2;
        }
        char *data = realloc(text->data, capacity);
        if (data == NULL) {
            needed = -1;
        } else {
            text->data = data;
            text->capacity = capacity;
        }
    }
    if (needed >= 0) {
        vsnprintf(text->data + text->length, needed + 1, format, args);
        text->length += needed;
    }
    va_end(args);
}

const char *SIM_ConfigText_Get(const SIM_ConfigText *text)
{
    return text->data != NULL ? text->data : "";
}

BOOL SIM_ConfigText_Save(const SIM_ConfigText *text, const char *path)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return FALSE;
    }
    BOOL ok = fwrite(SIM_ConfigText_Get(text), 1, text->length, file) == text->length;
    return fclose(file) == 0 && ok;
}

void SIM_ConfigText_Free(SIM_ConfigText *text)
{
    free(text->data);
    memset(text, 0, sizeof(*text));
}

static char *CopyString(const char *s)
{
    size_t length = strlen(s) + 1;
    char *copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, s, length);
    }
    return copy;
}

static void AddEntry(SIM_ConfigCheck *check, const char *section, const char *name)
{
    SIM_ConfigCheckEntry *entries = realloc(check->entries, (check->count + 1) * sizeof(*entries));
    if (entries == NULL) {
        return;
    }
    check->entries = entries;
    SIM_ConfigCheckEntry *entry = &entries[check->count++];
    memset(entry, 0, sizeof(*entry));
    entry->section = CopyString(section);
    entry->name = CopyString(name);
}

static int CollectKey(void *user, const char *section, const char *name, const char *value, int line)
{
    AddEntry(user, section, name);
    return 1;
}

static SIM_ConfigCheckEntry *FindEntry(const SIM_ConfigCheck *check, const char *section, const char *name)
{
    for (int i = 0; i < check->count; i++) {
        SIM_ConfigCheckEntry *entry = &check->entries[i];
        if (entry->section && entry->name && strcmp(entry->section, section) == 0 && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static BOOL EqualIgnoringCase(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

static void Warn(const SIM_ConfigCheck *check, int line, const char *format, ...)
{
    va_list args;

    if (check->quiet) {
        return;
    }
    sWarningCount++;
    if (line > 0) {
        fprintf(stderr, "[config] %s:%d: ", check->path, line);
    } else {
        fprintf(stderr, "[config] %s: ", check->path);
    }
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void SIM_ConfigCheck_Init(SIM_ConfigCheck *check, const char *path, const char *writerText,
    const SIM_ConfigKeyName *legacyKeys, int legacyCount)
{
    BOOL quiet = check->quiet;

    SIM_ConfigCheck_Free(check);
    check->path = path;
    check->quiet = quiet;
    ini_parse_string(writerText, CollectKey, check);
    for (int i = 0; i < legacyCount; i++) {
        AddEntry(check, legacyKeys[i].section, legacyKeys[i].name);
    }
}

void SIM_ConfigCheck_Free(SIM_ConfigCheck *check)
{
    for (int i = 0; i < check->count; i++) {
        free(check->entries[i].section);
        free(check->entries[i].name);
    }
    free(check->entries);
    check->entries = NULL;
    check->count = 0;
    check->fileFound = FALSE;
}

BOOL SIM_ConfigCheck_Key(SIM_ConfigCheck *check, const char *section, const char *name, int line)
{
    SIM_ConfigCheckEntry *entry = FindEntry(check, section, name);

    if (entry != NULL) {
        if (entry->line > 0) {
            Warn(check, line, "[%s] %s is also set on line %d; this later value wins", section, name, entry->line);
        }
        entry->line = line;
        entry->rejectedLine = 0;
        entry->note = NULL;
        return TRUE;
    }

    const SIM_ConfigCheckEntry *sameName = NULL, *otherCase = NULL;
    for (int i = 0; i < check->count; i++) {
        const SIM_ConfigCheckEntry *candidate = &check->entries[i];
        if (candidate->name == NULL) {
            continue;
        }
        if (strcmp(candidate->name, name) == 0) {
            sameName = candidate;
        } else if (otherCase == NULL && EqualIgnoringCase(candidate->name, name)) {
            otherCase = candidate;
        }
    }
    if (*section == '\0') {
        section = "no section";
    }
    if (sameName != NULL) {
        Warn(check, line, "%s is ignored: it is in [%s] but belongs in [%s]", name, section, sameName->section);
    } else if (otherCase != NULL) {
        Warn(check, line, "[%s] %s is ignored: names are case-sensitive, did you mean [%s] %s?", section, name,
            otherCase->section, otherCase->name);
    } else {
        Warn(check, line, "[%s] %s is ignored: this build has no such setting", section, name);
    }
    return FALSE;
}

void SIM_ConfigCheck_BadValue(SIM_ConfigCheck *check, const char *section, const char *name,
    const char *value, int line, const char *expected)
{
    SIM_ConfigCheckEntry *entry = FindEntry(check, section, name);

    Warn(check, line, "[%s] %s=%s is ignored: expected %s", section, name, value, expected);
    if (entry != NULL && entry->line == line) {
        entry->line = 0;
        entry->rejectedLine = line;
    }
}

void SIM_ConfigCheck_SetSource(SIM_ConfigCheck *check, const char *section, const char *name, int line,
    const char *note)
{
    SIM_ConfigCheckEntry *entry = FindEntry(check, section, name);

    if (entry != NULL) {
        entry->line = line;
        entry->rejectedLine = 0;
        entry->note = note;
    }
}

void SIM_ConfigCheck_ParseResult(SIM_ConfigCheck *check, int result, const char *consequence)
{
    check->fileFound = result != -1;
    if (result > 0) {
        Warn(check, result, "not a [section] or key=value line, so %s", consequence);
    } else if (result < -1) {
        Warn(check, 0, "out of memory while reading");
    }
}

typedef struct {
    const SIM_ConfigCheck *check;
    FILE *out;
    char section[64];
} PrintContext;

static int PrintKey(void *user, const char *section, const char *name, const char *value, int line)
{
    PrintContext *context = user;
    const SIM_ConfigCheckEntry *entry = FindEntry(context->check, section, name);
    const char *path = context->check->path;
    char source[160];

    if (strcmp(section, context->section) != 0) {
        snprintf(context->section, sizeof(context->section), "%s", section);
        fprintf(context->out, "[%s]\n", section);
    }
    // Paths are printed without their "./" prefix
    if (strncmp(path, "./", 2) == 0) {
        path += 2;
    }
    if (entry != NULL && entry->line > 0) {
        if (entry->note != NULL) {
            snprintf(source, sizeof(source), "%s:%d (%s)", path, entry->line, entry->note);
        } else {
            snprintf(source, sizeof(source), "%s:%d", path, entry->line);
        }
    } else if (entry != NULL && entry->rejectedLine > 0) {
        snprintf(source, sizeof(source), "default (%s:%d has an invalid value)", path, entry->rejectedLine);
    } else if (!context->check->fileFound) {
        snprintf(source, sizeof(source), "default (no %s)", path);
    } else {
        snprintf(source, sizeof(source), "default");
    }

    int width = fprintf(context->out, "%s=%s", name, value);
    fprintf(context->out, "%*s ; %s\n", width < 40 ? 40 - width : 0, "", source);
    return 1;
}

void SIM_ConfigCheck_Print(const SIM_ConfigCheck *check, const char *writerText, FILE *out)
{
    PrintContext context = { .check = check, .out = out };

    ini_parse_string(writerText, PrintKey, &context);
}

int SIM_ConfigCheck_WarningCount(void)
{
    return sWarningCount;
}

void SIM_ConfigCheck_SetPrintOnly(BOOL printOnly)
{
    sPrintOnly = printOnly;
}

BOOL SIM_ConfigCheck_PrintOnly(void)
{
    return sPrintOnly;
}
