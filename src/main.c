// The entry point and the widget window: input, menu, placement, tooltip and the polling timer.
#include "widget.h"
#include <imm.h>
#include <stdlib.h>
#include <windowsx.h>

#define QUIT_MESSAGE L"HwWidget.Quit"
#define INSTANCE_NAME L"Local\\HwWidget.Instance"
#define TIP_CLASS L"HwWidget.Tip"
#define MIN_SCALE 0.7
#define MAX_SCALE 2.0
#define SCALE_STEP 0.1
#define GAP 12.0f // between the card and a screen edge it snaps to, DIPs
#define SNAP_ZONE 28.0f
#define POLL_MS 5000
#define CPU_SMOOTHING 0.5f // the weight of a new CPU reading in the running average
#define TRIM_DELAY_MS 15000

enum { CMD_ROW = 1, CMD_GRID, CMD_COLUMN, CMD_BIGGER, CMD_SMALLER, CMD_LOCK, CMD_TOPMOST, CMD_CLOSE };

// GUID_CONSOLE_DISPLAY_STATE: the screen going off, dimmed or on.
static const GUID DISPLAY_STATE = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };

static HWND g_hwnd, g_tip;
static UINT g_quit_message, g_dpi = 96;
static Settings g_settings;
static Layout g_layout;
static Reading g_reading;
static float g_cpu_average = NAN;
static Gauge g_gauges[GAUGE_COUNT] = {
    { L"CPU", false, 75, 90, NAN },
    { L"GPU", false, 75, 85, NAN },
    { L"RAM", true, 75, 90, NAN },
    { L"VRAM", true, 75, 90, NAN },
};
static HPOWERNOTIFY g_display_notify;
static bool g_polling;
static bool g_restore_pending; // the saved spot is on a screen that is missing: still waking up, or unplugged
static bool g_dragging;
static POINT g_grab; // the cursor's offset from the window corner while dragging
static int g_wheel;  // wheel movement short of a whole notch
static bool g_tracking;
static int g_hover = -1, g_tip_gauge = -1;
static POINT g_tip_cursor;
static WCHAR g_tip_text[256], g_decimal[8] = L".";

// ---- Start-up ----

// The command line after the program's own path.
static const WCHAR *arguments(void)
{
    const WCHAR *p = GetCommandLineW();
    if (*p == L'"') {
        for (p++; *p && *p != L'"'; p++) {}
        if (*p) p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t') p++;
    }
    return p;
}

static bool has_arg(const WCHAR *args, const WCHAR *name)
{
    size_t n = wcslen(name);
    for (const WCHAR *p = args; (p = wcsstr(p, name)); p += n)
        if ((p == args || p[-1] == L' ' || p[-1] == L'\t') && (!p[n] || p[n] == L' ' || p[n] == L'\t')) return true;
    return false;
}

// Asks the running widget to close; its window lets this message through although it runs elevated.
static void quit_running(void)
{
    for (HWND h = NULL; (h = FindWindowExW(NULL, h, APP_CLASS, NULL));) PostMessageW(h, g_quit_message, 0, 0);
}

static bool is_running(void)
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, INSTANCE_NAME);
    if (m) CloseHandle(m);
    return m || GetLastError() == ERROR_ACCESS_DENIED; // denied: held by the elevated instance
}

static bool is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION elevation = { 0 };
    DWORD size;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size);
    CloseHandle(token);
    return elevation.TokenIsElevated != 0;
}

// CPU temperature needs admin rights; the logon task grants them without a UAC prompt.
static bool start_task(void)
{
    WCHAR exe[MAX_PATH], cmd[] = L"schtasks.exe /run /tn HwWidget";
    UINT n = GetSystemDirectoryW(exe, MAX_PATH - 16);
    if (!n || n >= MAX_PATH - 16) return false;
    wcscat(exe, L"\\schtasks.exe");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    DWORD code = 1;
    bool ok = WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code) &&
              code == 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

