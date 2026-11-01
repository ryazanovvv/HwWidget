// Software rendering into premultiplied BGRA bitmaps, shown with UpdateLayeredWindowIndirect.
// Shapes are anti-aliased from signed distances; text comes from GDI, drawn white on black (on the
// gauges at several times the size, then averaged down), and its brightness serves as coverage for
// the real colour. The window keeps its own copy of the picture, so no bitmap outlives an update.
// Windows takes even a partial update only from a bitmap the size of the window (a smaller one is
// silently ignored), but a changed gauge touches no more of it than its own cell.
#include "widget.h"

// Colours as 0xAARRGGBB and sizes in layout units (DIPs at 100 % scale), from the WPF original.
#define BASE 0xFF141518u // the card under its edge
#define EDGE 0x1CFFFFFFu
#define FILL_TOP 0xFF26282Du
#define FILL_END 0xFF1B1D21u
#define PRIMARY 0xF0FFFFFFu
#define SECONDARY 0x8CFFFFFFu
#define TRACK 0x22FFFFFFu
#define NORMAL 0xFF4CC2FFu
#define WARM 0xFFFFB938u
#define HOT 0xFFFF5F57u
#define TIP_FILL 0xF2232529u
#define TIP_EDGE 0x2EFFFFFFu
#define TIP_TEXT 0xFFEDEDEDu

#define CELL 74.0f    // a gauge and its margin
#define PADDING 11.0f // the card's edge and padding around the cells
#define DIAMETER 64.0f
#define THICKNESS 5.0f
#define RADIUS ((DIAMETER - THICKNESS) / 2)
#define START 135.0f // degrees, clockwise from three o'clock
#define FULL_SWEEP 270.0f
#define CORNER 18.0f
#define SHADOW_MARGIN 20.0f
#define SHADOW_BLUR 18.0f // WPF's BlurRadius: a Gaussian with sigma = radius / 3
#define SHADOW_DEPTH 2.0f
#define SHADOW_OPACITY 0.35f
#define VALUE_SIZE 19.0f
#define UNIT_SIZE 12.0f
#define CAPTION_SIZE 9.5f
#define TIP_SIZE 12.0f
#define TIP_CORNER 6.0f
#define DEG 0.0174532925f

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    uint32_t *bits;
    int w, h;
} Canvas;

typedef struct {
    HFONT handle;               // SUPERSAMPLE times the size of the text it draws
    float ascent, descent, cap; // window pixels
} Font;

static HDC measure;    // for text metrics; it has no bitmap of its own
static Canvas scratch; // the enlarged text, shared by the runs of one update
static Font value_font, unit_font, caption_font;
static HFONT tip_font;
static UINT tip_font_dpi;

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// Source-over of a colour, at the given coverage, onto a premultiplied pixel.
static inline void blend(uint32_t *p, uint32_t argb, float coverage)
{
    float a = coverage * (float)(argb >> 24) / 255;
    if (a <= 0) return;
    uint32_t d = *p;
    float k = 1 - a;
    uint32_t r = (uint32_t)(((argb >> 16) & 255) * a + ((d >> 16) & 255) * k + 0.5f);
    uint32_t g = (uint32_t)(((argb >> 8) & 255) * a + ((d >> 8) & 255) * k + 0.5f);
    uint32_t b = (uint32_t)((argb & 255) * a + (d & 255) * k + 0.5f);
    uint32_t alpha = (uint32_t)(255 * a + (d >> 24) * k + 0.5f);
    *p = alpha << 24 | r << 16 | g << 8 | b;
}

static uint32_t mix(uint32_t from, uint32_t to, float t)
{
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        float a = (float)((from >> shift) & 255), b = (float)((to >> shift) & 255);
        out |= (uint32_t)(a + (b - a) * t + 0.5f) << shift;
    }
    return out;
}

// Coverage of the pixel centred at (x, y) by a rounded rectangle, from the signed distance.
static float rrect(float x, float y, float left, float top, float right, float bottom, float r)
{
    float qx = fabsf(x - (left + right) / 2) - ((right - left) / 2 - r);
    float qy = fabsf(y - (top + bottom) / 2) - ((bottom - top) / 2 - r);
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
    return clamp01(0.5f - d);
}

