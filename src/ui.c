/* ui.c - drawing, formatting and helpers */
#include "flint.h"


u32 rgb(int r, int g, int b) {
    u32 c = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    return c ? c : 1;
}
u32 mix(u32 a, u32 b, float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return rgb(r, g, bl);
}
void hpush(Hist *h, float x) {
    if (h->n == HN) {
        memmove(h->v, h->v + 1, (HN - 1) * sizeof(float));
        h->n = HN - 1;
    }
    h->v[h->n++] = x;
}

int slurp(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}
/* mkdir -p of path's directory */
void mkdirs(const char *path) {
    char d[512];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(d, 0755);
            *p = '/';
        }
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) {
        tb_shutdown();
        fputs("flint: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void fmt_bytes(char *o, size_t n, double b) {
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (b >= 1024 && i < 4) b /= 1024, i++;
    if (i == 0) snprintf(o, n, "%.0f %s", b, u[i]);
    else snprintf(o, n, b < 10 ? "%.2f %s" : b < 100 ? "%.1f %s" : "%.0f %s", b, u[i]);
}
void fmt_rate(char *o, size_t n, double b) {
    static const char *u[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s"};
    int i = 0;
    while (b >= 1024 && i < 4) b /= 1024, i++;
    if (i == 0) snprintf(o, n, "%.0f %s", b, u[i]);
    else snprintf(o, n, b < 10 ? "%.2f %s" : b < 100 ? "%.1f %s" : "%.0f %s", b, u[i]);
}
void fmt_bits(char *o, size_t n, double bytes) {
    static const char *u[] = {"bitps", "Kibps", "Mibps", "Gibps", "Tibps"};
    double b = bytes * 8;
    int i = 0;
    while (b >= 1024 && i < 4) b /= 1024, i++;
    if (i == 0) snprintf(o, n, "%.0f %s", b, u[i]);
    else snprintf(o, n, b < 10 ? "%.2f %s" : b < 100 ? "%.1f %s" : "%.0f %s", b, u[i]);
}
void fmt_mem(char *o, size_t n, double b) {
    if (b >= 1073741824.0) snprintf(o, n, b < 10737418240.0 ? "%.1fG" : "%.0fG", b / 1073741824.0);
    else if (b >= 1048576.0) snprintf(o, n, b < 10485760.0 ? "%.1fM" : "%.0fM", b / 1048576.0);
    else if (b >= 1024.0) snprintf(o, n, "%.0fK", b / 1024.0);
    else snprintf(o, n, "%.0fB", b);
}

int W, H;
int truecolor = 1;
u32 cur_bg = 0x0F1217;

/* nearest xterm-256 colour */
static int near6(int v) {
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int b = 0;
    for (int i = 1; i < 6; i++)
        if (abs(v - lv[i]) < abs(v - lv[b])) b = i;
    return b;
}
u32 tcol(u32 c) {
    u32 v = c & 0xFFFFFF, fl = c & 0xFF000000u;
    if (truecolor || !v) return c;
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    int r = (int)(v >> 16), g = (int)((v >> 8) & 255), b = (int)(v & 255);
    int ri = near6(r), gi = near6(g), bi = near6(b);
    int d1 = (r - lv[ri]) * (r - lv[ri]) + (g - lv[gi]) * (g - lv[gi]) + (b - lv[bi]) * (b - lv[bi]);
    int avg = (r + g + b) / 3, k = (avg - 3) / 10;
    if (k < 0) k = 0;
    if (k > 23) k = 23;
    int gv = 8 + 10 * k;
    int d2 = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);
    return fl | (u32)(d2 < d1 ? 232 + k : 16 + 36 * ri + 6 * gi + bi);
}

u32 heat_c(float pct, u32 base) { return pct >= 85 ? theme.hot : pct >= 60 ? theme.warn : base; }

void cell(int x, int y, u32 ch, u32 fg, u32 bg) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    tb_set_cell(x, y, ch, tcol(fg), tcol(bg ? bg : cur_bg));
}
static int is_wide(u32 c) {
    return c >= 0x1100 && (c <= 0x115f || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
                           (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe6f) ||
                           (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
                           (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd));
}
/* zero-width characters are dropped */
static int zero_width(u32 c) {
    return (c >= 0x0300 && c <= 0x036f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f) ||
           (c >= 0x2060 && c <= 0x2064) || c == 0xfeff;
}
/* draw up to maxw cols, returns cols used */
int txt(int x, int y, int maxw, u32 fg, u32 bg, const char *s) {
    int n = 0;
    while (*s && n < maxw) {
        uint32_t c;
        int l = tb_utf8_char_to_unicode(&c, s);
        if (l <= 0) {
            s++;
            continue;
        }
        s += l;
        if (zero_width(c)) continue;
        if (c < 0x20 || (c >= 0x7f && c < 0xa0) || is_wide(c)) c = '?';
        cell(x + n, y, c, fg, bg);
        n++;
    }
    return n;
}
int ucols(const char *s) {
    int n = 0;
    while (*s) {
        uint32_t c;
        int l = tb_utf8_char_to_unicode(&c, s);
        if (l <= 0) { /* invalid byte: txt() skips it too */
            s++;
            continue;
        }
        s += l;
        if (!zero_width(c)) n++;
    }
    return n;
}
void rtxt(int xr, int y, u32 fg, u32 bg, const char *s) { /* right-aligned to xr */
    txt(xr - ucols(s) + 1, y, 9999, fg, bg, s);
}
void fill(int x, int y, int w, int h, u32 bg) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) cell(x + i, y + j, ' ', 0, bg);
}