// The widget never needs speed: below-normal priority, and EcoQoS lets Windows keep it on
// efficient cores at a low clock.
static void tune_process(void)
{
    typedef BOOL(WINAPI * SetInformation)(HANDLE process, int type, void *info, DWORD size);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetInformation set = PROC(SetInformation, GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    struct {
        ULONG version, control_mask, state_mask;
    } throttling = { 1, 1, 1 }; // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    if (set) set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &throttling, sizeof throttling);
}

// Start-up, a new layout and the menu touch pages that are then left alone; a while later they go
// back to Windows.
static void trim_later(void) { SetTimer(g_hwnd, TIMER_TRIM, TRIM_DELAY_MS, NULL); }

// ---- Tooltip ----

typedef struct {
    WCHAR *p;
    size_t len, cap;
} Text;

static void put(Text *t, const WCHAR *s)
{
    while (*s && t->len + 1 < t->cap) t->p[t->len++] = *s++;
    t->p[t->len] = 0;
}

// Gigabytes with one decimal, in the user's own notation: "12,3".
static void put_gb(Text *t, double bytes)
{
    long long tenths = llround(bytes / (1024.0 * 1024 * 1024) * 10);
    WCHAR number[24];
    format_int(number, tenths / 10);
    put(t, number);
    put(t, g_decimal);
    format_int(number, tenths % 10);
    put(t, number);
}

static void put_usage(Text *t, double used, double total)
{
    put(t, L"Занято ");
    put_gb(t, used);
    put(t, L" из ");
    put_gb(t, total);
    put(t, L" ГБ");
}

static void tip_text(int gauge, WCHAR *out, size_t cap)
{
    Text t = { out, 0, cap };
    out[0] = 0;
    switch (gauge) {
    case GAUGE_CPU:
        put(&t, sensors_cpu_name());
        if (!isnan(g_reading.cpu_temp)) break;
        put(&t, sensors_cpu_status() == CPU_TEMP_UNSUPPORTED ? L"\nТемпература недоступна: этот процессор не поддерживается"
                : sensors_cpu_status() == CPU_TEMP_NO_DRIVER
                    ? L"\nТемпература недоступна: нужен драйвер PawnIO и запуск с правами администратора"
                    : L"\nТемпература недоступна");
        break;
    case GAUGE_GPU:
        put(&t, sensors_gpu_name() ? sensors_gpu_name() : L"Видеокарта не найдена");
        break;
    case GAUGE_RAM:
        put_usage(&t, g_reading.ram_used, g_reading.ram_total);
        break;
    case GAUGE_VRAM:
        if (g_reading.vram_total > 0) put_usage(&t, g_reading.vram_used, g_reading.vram_total);
        else put(&t, L"Нет данных");
        break;
    }
}

static void show_tip(int gauge)
{
    if (!g_tip)
        g_tip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                TIP_CLASS, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_tip) return;
    GetCursorPos(&g_tip_cursor);
    tip_text(gauge, g_tip_text, ARRAYSIZE(g_tip_text));
    render_tip(g_tip, g_tip_text, g_tip_cursor, g_dpi);
    ShowWindow(g_tip, SW_SHOWNOACTIVATE);
    g_tip_gauge = gauge;
}

static void hide_tip(void)
{
    if (g_tip_gauge < 0) return;
    ShowWindow(g_tip, SW_HIDE);
    g_tip_gauge = -1;
}

// Keeps an open tooltip in step with the readings.
static void refresh_tip(void)
{
    if (g_tip_gauge < 0) return;
    WCHAR text[ARRAYSIZE(g_tip_text)];
    tip_text(g_tip_gauge, text, ARRAYSIZE(text));
    if (wcscmp(text, g_tip_text) == 0) return;
    wcscpy(g_tip_text, text);
    render_tip(g_tip, g_tip_text, g_tip_cursor, g_dpi);
}

// ---- Readings ----