// GDI fits small text to whole pixels, which thins the captions and moves runs by up to half
// a pixel. Drawn this many times larger and averaged down, text keeps its true shapes and
// fractional positions, as WPF drew it.
#ifndef SUPERSAMPLE
#define SUPERSAMPLE 4
#endif

// Text is blended in linear light, as WPF blends it, so light letters on the dark card keep their
// weight. Text only lands on nearly opaque backgrounds; the clamp keeps the pixel premultiplied.
// Two tables stand in for powf: a channel's linear light, and the channel for a linear light given
// in steps of 1 / LINEAR_STEPS.
#ifndef TEXT_GAMMA
#define TEXT_GAMMA 1.8f
#endif
#define LINEAR_STEPS 4096
static float to_linear[256];
static uint8_t from_linear[LINEAR_STEPS + 1];

static void gamma_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < 256; i++) to_linear[i] = powf(i / 255.0f, TEXT_GAMMA);
    for (int i = 0; i <= LINEAR_STEPS; i++)
        from_linear[i] = (uint8_t)(255 * powf((float)i / LINEAR_STEPS, 1 / TEXT_GAMMA) + 0.5f);
}

static void blend_text(uint32_t *p, uint32_t argb, float coverage)
{
    float a = coverage * (float)(argb >> 24) / 255;
    if (a <= 0) return;
    uint32_t d = *p, alpha = (uint32_t)(255 * a + (d >> 24) * (1 - a) + 0.5f), out = alpha << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        float light = to_linear[(argb >> shift) & 255] * a + to_linear[(d >> shift) & 255] * (1 - a);
        uint32_t c = from_linear[(int)(light * LINEAR_STEPS + 0.5f)];
        out |= (c < alpha ? c : alpha) << shift;
    }
    *p = out;
}

// ClearType renders each colour channel on its own; their mean is a finer coverage than grayscale.
static inline float cleartype_coverage(uint32_t pixel)
{
    return (float)(((pixel >> 16) & 255) + ((pixel >> 8) & 255) + (pixel & 255)) / 765;
}

static void canvas_free(Canvas *c)
{
    if (c->dc && c->old) SelectObject(c->dc, c->old);
    if (c->bitmap) DeleteObject(c->bitmap);
    if (c->dc) DeleteDC(c->dc);
    memset(c, 0, sizeof *c);
}

static bool canvas_init(Canvas *c, int w, int h)
{
    canvas_free(c);
    BITMAPINFO bi = { .bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    void *bits = NULL;
    c->dc = CreateCompatibleDC(NULL);
    c->bitmap = c->dc ? CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!c->bitmap) {
        canvas_free(c);
        return false;
    }
    c->old = SelectObject(c->dc, c->bitmap);
    c->bits = bits;
    c->w = w;
    c->h = h;
    return true;
}

// ---- Layout ----

void layout_init(Layout *L, float s, int cols)
{
    L->s = s;
    L->cols = cols;
    L->rows = GAUGE_COUNT / cols;
    L->margin = (int)lrintf(SHADOW_MARGIN * s);
    int w = (int)lrintf((CELL * L->cols + 2 * PADDING) * s);
    int h = (int)lrintf((CELL * L->rows + 2 * PADDING) * s);
    L->card = (RECT){ L->margin, L->margin, L->margin + w, L->margin + h };
    L->width = w + 2 * L->margin;
    L->height = h + 2 * L->margin;
    L->cell = CELL * s;
    L->grid_x = L->grid_y = L->margin + PADDING * s;
}

// The gauge's own square reacts to the mouse, not the margin around it.
int layout_hit(const Layout *L, int x, int y)
{
    float u = (x + 0.5f - L->grid_x) / L->s, v = (y + 0.5f - L->grid_y) / L->s; // layout units
    if (u < 0 || v < 0) return -1;
    int col = (int)(u / CELL), row = (int)(v / CELL);
    if (col >= L->cols || row >= L->rows) return -1;
    float margin = (CELL - DIAMETER) / 2;
    u -= col * CELL;
    v -= row * CELL;
    if (u < margin || u > CELL - margin || v < margin || v > CELL - margin) return -1;
    return row * L->cols + col;
}

