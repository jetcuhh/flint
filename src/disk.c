#include "flint.h"

#define MAXDISK 32
typedef struct {
    char name[24];
    double total, used, frac;
} Disk;
static Disk disks[MAXDISK];
static int ndisk;

static void unescape_mount(char *s) {
    char *o = s;
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
            s += 4;
        } else
            *o++ = *s++;
    }
    *o = 0;
}

void disk_update(void) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[1024], seen[MAXDISK][256];
    ndisk = 0;
    while (fgets(line, sizeof line, f) && ndisk < MAXDISK) {
        char dev[256], mp[256];
        if (sscanf(line, "%255s %255s", dev, mp) != 2) continue;
        unescape_mount(dev);
        unescape_mount(mp);
        if (strncmp(dev, "/dev/", 5) || !strncmp(dev, "/dev/loop", 9) || !strncmp(mp, "/snap", 5)) continue;
        int dup = 0;
        for (int i = 0; i < ndisk; i++)
            if (!strcmp(seen[i], dev)) dup = 1;
        if (dup) continue;
        struct statvfs sv;
        if (statvfs(mp, &sv) != 0 || sv.f_blocks == 0) continue;
        snprintf(seen[ndisk], sizeof seen[0], "%s", dev);
        Disk *d = &disks[ndisk++];
        if (!strcmp(mp, "/")) snprintf(d->name, sizeof d->name, "root");
        else {
            const char *b = strrchr(mp, '/');
            snprintf(d->name, sizeof d->name, "%s", b && b[1] ? b + 1 : mp);
        }
        for (int i = 0; i < ndisk - 1; i++)
            if (!strcmp(disks[i].name, d->name)) {
                size_t l = strlen(mp), m = sizeof d->name - 1;
                snprintf(d->name, sizeof d->name, "%s", l > m ? mp + l - m : mp);
                break;
            }
        d->total = (double)sv.f_blocks * sv.f_frsize;
        d->used = (double)(sv.f_blocks - sv.f_bfree) * sv.f_frsize;
        double avail = (double)sv.f_bavail * sv.f_frsize;
        d->frac = d->used + avail > 0 ? d->used / (d->used + avail) : 0;
    }
    fclose(f);
}

static void fmt_pair(char *o, size_t n, double used, double total) {
    char u[32], t[32];
    fmt_bytes(u, sizeof u, used);
    fmt_bytes(t, sizeof t, total);
    const char *us = strchr(u, ' '), *ts = strchr(t, ' ');
    if (us && ts && !strcmp(us, ts)) u[us - u] = 0;
    snprintf(o, n, "%s / %s", u, t);
}

static void disk_row(int ix, int iw, int r, const char *name, double total, double used, double f) {
    char b[48];
    if (ucols(name) <= 5) txt(ix, r, 5, C_DIM, 0, name);
    else {
        txt(ix, r, 4, C_DIM, 0, name);
        cell(ix + 4, r, 0x2026, C_DIM, 0);
    }
    int pw = 4, m = iw - 6 - 19 - pw;
    if (m < 3) pw = 0, m = iw - 6 - 18;
    if (m > 14) m = 14;
    if (m >= 3) bar(ix + 6, r, m, (float)f, heat_c((float)f * 100, theme.pal[P_BLUE]));
    fmt_pair(b, sizeof b, used, total);
    int vx = ix + 6 + (m >= 3 ? m : 0);
    rtxt(vx + 17, r, C_TEXT, 0, b);
    if (pw) {
        snprintf(b, sizeof b, "%3.0f%%", f * 100);
        rtxt(vx + 19 + pw - 1, r, C_DIM, 0, b);
    }
}

void disk_draw(int x, int y, int w, int h) {
    int t = tile(x, y, w, h, theme.tile);
    y += t, h -= t;
    char b[24];
    snprintf(b, sizeof b, "%d mounted", ndisk);
    const char *info[] = {b};
    label(x, y, w, theme.mark_mem, "disks", info, 1, C_DIM, NULL);
    int nrows = disk_count();
    int ix = x + 1, iw = w - 2, rows = h - 1 >= nrows + 1 ? h - 2 : h - 1;
    if (iw < 24) return;
    int r = 0;
    for (int i = 0; i < ndisk && r < rows; i++, r++)
        disk_row(ix, iw, y + 1 + r, disks[i].name, disks[i].total, disks[i].used, disks[i].frac);
    if (m_swt > 0 && r < rows)
        disk_row(ix, iw, y + 1 + r, "swap", m_swt, m_swt - m_swf, (m_swt - m_swf) / m_swt);
}

int disk_count(void) { return ndisk + (m_swt > 0); }
