/* mem.c - mem panel */
#include "flint.h"

double m_swt, m_swf;
static double m_tot, m_avail, m_cache, m_free;

static double kv(const char *buf, const char *key) {
    const char *p = strstr(buf, key);
    return p ? atof(p + strlen(key)) * 1024.0 : 0;
}
void mem_update(void) {
    static char buf[8192];
    buf[0] = '\n';
    if (slurp("/proc/meminfo", buf + 1, sizeof buf - 1) <= 0) return;
    m_tot = kv(buf, "\nMemTotal:");
    m_free = kv(buf, "\nMemFree:");
    m_avail = kv(buf, "\nMemAvailable:");
    m_cache = kv(buf, "\nCached:");
    m_swt = kv(buf, "\nSwapTotal:");
    m_swf = kv(buf, "\nSwapFree:");
}


/* memory tile: used, cached, available, free */
void mem_draw(int x, int y, int w, int h) {
    int t = tile(x, y, w, h, theme.tile);
    y += t, h -= t;
    char b[32];
    fmt_bytes(b, sizeof b, m_tot);
    const char *info[] = {b};
    label(x, y, w, theme.mark_mem, "memory", info, 1, C_DIM, NULL);
    int ix = x + 1, iw = w - 2, rows = h - 1 >= 5 ? 4 : h - 1;
    if (iw < 20) return;
    struct {
        const char *name;
        double val, tot;
        int heat;
        u32 col;
    } it[4] = {{"used", m_tot - m_avail, m_tot, 1, theme.pal[P_MAGENTA]},
               {"cached", m_cache, m_tot, 0, theme.pal[P_BLUE]},
               {"available", m_avail, m_tot, 0, theme.pal[P_GREEN]},
               {"free", m_free, m_tot, 0, theme.pal[P_CYAN]}};
    int pw = 4, m = iw - 10 - 10 - pw;
    if (m < 3) pw = 0, m = iw - 20;
    if (m > 18) m = 18;
    for (int i = 0; i < rows; i++) {
        int r = y + 1 + i;
        float f = it[i].tot > 0 ? (float)(it[i].val / it[i].tot) : 0;
        txt(ix, r, 10, C_DIM, 0, it[i].name);
        if (it[i].tot <= 0) { /* no swap configured */
            rtxt(ix + 10 + (m >= 3 ? m : 0) + 9, r, C_DIM, 0, "none");
            continue;
        }
        if (m >= 3) bar(ix + 10, r, m, f, it[i].heat ? heat_c(f * 100, it[i].col) : it[i].col);
        fmt_bytes(b, sizeof b, it[i].val);
        int vx = ix + 10 + (m >= 3 ? m : 0);
        rtxt(vx + 9, r, C_TEXT, 0, b);
        if (pw) {
            snprintf(b, sizeof b, "%3.0f%%", f * 100);
            rtxt(vx + 10 + pw - 1, r, it[i].heat ? heat_c(f * 100, C_DIM) : C_DIM, 0, b);
        }
    }
}

/* side by side if they fit, else stacked */
void memdisk_draw(int x, int y, int w, int h) {
    if ((w - 1) / 2 - 2 >= 34) {
        int lw = (w - 1) / 2;
        mem_draw(x, y, lw, h);
        disk_draw(x + lw + 1, y, w - lw - 1, h);
    } else if (h >= 10) {
        mem_draw(x, y, w, 6);
        disk_draw(x, y + 7, w, h - 7);
    } else
        mem_draw(x, y, w, h);
}

int memdisk_rows(int w) {
    int disks = disk_count() + 2; /* label + rows + blank */
    if ((w - 1) / 2 - 2 >= 34) return disks > 6 ? disks : 6;
    return 6 + 1 + disks;
}