static RECT cell_rect(const Layout *L, int index)
{
    float x = L->grid_x + (float)(index % L->cols) * L->cell, y = L->grid_y + (float)(index / L->cols) * L->cell;
    return (RECT){ (LONG)floorf(x), (LONG)floorf(y), (LONG)ceilf(x + L->cell), (LONG)ceilf(y + L->cell) };
}

// ---- The card ----

// The edge is 1 unit, kept whole pixels wide so it stays crisp.
static float edge_width(const Layout *L) { return fmaxf(1, rintf(L->s)); }

// The card's top-to-bottom gradient at a pixel row.
static uint32_t fill_at(const Layout *L, float y)
{
    float b = edge_width(L), top = (float)L->card.top + b, bottom = (float)L->card.bottom - b;
    return mix(FILL_TOP, FILL_END, clamp01((y - top) / (bottom - top)));
}

// Box widths whose three passes approximate a Gaussian blur.
static void gauss_boxes(float sigma, int boxes[3])
{
    int lo = (int)sqrtf(12 * sigma * sigma / 3 + 1);
    if (lo % 2 == 0) lo--;
    int m = (int)lrintf((12 * sigma * sigma - 3.0f * lo * lo - 12.0f * lo - 9) / (-4.0f * lo - 4));
    for (int i = 0; i < 3; i++) boxes[i] = i < m ? lo : lo + 2;
}

// A running-sum box blur along `lines` lines of `length` bytes; zero beyond the ends.
static void box_blur(const uint8_t *src, uint8_t *dst, int lines, int length, int step, int stride, int r)
{
    int span = 2 * r + 1;
    for (int l = 0; l < lines; l++) {
        const uint8_t *s = src + (size_t)l * stride;
        uint8_t *d = dst + (size_t)l * stride;
        int sum = 0;
        for (int i = 0; i < r && i < length; i++) sum += s[(size_t)i * step];
        for (int i = 0; i < length; i++) {
            if (i + r < length) sum += s[(size_t)(i + r) * step];
            if (i - r - 1 >= 0) sum -= s[(size_t)(i - r - 1) * step];
            d[(size_t)i * step] = (uint8_t)((sum + span / 2) / span);
        }
    }
}

// The card's shape moved down and blurred: the drop shadow's alpha for every window pixel.
// It is drawn once per size; the memory goes straight back afterwards.
static uint8_t *shadow_map(const Layout *L)
{
    int w = L->width, h = L->height;
    size_t n = (size_t)w * h;
    uint8_t *map = VirtualAlloc(NULL, 2 * n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!map) return NULL;
    uint8_t *tmp = map + n;
    float dy = SHADOW_DEPTH * L->s, r = CORNER * L->s;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            map[(size_t)y * w + x] = (uint8_t)(255 * rrect(x + 0.5f, y + 0.5f - dy, (float)L->card.left,
                (float)L->card.top, (float)L->card.right, (float)L->card.bottom, r) + 0.5f);
    int boxes[3];
    gauss_boxes(SHADOW_BLUR / 3 * L->s, boxes);
    for (int i = 0; i < 3; i++) {
        box_blur(map, tmp, h, w, 1, w, boxes[i] / 2);
        box_blur(tmp, map, w, h, w, 1, boxes[i] / 2);
    }
    return map;
}

// Draws the card and its shadow; the canvas covers the whole window.
static void render_card(Canvas *c, const Layout *L)
{
    uint8_t *shadow = shadow_map(L);
    float b = edge_width(L), r = CORNER * L->s;
    float left = (float)L->card.left, top = (float)L->card.top, right = (float)L->card.right,
          bottom = (float)L->card.bottom;
    for (int y = 0; y < c->h; y++) {
        float py = y + 0.5f;
        uint32_t fill = fill_at(L, py), *row = c->bits + (size_t)y * c->w;
        const uint8_t *shade = shadow ? shadow + (size_t)y * c->w : NULL;
        for (int x = 0; x < c->w; x++) {
            float px = x + 0.5f;
            row[x] = shade ? (uint32_t)(shade[x] * SHADOW_OPACITY + 0.5f) << 24 : 0;
            if (px < left - 1 || px > right + 1 || py < top - 1 || py > bottom + 1) continue;
            // As WPF draws a Border: the base, then the edge between the outer and the inner
            // outline (the corner radius runs along the middle of the edge), then the fill inside.
            float outer = rrect(px, py, left, top, right, bottom, r + b / 2);
            float inner = rrect(px, py, left + b, top + b, right - b, bottom - b, r - b / 2);
            blend(&row[x], BASE, rrect(px, py, left, top, right, bottom, r));
            blend(&row[x], EDGE, fmaxf(outer - inner, 0));
            blend(&row[x], fill, inner);
        }
    }
    if (shadow) VirtualFree(shadow, 0, MEM_RELEASE);
}

