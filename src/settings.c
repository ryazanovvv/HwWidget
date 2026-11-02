// Preferences in %LOCALAPPDATA%\HwWidget\settings.json, in the format the .NET version wrote,
// and error.log next to them.
#include "widget.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// %LOCALAPPDATA%\HwWidget\<name>; creates the folder. False when the path does not fit.
static bool path_of(const WCHAR *name, WCHAR out[MAX_PATH])
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH);
    if (!n || n + 11 + wcslen(name) + 5 >= MAX_PATH) return false; // room for ".tmp" too
    wcscat(out, L"\\HwWidget");
    CreateDirectoryW(out, NULL);
    wcscat(out, L"\\");
    wcscat(out, name);
    return true;
}

// The text after "key": in a flat JSON object, or NULL.
static const char *value_of(const char *json, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = json; (p = strchr(p, '"')); p++) {
        if (strncmp(p + 1, key, n) != 0 || p[n + 1] != '"') continue;
        p += n + 2;
        p += strspn(p, " \t\r\n");
        if (*p != ':') return NULL;
        p++;
        return p + strspn(p, " \t\r\n");
    }
    return NULL;
}

void settings_load(Settings *s)
{
    *s = (Settings){ .layout = LAYOUT_ROW, .scale = 1.0 };
    WCHAR path[MAX_PATH];
    if (!path_of(L"settings.json", path)) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    char json[2048];
    DWORD got = 0;
    if (!ReadFile(file, json, sizeof json - 1, &got, NULL)) got = 0;
    CloseHandle(file);
    json[got] = 0;

    const char *x = value_of(json, "X"), *y = value_of(json, "Y"), *v;
    if (x && y && *x != 'n' && *y != 'n') {
        s->has_pos = true;
        s->x = (int)strtol(x, NULL, 10);
        s->y = (int)strtol(y, NULL, 10);
    }
    const char *screen = value_of(json, "Screen"), *left = value_of(json, "ScreenLeft"), *top = value_of(json, "ScreenTop");
    size_t n = screen && *screen == '"' ? strcspn(screen + 1, "\"") : sizeof s->screen;
    if (s->has_pos && n < sizeof s->screen && left && top) {
        memcpy(s->screen, screen + 1, n);
        s->screen_left = (int)strtol(left, NULL, 10);
        s->screen_top = (int)strtol(top, NULL, 10);
    }
    if ((v = value_of(json, "Layout")))
        s->layout = !strncmp(v, "\"grid\"", 6) ? LAYOUT_GRID : !strncmp(v, "\"column\"", 8) ? LAYOUT_COLUMN : LAYOUT_ROW;
    if ((v = value_of(json, "Scale"))) s->scale = strtod(v, NULL);
    if (!(s->scale > 0)) s->scale = 1.0;
    s->locked = (v = value_of(json, "Locked")) && !strncmp(v, "true", 4);
    s->topmost = (v = value_of(json, "Topmost")) && !strncmp(v, "true", 4);
}

// Written to a temporary file first, so a crash or a full disk never leaves half a file.
void settings_save(const Settings *s)
{
    static const char *const layouts[] = { "row", "grid", "column" };
    char x[16] = "null", y[16] = "null", screen[SCREEN_NAME_SIZE + 96] = "", scale[32], json[512];
    if (s->has_pos) {
        snprintf(x, sizeof x, "%d", s->x);
        snprintf(y, sizeof y, "%d", s->y);
    }
    if (s->has_pos && s->screen[0]) // the name holds nothing JSON would need escaped
        snprintf(screen, sizeof screen, "  \"Screen\": \"%s\",\r\n  \"ScreenLeft\": %d,\r\n  \"ScreenTop\": %d,\r\n",
                 s->screen, s->screen_left, s->screen_top);
    snprintf(scale, sizeof scale, "%.2f", s->scale); // "1.10" → "1.1", "1.00" → "1"
    char *end = scale + strlen(scale);
    while (end[-1] == '0') *--end = 0;
    if (end[-1] == '.') *--end = 0;
    int n = snprintf(json, sizeof json,
        "{\r\n  \"X\": %s,\r\n  \"Y\": %s,\r\n%s  \"Layout\": \"%s\",\r\n  \"Scale\": %s,\r\n"
        "  \"Locked\": %s,\r\n  \"Topmost\": %s\r\n}",
        x, y, screen, layouts[s->layout], scale, s->locked ? "true" : "false", s->topmost ? "true" : "false");

    WCHAR path[MAX_PATH], tmp[MAX_PATH];
    if (!path_of(L"settings.json", path)) return;
    wcscpy(tmp, path);
    wcscat(tmp, L".tmp");
    HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, json, (DWORD)n, &put, NULL) && put == (DWORD)n;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING))
        log_error("Cannot save the settings (error %lu)", GetLastError());
}

void log_error(const char *format, ...)
{
    WCHAR path[MAX_PATH];
    if (!path_of(L"error.log", path)) return;
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond);
    va_list args;
    va_start(args, format);
    int m = vsnprintf(line + n, sizeof line - n - 2, format, args);
    va_end(args);
    if (m > 0) n += m < (int)(sizeof line - n - 2) ? m : (int)(sizeof line - n - 3);
    line[n++] = '\r';
    line[n++] = '\n';

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 256 * 1024) { // start over rather than grow forever
        CloseHandle(file);
        file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD put;
    WriteFile(file, line, (DWORD)n, &put, NULL);
    CloseHandle(file);
}