static bool same(float a, float b) { return a == b || (isnan(a) && isnan(b)); }

// The CPU's own sensor jumps with every short burst of load; its gauge follows a running average.
static float smooth_cpu(float t)
{
    g_cpu_average = isnan(t) || isnan(g_cpu_average) ? t : g_cpu_average + (t - g_cpu_average) * CPU_SMOOTHING;
    return g_cpu_average;
}

// The number a gauge shows: the reading rounded (to even, as Math.Round did), but held until the
// reading is a quarter past the halfway mark, so one that hovers there does not flip back and forth.
static float shown(float now, float reading)
{
    return isnan(now) || isnan(reading) || fabsf(reading - now) >= 0.75f ? rintf(reading) : now;
}

// A gauge is redrawn only when the number it shows changes, and only its own cell reaches the screen.
static void poll(bool draw)
{
    sensors_read(&g_reading);
    const float values[GAUGE_COUNT] = { smooth_cpu(g_reading.cpu_temp), g_reading.gpu_temp, g_reading.ram_load,
                                        g_reading.vram_load };
    unsigned changed = 0;
    for (int i = 0; i < GAUGE_COUNT; i++) {
        float value = shown(g_gauges[i].value, values[i]);
        if (same(value, g_gauges[i].value)) continue;
        g_gauges[i].value = value;
        changed |= 1u << i;
    }
    if (draw && changed) render_gauges(g_hwnd, &g_layout, g_gauges, changed);
    refresh_tip();
}

// Nobody sees a dark screen: no polling while it is off.
static void set_polling(bool on)
{
    if (on == g_polling) return;
    g_polling = on;
    if (on) {
        g_cpu_average = NAN; // a fresh start, not an average with the time before the screen went off
        poll(true);
        SetCoalescableTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL, POLL_MS / 4);
    } else {
        KillTimer(g_hwnd, TIMER_POLL);
    }
}

// ---- Placement, in device pixels ----

static int px(float dips) { return (int)lrintf(dips * (float)g_dpi / 96); }

// The visible card: the window minus its transparent shadow margin.
static RECT card_rect(void)
{
    RECT r;
    GetWindowRect(g_hwnd, &r);
    int m = g_layout.margin;
    return (RECT){ r.left + m, r.top + m, r.right - m, r.bottom - m };
}