// ---- Gauges ----

// An angle's place on the way round, clockwise from 0 at three o'clock to 4 for the full turn:
// in the same order as the angle itself, and much cheaper than atan2.
static float turn_of(float dx, float dy)
{
    float p = dy / (fabsf(dx) + fabsf(dy));
    return dx < 0 ? 2 - p : dy < 0 ? 4 + p : p;
}

// Part of the ring, clockwise from START: how far it goes in turn_of units, and its caps' centres.
typedef struct {
    float span, x0, y0, x1, y1;
} RingArc;

static RingArc arc_of(float cx, float cy, float radius, float sweep)
{
    float a = START * DEG, b = (START + sweep) * DEG;
    float span = turn_of(cosf(b), sinf(b)) - turn_of(cosf(a), sinf(a));
    return (RingArc){ span < 0 ? span + 4 : span, cx + radius * cosf(a), cy + radius * sinf(a),
                      cx + radius * cosf(b), cy + radius * sinf(b) };
}

// From a pixel to the middle of an arc's stroke: straight across the ring beside the arc, else to the nearer cap.
static inline float arc_distance(const RingArc *a, float turn, float across, float x, float y)
{
    if (turn <= a->span) return across;
    float x0 = x - a->x0, y0 = y - a->y0, x1 = x - a->x1, y1 = y - a->y1;
    return sqrtf(fminf(x0 * x0 + y0 * y0, x1 * x1 + y1 * y1));
}

// Strokes the track and, over it, the value's part of the ring, both with round caps, in one pass.
// It visits only the pixels between the ring's outer and inner edges: no cap reaches past them.
static void draw_ring(Canvas *c, RECT clip, float cx, float cy, float radius, float width, float sweep, uint32_t color)
{
    float reach = width / 2 + 0.5f; // coverage fades out over the half pixel past the stroke
    float outer = radius + reach, inner = fmaxf(radius - reach, 0);
    float start = turn_of(cosf(START * DEG), sinf(START * DEG));
    RingArc track = arc_of(cx, cy, radius, FULL_SWEEP), value = arc_of(cx, cy, radius, sweep);
    for (int y = clip.top; y < clip.bottom; y++) {
        float py = y + 0.5f, dy = py - cy;
        if (dy * dy >= outer * outer) continue;
        float xo = sqrtf(outer * outer - dy * dy), xi = dy * dy < inner * inner ? sqrtf(inner * inner - dy * dy) : 0;
        // The run left of the hole in the middle and the run right of it; one run where the row has no hole.
        int hole_left = (int)ceilf(cx - xi), hole_right = (int)floorf(cx + xi);
        if (hole_right < hole_left) hole_right = hole_left;
        const int runs[2][2] = { { (int)floorf(cx - xo), hole_left }, { hole_right, (int)ceilf(cx + xo) } };
        uint32_t *row = c->bits + (size_t)y * c->w;
        for (int r = 0; r < 2; r++) {
            int left = runs[r][0] > clip.left ? runs[r][0] : clip.left;
            int right = runs[r][1] < clip.right ? runs[r][1] : clip.right;
            for (int x = left; x < right; x++) {
                float px = x + 0.5f, dx = px - cx;
                float across = fabsf(sqrtf(dx * dx + dy * dy) - radius); // from the ring's centre line
                if (across >= reach) continue;
                float turn = turn_of(dx, dy) - start;
                if (turn < 0) turn += 4;
                uint32_t *p = &row[x];
                blend(p, TRACK, clamp01(reach - arc_distance(&track, turn, across, px, py)));
                if (sweep > 0) blend(p, color, clamp01(reach - arc_distance(&value, turn, across, px, py)));
            }
        }
    }
}

