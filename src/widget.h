// HW Widget: CPU/GPU temperature and RAM/VRAM load on a small card on the desktop.
// One thread and one layered window drawn in software; the sensors are read every five seconds.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define APP_CLASS L"HwWidget"
#define IDR_PAWNIO_AMD 101 // RCDATA in hwwidget.rc

// GetProcAddress for a function of a known type.
#define PROC(type, module, name) ((type)(void (*)(void))GetProcAddress(module, name))

enum { TIMER_POLL = 1, TIMER_SETTLE, TIMER_TRIM };

// ---- settings.c: %LOCALAPPDATA%\HwWidget\settings.json and error.log ----

enum { LAYOUT_ROW, LAYOUT_GRID, LAYOUT_COLUMN };

#define SCREEN_NAME_SIZE 128

typedef struct {
    bool has_pos;                  // false until the widget has been placed
    int x, y;                      // window position, device pixels
    char screen[SCREEN_NAME_SIZE]; // the screen it is on (see identify_screen in main.c), "" when unknown
    int screen_left, screen_top;   // where that screen's top-left corner was when x, y were saved
    int layout;
    double scale;
    bool locked, topmost;
} Settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);
void log_error(const char *format, ...);

// ---- sensors.c ----

enum { CPU_TEMP_OK, CPU_TEMP_NO_DRIVER, CPU_TEMP_UNSUPPORTED };

typedef struct {
    float cpu_temp, gpu_temp;     // °C, NAN when unavailable
    float ram_load, vram_load;    // %, NAN when unavailable
    double ram_used, ram_total;   // bytes
    double vram_used, vram_total; // bytes, 0 when unavailable
} Reading;

void sensors_open(void);
void sensors_read(Reading *r);
void sensors_close(void);
const WCHAR *sensors_cpu_name(void);
const WCHAR *sensors_gpu_name(void); // NULL without a GPU
int sensors_cpu_status(void);

// ---- render.c: the card, the gauges and the tooltip, drawn in software ----

enum { GAUGE_CPU, GAUGE_GPU, GAUGE_RAM, GAUGE_VRAM, GAUGE_COUNT };

typedef struct {
    const WCHAR *label;
    bool percent; // "%" is drawn smaller; otherwise "°" hangs after the number
    float warn, hot;
    float value; // the number shown, already rounded; NAN without a reading
} Gauge;

typedef struct {
    float s;                    // device pixels per layout unit: DPI scale × widget scale
    int width, height;          // the window, px
    int margin;                 // room for the shadow around the card, px
    RECT card;                  // px, within the window
    int cols, rows;
    float cell, grid_x, grid_y; // the gauge cells, px
} Layout;

void layout_init(Layout *L, float s, int cols);
int layout_hit(const Layout *L, int x, int y); // the gauge at a window pixel, or -1
bool render_layout(const Layout *L); // the fonts for a layout
bool render_window(HWND hwnd, const POINT *pos, const Layout *L, const Gauge *gauges); // all of it
void render_gauges(HWND hwnd, const Layout *L, const Gauge *gauges, unsigned changed); // a bit per gauge
void render_tip(HWND tip, const WCHAR *text, POINT cursor, UINT dpi);
void render_free(void);
int format_int(WCHAR *out, long long value);

// ---- desktop.c: keeps the widget on the desktop layer ----

void desktop_attach(HWND hwnd, bool enabled);
void desktop_detach(void);
void desktop_enable(bool enabled);
void desktop_tick(void);
void desktop_filter(WINDOWPOS *pos);