static RECT work_area_at(RECT r)
{
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

static bool is_on_screen(int x, int y)
{
    RECT probe = { x, y, x + 120, y + 80 };
    return MonitorFromRect(&probe, MONITOR_DEFAULTTONULL) != NULL;
}

static void move_by(int dx, int dy)
{
    if (!dx && !dy) return;
    RECT r;
    GetWindowRect(g_hwnd, &r);
    SetWindowPos(g_hwnd, NULL, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void move_to(POINT spot)
{
    RECT r;
    GetWindowRect(g_hwnd, &r);
    move_by(spot.x - r.left, spot.y - r.top);
}

// First run: the top-right corner of the main screen.
static void place_default(void)
{
    RECT area = work_area_at((RECT){ 0, 0, 1, 1 }), card = card_rect(); // the main screen holds (0, 0)
    move_by(area.right - px(GAP) - card.right, area.top + px(GAP) - card.top);
}

// Close to a screen edge, the card settles exactly GAP away from it.
static void snap_to_edges(void)
{
    RECT card = card_rect(), area = work_area_at(card);
    int gap = px(GAP), zone = px(SNAP_ZONE), dx = 0, dy = 0;
    if (abs(card.left - (area.left + gap)) < zone) dx = area.left + gap - card.left;
    else if (abs(card.right - (area.right - gap)) < zone) dx = area.right - gap - card.right;
    if (abs(card.top - (area.top + gap)) < zone) dy = area.top + gap - card.top;
    else if (abs(card.bottom - (area.bottom - gap)) < zone) dy = area.bottom - gap - card.bottom;
    move_by(dx, dy);
}

// After a resize or a display change, pulls the card back onto its screen.
static void keep_inside(void)
{
    RECT card = card_rect(), area = work_area_at(card);
    int dx = card.right > area.right ? area.right - card.right : 0;
    if (card.left + dx < area.left) dx = area.left - card.left;
    int dy = card.bottom > area.bottom ? area.bottom - card.bottom : 0;
    if (card.top + dy < area.top) dy = area.top - card.top;
    move_by(dx, dy);
}

// ---- The widget's own screen ----

// A screen's own name, which it keeps when unplugged and plugged back into the same port:
// "DISPLAY#GSM5B7F#5&2b4ef27c&0&UID4352" out of its device path "\\?\DISPLAY#...#{guid}". Also where
// the screen is now.
static bool identify_screen(HMONITOR monitor, char name[SCREEN_NAME_SIZE], RECT *rc)
{
    MONITORINFOEXW mi;
    DISPLAY_DEVICEW device = { .cb = sizeof device };
    mi.cbSize = sizeof mi;
    name[0] = 0;
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&mi) ||
        !EnumDisplayDevicesW(mi.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME))
        return false;
    const WCHAR *p = device.DeviceID;
    if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    size_t n = 0;
    for (; *p && *p != L'{' && n + 1 < SCREEN_NAME_SIZE; p++)
        name[n++] = *p < 0x80 && *p != L'"' && *p != L'\\' ? (char)*p : '_'; // nothing to escape in JSON
    while (n && name[n - 1] == '#') n--;
    name[n] = 0;
    *rc = mi.rcMonitor;
    return n > 0;
}

typedef struct {
    const char *name;
    RECT rc;
    bool found;
} ScreenSearch;

static BOOL CALLBACK match_screen(HMONITOR monitor, HDC dc, LPRECT clip, LPARAM data)
{
    (void)dc, (void)clip;
    ScreenSearch *s = (ScreenSearch *)data;
    char name[SCREEN_NAME_SIZE];
    RECT rc;
    if (!identify_screen(monitor, name, &rc) || strcmp(name, s->name)) return TRUE;
    s->rc = rc;
    s->found = true;
    return FALSE;
}

// Where the saved spot is now: on its own screen, wherever that screen sits among the others. False
// while the screen is missing: right after logon it may still be waking up, or it was unplugged.
static bool home_spot(POINT *spot)
{
    if (!g_settings.has_pos) return false;
    *spot = (POINT){ g_settings.x, g_settings.y };
    if (!g_settings.screen[0]) return is_on_screen(spot->x, spot->y); // saved before screens had names
    ScreenSearch s = { g_settings.screen };
    EnumDisplayMonitors(NULL, NULL, match_screen, (LPARAM)&s);
    if (!s.found) return false;
    spot->x += s.rc.left - g_settings.screen_left;
    spot->y += s.rc.top - g_settings.screen_top;
    return true;
}

static void save_position(void)
{
    if (!g_restore_pending) { // otherwise keep the spot on the screen that is missing
        RECT r = card_rect(), screen;
        if (identify_screen(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), g_settings.screen, &screen)) {
            g_settings.screen_left = screen.left;
            g_settings.screen_top = screen.top;
        }
        GetWindowRect(g_hwnd, &r);
        g_settings.has_pos = true;
        g_settings.x = r.left;
        g_settings.y = r.top;
    }
    settings_save(&g_settings);
}

// Coming from a screen with another DPI, the window is resized around a spot of Windows' choosing,
// so it is put on its own spot once more; nothing is saved in between.
static void go_home(POINT spot)
{
    g_restore_pending = true;
    move_to(spot);
    move_to(spot);
    g_restore_pending = false;
    keep_inside();
}

// While its screen is missing, the widget waits at the closest spot on the screens there are. The saved
// spot stays its home, unless the user drags the widget elsewhere.
static void wait_near_home(void)
{
    g_restore_pending = true;
    move_to((POINT){ g_settings.x, g_settings.y });
    keep_inside();
    snap_to_edges();
}