// A GDI font at a pixel size, or the fallback when the face is missing (Segoe UI Variable
// comes with Windows 11). GDI keeps every weight of it as a face of its own.
static HFONT make_font(HDC dc, const WCHAR *face, const WCHAR *fallback, int weight, float size, BYTE quality)
{
    LOGFONTW lf = { 0 };
    lf.lfHeight = -(LONG)lrintf(size);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfQuality = quality;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, font);
    WCHAR got[64] = L"";
    GetTextFaceW(dc, ARRAYSIZE(got), got);
    SelectObject(dc, old);
    if (wcsncmp(got, face, wcslen(face)) == 0) return font;
    DeleteObject(font);
    wcsncpy(lf.lfFaceName, fallback, LF_FACESIZE - 1);
    return CreateFontIndirectW(&lf);
}

// The gauge fonts' metrics are kept in window pixels.
static void font_init(Font *f, const WCHAR *face, float size)
{
    if (f->handle) DeleteObject(f->handle);
    f->handle = make_font(measure, face, L"Segoe UI Semibold", FW_SEMIBOLD, size * SUPERSAMPLE, ANTIALIASED_QUALITY);
    HGDIOBJ old = SelectObject(measure, f->handle);
    TEXTMETRICW tm;
    GetTextMetricsW(measure, &tm);
    GLYPHMETRICS gm;
    const MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    float cap = GetGlyphOutlineW(measure, L'H', GGO_METRICS, &gm, 0, NULL, &identity) != GDI_ERROR
        ? (float)gm.gmBlackBoxY : tm.tmAscent * 0.7f;
    f->ascent = (float)tm.tmAscent / SUPERSAMPLE;
    f->descent = (float)tm.tmDescent / SUPERSAMPLE;
    f->cap = cap / SUPERSAMPLE;
    SelectObject(measure, old);
}

static float text_width(const Font *f, const WCHAR *text, int n)
{
    HGDIOBJ old = SelectObject(measure, f->handle);
    SIZE size = { 0, 0 };
    GetTextExtentPoint32W(measure, text, n, &size);
    SelectObject(measure, old);
    return (float)size.cx / SUPERSAMPLE;
}

