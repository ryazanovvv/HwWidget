// Keeps the widget on the desktop layer like a gadget: under every normal window, even after a click,
// and lifted above the desktop whenever the desktop itself is what's on screen ("Show desktop" / Win+D
// raises the desktop window over everything, so without this the widget would vanish under it).
#include "widget.h"
#include <dwmapi.h>

static HWND window;
static HWINEVENTHOOK foreground_hook, minimize_hook;
static int checks_left;
static bool enabled, raised;

static void lift(void)
{
    raised = true;
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void sink(void)
{
    raised = false;
    SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool is_cloaked(HWND hwnd)
{
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked;
}

// True when the first on-screen window from the top of the z-order is the desktop itself:
// after "Show desktop", or with every window minimized.
static bool desktop_on_top(void)
{
    DWORD self = GetCurrentProcessId();
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h) || is_cloaked(h)) continue;
        // Checked before the tool-window filter: the desktop window is a tool window itself.
        WCHAR name[16];
        if (GetClassNameW(h, name, ARRAYSIZE(name)) && (!wcscmp(name, L"Progman") || !wcscmp(name, L"WorkerW")))
            return true;
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) continue;
        DWORD process = 0;
        GetWindowThreadProcessId(h, &process);
        if (process == self) continue;
        RECT r;
        if (GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top) return false;
    }
    return false;
}

static void update(void)
{
    if (!enabled) return;
    bool shown = desktop_on_top();
    if (shown && !raised) lift();
    else if (!shown && raised) sink();
}

// Events arrive mid-change (Win+D minimizes first and raises the desktop only after the
// animation), so the z-order is checked a few times during the second that follows.
static void schedule_checks(void)
{
    checks_left = 4;
    SetTimer(window, TIMER_SETTLE, 250, NULL);
}

static void CALLBACK on_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread,
                              DWORD time)
{
    (void)hook, (void)event, (void)hwnd, (void)object, (void)child, (void)thread, (void)time;
    schedule_checks();
}

void desktop_tick(void)
{
    if (--checks_left <= 0) KillTimer(window, TIMER_SETTLE);
    update();
}

void desktop_attach(HWND hwnd, bool on)
{
    window = hwnd;
    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, on_event, 0, 0, flags);
    minimize_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, on_event, 0, 0, flags);
    schedule_checks();
    desktop_enable(on);
}

void desktop_detach(void)
{
    KillTimer(window, TIMER_SETTLE);
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    if (minimize_hook) UnhookWinEvent(minimize_hook);
    foreground_hook = minimize_hook = NULL;
}

// Off while the user wants the widget above all windows.
void desktop_enable(bool on)
{
    enabled = on;
    if (on) {
        sink();
        update();
    } else {
        raised = false;
    }
}

// Whatever tries to bring the widget forward (a click, activation) sends it to the bottom instead.
void desktop_filter(WINDOWPOS *pos)
{
    if (enabled && !raised && !(pos->flags & SWP_NOZORDER) && pos->hwndInsertAfter != HWND_BOTTOM)
        pos->hwndInsertAfter = HWND_BOTTOM;
}