// The saved spot, or the default corner on the first run.
static void place(void)
{
    POINT home;
    if (home_spot(&home)) go_home(home);
    else if (g_settings.has_pos) wait_near_home();
    else place_default();
    save_position();
}

static void on_screens_changed(void)
{
    POINT home;
    if (home_spot(&home)) go_home(home);
    else if (g_settings.has_pos) wait_near_home();
    else keep_inside();
}

// ---- Layout and scale ----

static void relayout(const POINT *pos)
{
    int cols = g_settings.layout == LAYOUT_GRID ? 2 : g_settings.layout == LAYOUT_COLUMN ? 1 : 4;
    layout_init(&g_layout, (float)g_dpi / 96 * (float)g_settings.scale, cols);
    if (!render_layout(&g_layout) || !render_window(g_hwnd, pos, &g_layout, g_gauges))
        log_error("Cannot allocate a %dx%d bitmap", g_layout.width, g_layout.height);
    trim_later();
}

// The window grows or shrinks from its top-left corner, or from the screen edges the card is
// docked to, so a card in the top-right corner stays there.
static void resize(void)
{
    hide_tip();
    RECT card = card_rect(), area = work_area_at(card), r;
    bool right = card.right == area.right - px(GAP), bottom = card.bottom == area.bottom - px(GAP);
    GetWindowRect(g_hwnd, &r);
    relayout(&(POINT){ r.left, r.top });
    RECT now = card_rect();
    move_by(right ? card.right - now.right : 0, bottom ? card.bottom - now.bottom : 0);
    keep_inside();
    snap_to_edges();
    save_position();
}

static void change_scale(int steps)
{
    double scale = nearbyint((g_settings.scale + steps * SCALE_STEP) * 100) / 100;
    scale = scale < MIN_SCALE ? MIN_SCALE : scale > MAX_SCALE ? MAX_SCALE : scale;
    if (scale == g_settings.scale) return;
    g_settings.scale = scale;
    resize();
}

static void on_dpi_changed(UINT dpi, const RECT *suggested)
{
    RECT before;
    GetWindowRect(g_hwnd, &before);
    g_dpi = dpi;
    hide_tip();
    relayout(&(POINT){ suggested->left, suggested->top });
    if (g_dragging) { // keep the same spot of the card under the cursor; the drop settles
        g_grab.x = g_grab.x * g_layout.width / (before.right > before.left ? before.right - before.left : 1);
        g_grab.y = g_grab.y * g_layout.height / (before.bottom > before.top ? before.bottom - before.top : 1);
        return;
    }
    POINT home;
    if (g_settings.has_pos && !home_spot(&home)) g_restore_pending = true; // Windows moved it off a screen that went away
    keep_inside();
    snap_to_edges();
    save_position();
}

// ---- Mouse: drag to move, Ctrl + wheel to resize, hover for details ----

static void start_drag(void)
{
    POINT cursor;
    RECT r;
    GetCursorPos(&cursor);
    GetWindowRect(g_hwnd, &r);
    g_grab = (POINT){ cursor.x - r.left, cursor.y - r.top };
    g_dragging = true;
    SetCapture(g_hwnd);
}

static void end_drag(void)
{
    g_dragging = false;
    g_restore_pending = false; // a new spot, picked by the user
    snap_to_edges();
    save_position();
}