// Draws a run with its left end at x and its baseline at y (window pixels), within `clip`.
// The baseline goes to a whole pixel, as WPF puts it, which keeps the feet of the letters crisp.
// The enlarged copy goes to the top-left corner of the scratch bitmap, which grows to fit it.
static void draw_text(Canvas *c, RECT clip, const Font *f, const WCHAR *text, int n, float x, float y, uint32_t color)
{
    const int k = SUPERSAMPLE;
    y = floorf(y + 0.5f);
    RECT box = { (LONG)floorf(x) - 2, (LONG)floorf(y - f->ascent) - 1,
                 (LONG)ceilf(x + text_width(f, text, n)) + 2, (LONG)ceilf(y + f->descent) + 1 };
    if (!IntersectRect(&box, &box, &clip)) return;
    int w = (box.right - box.left) * k, h = (box.bottom - box.top) * k;
    if ((w > scratch.w || h > scratch.h) &&
        !canvas_init(&scratch, w > scratch.w ? w : scratch.w, h > scratch.h ? h : scratch.h))
        return;
    for (int v = 0; v < h; v++) memset(scratch.bits + (size_t)v * scratch.w, 0, (size_t)w * sizeof *scratch.bits);
    HGDIOBJ old = SelectObject(scratch.dc, f->handle);
    SetBkMode(scratch.dc, TRANSPARENT);
    SetTextColor(scratch.dc, RGB(255, 255, 255));
    SetTextAlign(scratch.dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
    TextOutW(scratch.dc, (int)lrintf((x - box.left) * k), (int)lrintf((y - box.top) * k), text, n);
    SelectObject(scratch.dc, old);
    GdiFlush();
    for (int y0 = box.top; y0 < box.bottom; y0++) {
        uint32_t *row = c->bits + (size_t)y0 * c->w;
        for (int x0 = box.left; x0 < box.right; x0++) {
            unsigned sum = 0; // green of the k × k samples under the pixel: the text is gray
            for (int v = 0; v < k; v++) {
                const uint32_t *src = scratch.bits + (size_t)((y0 - box.top) * k + v) * scratch.w + (x0 - box.left) * k;
                for (int u = 0; u < k; u++) sum += (src[u] >> 8) & 255;
            }
            if (sum) blend_text(&row[x0], color, (float)sum / (255 * k * k));
        }
    }
}

int format_int(WCHAR *out, long long value)
{
    WCHAR digits[24];
    int n = 0, len = 0;
    unsigned long long u = value < 0 ? 0ull - (unsigned long long)value : (unsigned long long)value;
    do digits[n++] = (WCHAR)(L'0' + u % 10);
    while (u /= 10);
    if (value < 0) out[len++] = L'-';
    while (n) out[len++] = digits[--n];
    out[len] = 0;
    return len;
}

// Draws one gauge. Its cell lies well inside the card, so the gradient alone is under it.
static void draw_gauge(Canvas *c, const Layout *L, const Gauge *g, int index)
{
    RECT cell = cell_rect(L, index);
    for (int y = cell.top; y < cell.bottom; y++) {
        uint32_t fill = fill_at(L, y + 0.5f), *row = c->bits + (size_t)y * c->w;
        for (int x = cell.left; x < cell.right; x++) row[x] = fill;
    }

    float cx = L->grid_x + (index % L->cols + 0.5f) * L->cell, cy = L->grid_y + (index / L->cols + 0.5f) * L->cell;
    float radius = RADIUS * L->s, width = THICKNESS * L->s;
    draw_ring(c, cell, cx, cy, radius, width, g->value > 0 ? FULL_SWEEP * fminf(g->value, 100) / 100 : 0,
              g->value >= g->hot ? HOT : g->value >= g->warn ? WARM : NORMAL);

    // Optical centring: the baseline half a cap height below the centre. The digits are centred
    // and "°" hangs after them, like on a weather widget; "52%" is centred as a whole.
    float baseline = cy + value_font.cap / 2;
    if (isnan(g->value)) {
        draw_text(c, cell, &value_font, L"—", 1, cx - text_width(&value_font, L"—", 1) / 2, baseline, SECONDARY);
    } else {
        WCHAR number[24];
        int n = format_int(number, (long long)g->value);
        float w = text_width(&value_font, number, n);
        float x = cx - (g->percent ? w + text_width(&unit_font, L"%", 1) : w) / 2;
        draw_text(c, cell, &value_font, number, n, x, baseline, PRIMARY);
        if (g->percent) draw_text(c, cell, &unit_font, L"%", 1, x + w, baseline, SECONDARY);
        else draw_text(c, cell, &value_font, L"°", 1, x + w, baseline, SECONDARY);
    }

    // The caption sits in the gap at the bottom of the ring.
    int n = (int)wcslen(g->label);
    float top = cy + RADIUS * L->s * 0.74f - (caption_font.ascent + caption_font.descent) / 2;
    draw_text(c, cell, &caption_font, g->label, n, cx - text_width(&caption_font, g->label, n) / 2,
              top + caption_font.ascent, SECONDARY);
}

// ---- The window ----

bool render_layout(const Layout *L)
{
    gamma_init();
    if (!measure && !(measure = CreateCompatibleDC(NULL))) return false;
    font_init(&value_font, L"Segoe UI Variable Display Semib", VALUE_SIZE * L->s);
    font_init(&unit_font, L"Segoe UI Variable Display Semib", UNIT_SIZE * L->s);
    font_init(&caption_font, L"Segoe UI Variable Text Semibold", CAPTION_SIZE * L->s);
    return true;
}

// Hands the canvas to the window; with a dirty rectangle only that part is copied.
static void present(HWND hwnd, const Layout *L, const Canvas *c, const POINT *pos, const RECT *dirty)
{
    SIZE size = { L->width, L->height };
    POINT origin = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO info = { sizeof info, NULL, pos, &size, c->dc, &origin, 0, &bf, ULW_ALPHA, dirty };
    UpdateLayeredWindowIndirect(hwnd, &info);
}

bool render_window(HWND hwnd, const POINT *pos, const Layout *L, const Gauge *gauges)
{
    Canvas c = { 0 };
    if (!canvas_init(&c, L->width, L->height)) return false;
    render_card(&c, L);
    for (int i = 0; i < GAUGE_COUNT; i++) draw_gauge(&c, L, &gauges[i], i);
    present(hwnd, L, &c, pos, NULL);
    canvas_free(&c);
    canvas_free(&scratch);
    return true;
}

// The fresh canvas holds only the changed gauges, so each cell is handed over on its own: one
// rectangle around two of them could take in an empty cell between.
void render_gauges(HWND hwnd, const Layout *L, const Gauge *gauges, unsigned changed)
{
    Canvas c = { 0 };
    if (!canvas_init(&c, L->width, L->height)) return;
    for (int i = 0; i < GAUGE_COUNT; i++) {
        if (!(changed & 1u << i)) continue;
        RECT cell = cell_rect(L, i);
        draw_gauge(&c, L, &gauges[i], i);
        present(hwnd, L, &c, NULL, &cell);
    }
    canvas_free(&c);
    canvas_free(&scratch);
}

// ---- Tooltip ----

// Sizes, draws and places the tooltip below the cursor. Its bitmap lives only for the update:
// the window keeps the picture.
void render_tip(HWND tip, const WCHAR *text, POINT cursor, UINT dpi)
{
    float k = dpi / 96.0f;
    if (!tip_font || tip_font_dpi != dpi) {
        if (tip_font) DeleteObject(tip_font);
        HDC dc = CreateCompatibleDC(NULL);
        tip_font = make_font(dc, L"Segoe UI Variable Text", L"Segoe UI", FW_NORMAL, TIP_SIZE * k, CLEARTYPE_QUALITY);
        tip_font_dpi = dpi;
        DeleteDC(dc);
    }

    Canvas c = { 0 };
    if (!canvas_init(&c, 1, 1)) return;
    SelectObject(c.dc, tip_font);
    RECT box = { 0, 0, 0, 0 };
    DrawTextW(c.dc, text, -1, &box, DT_CALCRECT | DT_NOPREFIX);
    float b = fmaxf(1, rintf(k)), r = TIP_CORNER * k;
    int pad_x = (int)b + (int)lrintf(10 * k), pad_y = (int)b + (int)lrintf(6 * k);
    int w = box.right + 2 * pad_x, h = box.bottom + 2 * pad_y;
    if (!canvas_init(&c, w, h)) return;
    SelectObject(c.dc, tip_font);
    SetBkMode(c.dc, TRANSPARENT);
    SetTextColor(c.dc, RGB(255, 255, 255));
    OffsetRect(&box, pad_x, pad_y);
    DrawTextW(c.dc, text, -1, &box, DT_NOPREFIX);
    GdiFlush();

    // The text is white on black so far; it becomes the coverage of the real text colour.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t *p = &c.bits[(size_t)y * w + x];
            float text_cover = cleartype_coverage(*p), px = x + 0.5f, py = y + 0.5f;
            float outer = rrect(px, py, 0, 0, (float)w, (float)h, r + b / 2);
            float inner = rrect(px, py, b, b, w - b, h - b, r - b / 2);
            *p = 0;
            blend(p, TIP_EDGE, fmaxf(outer - inner, 0));
            blend(p, TIP_FILL, inner);
            blend_text(p, TIP_TEXT, text_cover);
        }

    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    POINT pos = { cursor.x, cursor.y + (LONG)lrintf(20 * k) };
    if (pos.x + w > mi.rcWork.right) pos.x = mi.rcWork.right - w;
    if (pos.x < mi.rcWork.left) pos.x = mi.rcWork.left;
    if (pos.y + h > mi.rcWork.bottom) pos.y = cursor.y - h - (LONG)lrintf(4 * k);
    if (pos.y < mi.rcWork.top) pos.y = mi.rcWork.top;
    SIZE size = { w, h };
    POINT origin = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(tip, NULL, &pos, &size, c.dc, &origin, 0, &bf, ULW_ALPHA);
    canvas_free(&c);
}

void render_free(void)
{
    canvas_free(&scratch);
    if (measure) DeleteDC(measure), measure = NULL;
    Font *fonts[] = { &value_font, &unit_font, &caption_font };
    for (int i = 0; i < 3; i++)
        if (fonts[i]->handle) DeleteObject(fonts[i]->handle), fonts[i]->handle = NULL;
    if (tip_font) DeleteObject(tip_font), tip_font = NULL;
}