/* filled rectangle; without truecolor a rule row instead; returns rows used */
int tile(int x, int y, int w, int h, u32 bg) {
    cur_bg = truecolor ? bg : theme.bg;
    if (w <= 0 || h <= 0) return 0;
    fill(x, y, w, h, 0);
    if (truecolor || h < 3) return 0;
    for (int i = 0; i < w; i++) cell(x + i, y, 0x2500, theme.rule, 0);
    return 1;
}

/* "■ name ───── info": first info that fits; returns its x or -1 */
int label(int x, int y, int w, u32 mark, const char *name, const char *const *info, int n, u32 ifg, int *iw_out) {
    int right = x + w - 2, cx = x + 3;
    if (iw_out) *iw_out = 0;
    if (right < cx + 1) return -1;
    cell(x + 1, y, 0x25a0, mark, 0);
    cx += txt(cx, y, right - cx + 1, BOLD(theme.bright), 0, name);
    int rs = cx + 1, ix = -1, ic = 0;
    for (int i = 0; i < n && ix < 0; i++) {
        int c = info[i] && *info[i] ? ucols(info[i]) : 0;
        if (!c || right - c + 1 - 2 < rs) continue;
        ix = right - c + 1;
        ic = c;
        txt(ix, y, c, ifg, 0, info[i]);
        if (iw_out) *iw_out = c;
    }
    for (int i = rs; i <= (ic ? ix - 2 : right); i++) cell(i, y, 0x2500, theme.rule, 0);
    return ix;
}

/* " key " chip; returns width */
int keycap(int x, int y, const char *s, u32 bg, u32 fg) {
    if (!bg) bg = theme.keycap;
    if (!fg) fg = theme.bright;
    int n = ucols(s);
    cell(x, y, ' ', 0, bg);
    txt(x + 1, y, n, BOLD(fg), bg, s);
    cell(x + 1 + n, y, ' ', 0, bg);
    return n + 2;
}

/* thin meter; col 0 = heat colour */
void bar(int x, int y, int w, float f, u32 col) {
    if (w <= 0) return;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int n = (int)(f * (float)w + 0.5f);
    if (f > 0 && n < 1) n = 1;
    u32 c = col ? col : heat_c(f * 100, theme.meter);
    for (int i = 0; i < w; i++) cell(x + i, y, 0x2501, i < n ? c : theme.track, 0);
}

/* colour ramp lo -> mid -> hi; mid 0 = two stops */
static u32 ramp(float f, u32 lo, u32 mid, u32 hi) {
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    if (!mid) return mix(lo, hi, f);
    return f < 0.5f ? mix(lo, mid, f * 2) : mix(mid, hi, (f - 0.5f) * 2);
}

/* braille area graph: 2 samples per cell, newest at right, colour by height */
void graph(int x, int y, int w, int h, const float *v, int n, float scale, u32 lo, u32 mid, u32 hi) {
    if (w <= 0 || h <= 0) return;
    if (scale <= 0) scale = 1;
    /* braille dot bits, bottom to top */
    static const unsigned char dl[4] = {0x40, 0x04, 0x02, 0x01}, dr[4] = {0x80, 0x20, 0x10, 0x08};
    for (int c = 0; c < w; c++) {
        int lv[2] = {-1, -1}; /* dots per sample, -1 = none */
        for (int k = 0; k < 2; k++) {
            int idx = n - 2 * w + 2 * c + k;
            if (idx < 0) continue;
            float f = v[idx] / scale;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            lv[k] = (int)(f * (float)(h * 4) + 0.5f);
            if (f > 0.005f && lv[k] < 1) lv[k] = 1;
        }
        if (lv[0] < 0 && lv[1] < 0) continue;
        int top = (lv[0] > lv[1] ? lv[0] : lv[1]);
        for (int r = 0; r < h || (r == 0); r++) {
            unsigned bits = 0;
            for (int d = 0; d < 4; d++) {
                if (lv[0] > r * 4 + d) bits |= dl[d];
                if (lv[1] > r * 4 + d) bits |= dr[d];
            }
            if (r == 0) { /* baseline under empty samples */
                if (lv[0] == 0) bits |= dl[0];
                if (lv[1] == 0) bits |= dr[0];
            }
            if (!bits) break;
            int cap = (r + 1) * 4 < top ? (r + 1) * 4 : top;
            u32 col = top == 0 ? theme.track : ramp((float)cap / (float)(h * 4), lo, mid, hi);
            if (top > 0 && (r + 1) * 4 < top) col = mix(col, cur_bg, 0.35f);
            cell(x + c, y + h - 1 - r, 0x2800 + bits, col, 0);
        }
    }
}