static void on_mouse_move(int x, int y)
{
    if (g_dragging) {
        POINT cursor;
        GetCursorPos(&cursor);
        SetWindowPos(g_hwnd, NULL, cursor.x - g_grab.x, cursor.y - g_grab.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    if (!g_tracking) {
        TRACKMOUSEEVENT leave = { sizeof leave, TME_LEAVE, g_hwnd, 0 };
        g_tracking = TrackMouseEvent(&leave);
    }
    int gauge = layout_hit(&g_layout, x, y);
    if (gauge == g_hover) return;
    g_hover = gauge;
    hide_tip();
    if (gauge >= 0) { // the tooltip comes once the cursor rests on the gauge
        TRACKMOUSEEVENT hover = { sizeof hover, TME_HOVER | TME_LEAVE, g_hwnd, HOVER_DEFAULT };
        TrackMouseEvent(&hover);
    }
}

static void on_wheel(int delta)
{
    g_wheel += delta;
    int steps = g_wheel / WHEEL_DELTA;
    g_wheel -= steps * WHEEL_DELTA;
    if (steps) change_scale(steps);
}

// ---- Context menu ----

// Native menus in the dark theme, to match the card. uxtheme is loaded only when first needed.
static void dark_menus(void)
{
    static bool done;
    if (done) return;
    done = true;
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    typedef int(WINAPI * SetPreferredAppMode)(int mode);
    typedef void(WINAPI * FlushMenuThemes)(void);
    SetPreferredAppMode set_mode = PROC(SetPreferredAppMode, uxtheme, MAKEINTRESOURCEA(135));
    FlushMenuThemes flush = PROC(FlushMenuThemes, uxtheme, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(2); // ForceDark
    if (flush) flush();
}

static void set_topmost(bool on)
{
    g_settings.topmost = on;
    // The desktop layer lets go before the window goes topmost, and takes over only after it stops.
    if (on) {
        desktop_enable(false);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        desktop_enable(true);
    }
}

static void show_menu(LPARAM at)
{
    POINT pt = { GET_X_LPARAM(at), GET_Y_LPARAM(at) };
    if (at == -1) GetCursorPos(&pt);
    hide_tip();
    dark_menus();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, CMD_ROW, L"В строку");
    AppendMenuW(menu, MF_STRING, CMD_GRID, L"Квадрат 2 × 2");
    AppendMenuW(menu, MF_STRING, CMD_COLUMN, L"В столбец");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_settings.scale >= MAX_SCALE ? MF_GRAYED : 0), CMD_BIGGER, L"Крупнее");
    AppendMenuW(menu, MF_STRING | (g_settings.scale <= MIN_SCALE ? MF_GRAYED : 0), CMD_SMALLER, L"Мельче");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_settings.locked ? MF_CHECKED : 0), CMD_LOCK, L"Закрепить на месте");
    AppendMenuW(menu, MF_STRING | (g_settings.topmost ? MF_CHECKED : 0), CMD_TOPMOST, L"Поверх всех окон");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_CLOSE, L"Закрыть");
    CheckMenuRadioItem(menu, CMD_ROW, CMD_COLUMN, CMD_ROW + g_settings.layout, MF_BYCOMMAND);

    // Without the foreground the menu would not close on a click elsewhere.
    SetForegroundWindow(g_hwnd);
    int cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    trim_later();

    switch (cmd) {
    case CMD_ROW:
    case CMD_GRID:
    case CMD_COLUMN:
        g_settings.layout = cmd - CMD_ROW;
        resize();
        break;
    case CMD_BIGGER:
        change_scale(1);
        break;
    case CMD_SMALLER:
        change_scale(-1);
        break;
    case CMD_LOCK:
        g_settings.locked = !g_settings.locked;
        settings_save(&g_settings);
        break;
    case CMD_TOPMOST:
        set_topmost(!g_settings.topmost);
        settings_save(&g_settings);
        break;
    case CMD_CLOSE:
        DestroyWindow(g_hwnd);
        break;
    }
}

