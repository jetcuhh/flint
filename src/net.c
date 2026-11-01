/* net.c - net panel */
#include "flint.h"

#define MAXIF 64
typedef struct {
    char name[IF_NAMESIZE];
    unsigned long long rx, tx;
} IfStat;
static IfStat ifs[MAXIF];
static int nif;
static char cur_if[IF_NAMESIZE];
static unsigned long long prx, ptx;
static int net_primed;
static double dl_rate, ul_rate, dl_tot, ul_tot;
static Hist h_dl, h_ul;
static char ipstr[48];

static int if_index(const char *name) {
    for (int i = 0; i < nif; i++)
        if (!strcmp(ifs[i].name, name)) return i;
    return -1;
}
void net_update(double dt) {
    static char buf[65536];
    if (slurp("/proc/net/dev", buf, sizeof buf) <= 0) return;
    nif = 0;
    char *p = strchr(buf, '\n');
    if (p) p = strchr(p + 1, '\n');
    while (p && *++p && nif < MAXIF) {
        char *colon = strchr(p, ':');
        if (!colon) break;
        while (*p == ' ') p++;
        IfStat *s = &ifs[nif];
        size_t l = (size_t)(colon - p);
        if (l >= sizeof s->name) l = sizeof s->name - 1;
        memcpy(s->name, p, l);
        s->name[l] = 0;
        if (sscanf(colon + 1, " %llu %*u %*u %*u %*u %*u %*u %*u %llu", &s->rx, &s->tx) == 2) nif++;
        p = strchr(colon, '\n');
    }
    if (!nif) return;
    if (if_index(cur_if) < 0) { /* pick busiest non-lo iface */
        int best = -1;
        for (int i = 0; i < nif; i++)
            if (strcmp(ifs[i].name, "lo") && (best < 0 || ifs[i].rx > ifs[best].rx)) best = i;
        snprintf(cur_if, sizeof cur_if, "%s", ifs[best < 0 ? 0 : best].name);
        net_primed = 0;
    }
    IfStat *s = &ifs[if_index(cur_if)];
    if (net_primed && dt > 0) {
        /* counter reset: no traffic */
        dl_rate = s->rx >= prx ? (double)(s->rx - prx) / dt : 0;
        ul_rate = s->tx >= ptx ? (double)(s->tx - ptx) / dt : 0;
        hpush(&h_dl, (float)dl_rate);
        hpush(&h_ul, (float)ul_rate);
    }
    prx = s->rx;
    ptx = s->tx;
    dl_tot = (double)s->rx;
    ul_tot = (double)s->tx;
    net_primed = 1;

    ipstr[0] = 0;
    struct ifaddrs *ifa, *a;
    if (getifaddrs(&ifa) == 0) {
        for (a = ifa; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, cur_if)) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ipstr, sizeof ipstr);
                break;
            }
        freeifaddrs(ifa);
    }
}
void net_switch(int dir) {
    if (nif < 1) return;
    int i = if_index(cur_if);
    i = ((i < 0 ? 0 : i) + dir + nif) % nif;
    snprintf(cur_if, sizeof cur_if, "%s", ifs[i].name);
    net_primed = 0;
    dl_rate = ul_rate = 0;
    dl_tot = (double)ifs[i].rx; /* show new totals even when paused */
    ul_tot = (double)ifs[i].tx;
    ipstr[0] = 0;
    h_dl.n = h_ul.n = 0;
}


static int sw_x = -1, sw_y; /* "‹ eth0 ›" switcher start */

void net_hide(void) { sw_x = -1; }

/* the arrows, 1 cell slack */
int net_click(int mx, int my) {
    if (sw_x < 0 || my != sw_y) return 0;
    int nx = sw_x + 3 + ucols(cur_if);
    if (mx >= sw_x && mx <= sw_x + 1) net_switch(-1);
    else if (mx >= nx - 1 && mx <= nx) net_switch(1);
    else return 0;
    return 1;
}

/* one direction: arrow, rate, graph */
static void net_line(int ix, int iw, int r, int gh, const char *arrow, double rate, const Hist *hs, u32 lo, u32 hi) {
    char b[48];
    fmt_rate(b, sizeof b, rate);
    txt(ix, r, 1, BOLD(hi), 0, arrow);
    txt(ix + 2, r, 11, BOLD(C_BRIGHT), 0, b);
    int sx = 14;
    if (iw >= 56 && rate > 0) {
        char t[32];
        fmt_bits(t, sizeof t, rate);
        snprintf(b, sizeof b, "(%s)", t);
        txt(ix + sx, r, 13, C_DIM, 0, b);
    }
    if (iw >= 56) sx += 14; /* keep the column so the graph stays put */
    int sw = iw - sx;
    if (sw < 1) return;
    float sc = 10240;
    for (int i = hs->n - 1; i >= 0 && i >= hs->n - sw; i--)
        if (hs->v[i] > sc) sc = hs->v[i];
    graph(ix + sx, r, sw, gh, hs->v, hs->n, sc, lo, 0, hi);
}

void net_draw(int x, int y, int w, int h) {
    int t = tile(x, y, w, h, theme.tile);
    y += t, h -= t;
    char a[80], c[48];
    snprintf(c, sizeof c, "\xe2\x80\xb9 %s \xe2\x80\xba", cur_if);
    snprintf(a, sizeof a, "%s \xc2\xb7 %s", c, ipstr);
    const char *info[] = {a, c};
    sw_x = label(x, y, w, theme.mark_net, "network", info + !ipstr[0], 2 - !ipstr[0], C_DIM, NULL);
    sw_y = y;
    int ix = x + 1, iw = w - 2, avail = h - 1;
    int gh = (avail - 2) / 2; /* graph rows per direction */
    if (gh < 1) gh = 1;
    if (gh > 6) gh = 6;
    if (iw < 14) return;
    int spare = avail - (2 * gh + 1), pad = spare > 0 ? spare / 2 + 1 : 0; /* centred, nudged one row down */
    if (pad > spare) pad = spare > 0 ? spare : 0;
    y += pad;
    if (avail >= gh) net_line(ix, iw, y + 1, gh, "\xe2\x96\xbc", dl_rate, &h_dl, theme.pal[P_BLUE], theme.pal[P_LBLUE]);
    if (avail >= 2 * gh) net_line(ix, iw, y + 1 + gh, gh, "\xe2\x96\xb2", ul_rate, &h_ul, theme.pal[P_GREEN], theme.pal[P_LGREEN]);
    if (avail >= 2 * gh + 1) {
        char d[32], u[32];
        fmt_bytes(d, sizeof d, dl_tot);
        fmt_bytes(u, sizeof u, ul_tot);
        int tx = ix, ty = y + 1 + 2 * gh;
        int full = ucols(d) + ucols(u) + 14 <= iw; /* "total  ▼ d   ▲ u" */
        if (full) tx += txt(tx, ty, 8, C_DIM, 0, "total  ");
        tx += txt(tx, ty, 2, theme.pal[P_LBLUE], 0, "\xe2\x96\xbc ");
        tx += txt(tx, ty, iw, C_TEXT, 0, d);
        tx += txt(tx, ty, 3, 0, 0, full ? "   " : " ");
        tx += txt(tx, ty, 2, theme.pal[P_LGREEN], 0, "\xe2\x96\xb2 ");
        txt(tx, ty, ix + iw - tx, C_TEXT, 0, u);
    }
}