// ---- Window ----

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_quit_message && msg) {
        DestroyWindow(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_POLL) {
            poll(true);
        } else if (wp == TIMER_SETTLE) {
            desktop_tick();
        } else if (wp == TIMER_TRIM) { // see trim_later
            KillTimer(hwnd, TIMER_TRIM);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        hide_tip();
        if (!g_settings.locked) start_drag();
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) ReleaseCapture(); // WM_CAPTURECHANGED ends the drag
        if (g_dragging) end_drag();       // unless the capture was never taken
        return 0;
    case WM_CAPTURECHANGED:
        if (g_dragging) end_drag();
        return 0;
    case WM_MOUSEMOVE:
        on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEHOVER:
        if (g_hover >= 0 && !g_dragging) show_tip(g_hover);
        return 0;
    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hover = -1;
        hide_tip();
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_RBUTTONDOWN:
        hide_tip();
        return 0;
    case WM_CONTEXTMENU:
        show_menu(lp);
        return 0;
    case WM_WINDOWPOSCHANGING:
        desktop_filter((WINDOWPOS *)lp);
        break;
    case WM_DPICHANGED:
        on_dpi_changed(HIWORD(wp), (const RECT *)lp);
        return 0;
    case WM_DISPLAYCHANGE:
        on_screens_changed();
        break;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) on_screens_changed();
        break;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            const POWERBROADCAST_SETTING *setting = (const POWERBROADCAST_SETTING *)lp;
            if (!memcmp(&setting->PowerSetting, &DISPLAY_STATE, sizeof DISPLAY_STATE) && setting->DataLength >= 4)
                set_polling(*(const DWORD *)setting->Data != 0); // 0 off, 1 on, 2 dimmed
        }
        return TRUE;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        KillTimer(hwnd, TIMER_TRIM);
        desktop_detach();
        if (g_display_notify) UnregisterPowerSettingNotification(g_display_notify);
        if (g_tip) DestroyWindow(g_tip);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd_line, int show)
{
    (void)prev, (void)cmd_line, (void)show;
    const WCHAR *args = arguments();
    g_quit_message = RegisterWindowMessageW(QUIT_MESSAGE);
    if (has_arg(args, L"--quit")) {
        quit_running();
        return 0;
    }
    if (is_running()) return 0;
    if (!is_elevated() && !has_arg(args, L"--from-task") && !has_arg(args, L"--no-task") && start_task()) return 0;
    HANDLE single = CreateMutexW(NULL, TRUE, INSTANCE_NAME);
    if (!single || GetLastError() == ERROR_ALREADY_EXISTS) return 0; // lost a start-up race

    tune_process();
    ImmDisableIME((DWORD)-1); // nothing to type into: keeps the IME and text services out of the process
    settings_load(&g_settings);
    g_settings.scale = g_settings.scale < MIN_SCALE ? MIN_SCALE : g_settings.scale > MAX_SCALE ? MAX_SCALE : g_settings.scale;
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, g_decimal, ARRAYSIZE(g_decimal)))
        wcscpy(g_decimal, L".");
    sensors_open();
    poll(false); // the first frame already shows the readings

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = TIP_CLASS;
    RegisterClassExW(&wc);

    // Created on the saved spot, so the first layout already has its screen's DPI.
    POINT home;
    bool saved = home_spot(&home);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (g_settings.topmost ? WS_EX_TOPMOST : 0),
                             APP_CLASS, L"HW Widget", WS_POPUP, saved ? home.x : 0, saved ? home.y : 0, 1, 1,
                             NULL, NULL, instance, NULL);
    if (!g_hwnd) return 1;
    // Let "HwWidget.exe --quit" reach this window even though it runs elevated.
    ChangeWindowMessageFilterEx(g_hwnd, g_quit_message, MSGFLT_ALLOW, NULL);
    g_dpi = GetDpiForWindow(g_hwnd);
    relayout(NULL);
    place();
    desktop_attach(g_hwnd, !g_settings.topmost);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    g_polling = true;
    SetCoalescableTimer(g_hwnd, TIMER_POLL, POLL_MS, NULL, POLL_MS / 4);
    g_display_notify = RegisterPowerSettingNotification(g_hwnd, &DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    sensors_close();
    render_free();
    CloseHandle(single);
    return 0;
}
