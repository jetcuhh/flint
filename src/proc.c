/* proc.c - proc panel */
#include "flint.h"
#include <stdarg.h>

typedef struct {
    int pid, threads;
    uid_t uid;
    unsigned long long ticks;
    double rss;
    float cpu;
    char comm[24], user[12], cmd[160];
} Proc;
static Proc *pr, *pp;
static int npr, npp, cappr, cappp, proc_primed;
#define NUC 1024
static struct {
    uid_t uid;
    char name[12];
} uc[NUC];
static int nuc, uc_next;

static void user_of(uid_t u, char *o) {
    for (int i = 0; i < nuc; i++)
        if (uc[i].uid == u) {
            strcpy(o, uc[i].name);
            return;
        }
    struct passwd *p = getpwuid(u);
    if (p) snprintf(o, 12, "%s", p->pw_name);
    else snprintf(o, 12, "%u", (unsigned)u);
    int s = nuc < NUC ? nuc++ : uc_next++ % NUC;
    uc[s].uid = u;
    strcpy(uc[s].name, o);
}
static int cmp_pid(const void *a, const void *b) { return ((const Proc *)a)->pid - ((const Proc *)b)->pid; }

static void read_procs(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    Proc *t = pp;
    pp = pr;
    pr = t;
    int tc = cappp;
    cappp = cappr;
    cappr = tc;
    npp = npr;
    npr = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        char path[64], buf[1024];
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        if (slurp(path, buf, sizeof buf) <= 0) continue;
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;
        if (npr == cappr) {
            cappr = cappr ? cappr * 2 : 512;
            pr = xrealloc(pr, (size_t)cappr * sizeof(Proc));
        }
        Proc *p = &pr[npr];
        memset(p, 0, sizeof *p);
        p->pid = pid;
        size_t cl = (size_t)(rp - lp - 1);
        if (cl >= sizeof p->comm) cl = sizeof p->comm - 1;
        memcpy(p->comm, lp + 1, cl);
        char state;
        unsigned long long ut = 0, st = 0;
        long thr = 0, rss = 0;
        if (sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %ld %*d %*u %*u %ld",
                   &state, &ut, &st, &thr, &rss) < 5)
            continue;
        p->threads = (int)thr;
        p->ticks = ut + st;
        p->rss = (double)rss * (double)page_sz;
        struct stat sb;
        snprintf(path, sizeof path, "/proc/%d", pid);
        p->uid = stat(path, &sb) == 0 ? sb.st_uid : 0;
        user_of(p->uid, p->user);
        char cb[256];
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        int n = slurp(path, cb, sizeof cb);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                if (cb[i] == 0 || (unsigned char)cb[i] < 0x20) cb[i] = ' ';
            while (n > 0 && cb[n - 1] == ' ') cb[--n] = 0;
            snprintf(p->cmd, sizeof p->cmd, "%s", cb);
        }
        npr++;
    }
    closedir(d);
    qsort(pr, (size_t)npr, sizeof(Proc), cmp_pid);
    int j = 0;
    for (int i = 0; i < npr; i++) {
        while (j < npp && pp[j].pid < pr[i].pid) j++;
        pr[i].cpu = 0;
        if (proc_primed && dtotal_all > 0 && j < npp && pp[j].pid == pr[i].pid && pr[i].ticks >= pp[j].ticks)
            pr[i].cpu = (float)((pr[i].ticks - pp[j].ticks) * 100.0 / (double)dtotal_all);
    }
    proc_primed = 1;
}

/* view state */
static const char *sort_names[S_N] = {"pid", "program", "command", "user", "threads", "mem", "cpu"};
int sort_key = S_CPU, sel_pid = -1;
static int sel_idx, scroll_off;
int nview;
static int *view, capview;
char filter[48];
int filter_edit;
int confirm_sig, confirm_pid;
char status_msg[96];
long status_at;

static int cmp_view(const void *a, const void *b) {
    const Proc *x = &pr[*(const int *)a], *y = &pr[*(const int *)b];
    double r = 0;
    switch (sort_key) {
    /* all columns sort downward */
    case S_PID: r = y->pid - x->pid; break;
    case S_PROG: r = strcasecmp(y->comm, x->comm); break;
    case S_CMD: r = strcasecmp(y->cmd[0] ? y->cmd : y->comm, x->cmd[0] ? x->cmd : x->comm); break;
    case S_USER: r = strcmp(y->user, x->user); break;
    case S_THR: r = y->threads - x->threads; break;
    case S_MEM: r = y->rss - x->rss; break;
    default: r = y->cpu - x->cpu; break;
    }
    if (r < 0) return -1;
    if (r > 0) return 1;
    return y->pid - x->pid;
}
void proc_rebuild(void) {
    if (capview < npr) {
        capview = npr + 64;
        view = xrealloc(view, (size_t)capview * sizeof(int));
    }
    nview = 0;
    for (int i = 0; i < npr; i++) {
        if (filter[0] && !strcasestr(pr[i].comm, filter) && !strcasestr(pr[i].cmd, filter) &&
            !strcasestr(pr[i].user, filter))
            continue;
        view[nview++] = i;
    }
    qsort(view, (size_t)nview, sizeof(int), cmp_view);
    if (sel_pid < 0) { /* nothing selected: stay that way */
        sel_idx = 0;
        return;
    }
    int found = -1;
    for (int i = 0; i < nview; i++)
        if (pr[view[i]].pid == sel_pid) found = i;
    if (found >= 0) sel_idx = found;
    else if (sel_idx >= nview) sel_idx = nview - 1;
    if (sel_idx < 0) sel_idx = 0;
    sel_pid = nview ? pr[view[sel_idx]].pid : -1;
}
void proc_deselect(void) { sel_pid = -1; }
void proc_select_first(void) {
    if (!nview) return;
    sel_idx = scroll_off = 0;
    sel_pid = pr[view[0]].pid;
}
void proc_move(int d) {
    if (!nview) return;
    if (sel_pid < 0) { /* first move selects the top visible row */
        sel_idx = scroll_off < nview ? scroll_off : nview - 1;
        if (d >= nview || d <= -nview) sel_idx = d < 0 ? 0 : nview - 1;
        sel_pid = pr[view[sel_idx]].pid;
        return;
    }
    sel_idx += d;
    if (sel_idx < 0) sel_idx = 0;
    if (sel_idx >= nview) sel_idx = nview - 1;
    sel_pid = pr[view[sel_idx]].pid;
}

typedef struct {
    int x, w;
} Span;
static struct { /* click targets from the last draw */
    int y, ix, iy, iw, rows;
    int cx[S_N], cw[S_N];
    Span filt, sort;
} hit;
void proc_scroll(int d) { /* moves the selection, or just the view when nothing is selected */
    if (sel_pid >= 0) return proc_move(d);
    int maxo = nview - hit.rows;
    scroll_off += d;
    if (scroll_off > maxo) scroll_off = maxo;
    if (scroll_off < 0) scroll_off = 0;
}
static struct { /* popup rect */
    int x, y, w, h;
} pbox;
static int sb_drag;
static char confirm_name[24];
static struct { /* confirm popup yes-button */
    int y;
    Span yes;
} pop;
int sort_menu; /* sort picker open */
static int sm_sel;
int info_open; /* process details popup open */
static int info_pid, dc_pid = -1;
static long dc_t;

void proc_hide(void) { memset(&hit, 0, sizeof hit); }

static int in(Span s, int mx) { return mx >= s.x && mx < s.x + s.w; }

void proc_draw(int x, int y, int w, int h) {
    memset(&hit, 0, sizeof hit);
    int t = tile(x, y, w, h, theme.tile_proc);
    y += t, h -= t;
    hit.y = y;

    /* label row; the keycap opens the sort menu */
    char sk[40];
    snprintf(sk, sizeof sk, "sort: %s", sort_names[sort_key]);
    int capw = ucols(sk) + 2, kx = x + w - 1 - capw, lw = w;
    if (kx > x + 16) lw = kx - x;
    char i2[96];
    u32 ifg = C_DIM;
    if (filter_edit || filter[0]) {
        /* show the filter's tail */
        int room = lw - 15 - 8 - 1, skip = 0;
        if (room < 4) room = 4;
        while (filter[skip] && ucols(filter + skip) > room) {
            skip++;
            while (((unsigned char)filter[skip] & 0xc0) == 0x80) skip++;
        }
        snprintf(i2, sizeof i2, "filter: %s%s", filter + skip, filter_edit ? "_" : "");
        ifg = C_BRIGHT;
    } else {
        snprintf(i2, sizeof i2, "%d of %d", nview, npr);
    }
    const char *info[] = {i2};
    int iwd;
    int ixs = label(x, y, lw, theme.mark_proc, "processes", info, 1, ifg, &iwd);
    if (ixs >= 0) hit.filt = (Span){ixs, iwd};
    if (lw != w) {
        keycap(kx, y, sk, 0, 0);
        hit.sort = (Span){kx, capw};
    }

    int ix = x + 1, iy = y + 1, iw = w - 2;
    uid_t me = getuid();
    int rows = h - 2; /* minus label and header */
    if (iw < 32 || rows < 1) return;
    /* columns drop when narrow: command, threads, user, then program shrinks */
    const int thrw = 7, userw = 8, memw = 7, cpuw = 6;
    int pidw = 5; /* longest pid, min 5 */
    for (int i = 0; i < npr; i++) {
        int d = 1;
        for (int v = pr[i].pid; v >= 10; v /= 10) d++;
        if (d > pidw) pidw = d;
    }
    int progw = 16, use_thr = 1, use_user = 1, cmdw = 0;
    int avail = iw - 1;
#define USED() (pidw + 1 + progw + 1 + memw + 1 + cpuw + (use_thr ? thrw + 1 : 0) + (use_user ? userw + 1 : 0))
    if (USED() > avail) use_thr = 0;
    if (USED() > avail) use_user = 0;
    if (USED() > avail) progw -= USED() - avail;
    if (progw < 8) progw = 8;
    if (avail - USED() - 1 >= 10) cmdw = avail - USED() - 1;
    int cxs[S_N] = {0}, cws[S_N] = {0};
    {
        int cx = ix;
        cxs[S_PID] = cx, cws[S_PID] = pidw, cx += pidw + 1;
        cxs[S_PROG] = cx, cws[S_PROG] = progw, cx += progw + 1;
        if (cmdw) cxs[S_CMD] = cx, cws[S_CMD] = cmdw, cx += cmdw + 1;
        if (use_user) cxs[S_USER] = cx, cws[S_USER] = userw, cx += userw + 1;
        if (use_thr) cxs[S_THR] = cx, cws[S_THR] = thrw, cx += thrw + 1;
        cxs[S_MEM] = cx, cws[S_MEM] = memw, cx += memw + 1;
        cxs[S_CPU] = cx, cws[S_CPU] = cpuw;
    }
#undef USED
    memcpy(hit.cx, cxs, sizeof cxs);
    memcpy(hit.cw, cws, sizeof cws);
    /* header */
    static const char *hname[S_N] = {"pid", "program", "command", "user", "threads", "mem", "cpu"};
    static const int hright[S_N] = {1, 0, 0, 0, 1, 1, 1};
    for (int k = 0; k < S_N; k++) {
        if (!cws[k]) continue;
        u32 col = k == sort_key ? BOLD(C_BRIGHT) : C_DIM;
        const char *hn = hname[k];
        if (hright[k]) rtxt(cxs[k] + cws[k] - 1, iy, col, 0, hn);
        else txt(cxs[k], iy, cws[k], col, 0, hn);
    }
    if (sel_pid >= 0 && sel_idx < scroll_off) scroll_off = sel_idx;
    if (sel_pid >= 0 && sel_idx >= scroll_off + rows) scroll_off = sel_idx - rows + 1;
    if (scroll_off > nview - rows) scroll_off = nview - rows;
    if (scroll_off < 0) scroll_off = 0;
    hit.ix = ix, hit.iy = iy, hit.iw = iw, hit.rows = rows;
    for (int i = 0; i < rows; i++) {
        int vi = scroll_off + i, ry = iy + 1 + i;
        if (vi >= nview) break;
        const Proc *p = &pr[view[vi]];
        int sel = sel_pid >= 0 && vi == sel_idx;
        u32 bg = sel ? theme.sel_bg : 0;
        if (sel) fill(x, ry, w, 1, bg);
        u32 tx = sel ? theme.sel_text : C_TEXT, dim = sel ? theme.sel_text : C_DIM;
        u32 cc = sel ? theme.sel_text : p->cpu >= 50 ? theme.hot : p->cpu >= 10 ? theme.warn : p->cpu >= 0.1f ? C_TEXT : theme.idle;
        /* user: root red, you green, others blue */
        u32 uc = sel ? theme.sel_text : p->uid == 0 ? theme.pal[P_RED] : p->uid == me ? theme.pal[P_GREEN] : theme.pal[P_BLUE];
        /* mem: yellow from 256 MiB, warn from 1 GiB */
        u32 mc = sel ? theme.sel_text : p->rss >= 1073741824.0 ? theme.warn : p->rss >= 268435456.0 ? theme.pal[P_LYELLOW] : C_TEXT;
        char b[32];
        snprintf(b, sizeof b, "%d", p->pid);
        rtxt(cxs[S_PID] + pidw - 1, ry, tx, bg, b);
        txt(cxs[S_PROG], ry, progw, tx, bg, p->comm);
        if (cmdw) txt(cxs[S_CMD], ry, cmdw, dim, bg, p->cmd[0] ? p->cmd : p->comm);
        if (use_user) txt(cxs[S_USER], ry, userw, uc, bg, p->user);
        if (use_thr) {
            snprintf(b, sizeof b, "%d", p->threads);
            rtxt(cxs[S_THR] + thrw - 1, ry, dim, bg, b);
        }
        fmt_mem(b, sizeof b, p->rss);
        rtxt(cxs[S_MEM] + memw - 1, ry, mc, bg, b);
        snprintf(b, sizeof b, "%.1f", p->cpu);
        rtxt(cxs[S_CPU] + cpuw - 1, ry, cc, bg, b);
    }
    /* scrollbar in the right padding cell */
    if (nview > rows && rows > 0) {
        int th = rows * rows / nview;
        if (th < 1) th = 1;
        int maxo = nview - rows, tp = maxo ? scroll_off * (rows - th) / maxo : 0;
        for (int i = 0; i < rows; i++) {
            if (i < tp || i >= tp + th) continue; /* thumb only, no track */
            cell(x + w - 1, iy + 1 + i, 0x2588, mix(theme.track, theme.dim, 0.5f), sel_pid >= 0 && scroll_off + i == sel_idx ? theme.sel_bg : 0);
        }
    }
}


static int pend_pid, pend_sig;
static long pend_at;
static char pend_name[24];

void proc_signal(int pid, int sig) {
    status_msg[0] = 0;
    pend_pid = 0;
    if (pid <= 1) return;
    if (kill(pid, sig) != 0) {
        if (errno == EPERM) snprintf(status_msg, sizeof status_msg, "pid %d: permission denied, run flint with sudo", pid);
        else if (errno == ESRCH) snprintf(status_msg, sizeof status_msg, "pid %d: already exited", pid);
        else snprintf(status_msg, sizeof status_msg, "pid %d: %s", pid, strerror(errno));
        status_at = now_ms();
        return;
    }
    pend_pid = pid, pend_sig = sig, pend_at = now_ms();
    snprintf(pend_name, sizeof pend_name, "%s", confirm_name);
}

static void pend_check(void) {
    if (!pend_pid || now_ms() - pend_at < 2000) return;
    char path[64], buf[512], st = 0;
    int alive = 0;
    snprintf(path, sizeof path, "/proc/%d/stat", pend_pid);
    if (slurp(path, buf, sizeof buf) > 0) {
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (lp && rp && rp > lp && rp[1] == ' ') {
            size_t cl = (size_t)(rp - lp - 1);
            if (cl > sizeof pend_name - 1) cl = sizeof pend_name - 1;
            st = rp[2];
            alive = strlen(pend_name) == cl && !memcmp(pend_name, lp + 1, cl) && st != 'Z' && st != 'X';
        }
    }
    if (alive) {
        if (pend_sig == SIGKILL && st == 'D')
            snprintf(status_msg, sizeof status_msg, "pid %d (%s): stuck in uninterruptible wait, cannot be killed", pend_pid, pend_name);
        else if (pend_sig == SIGKILL)
            snprintf(status_msg, sizeof status_msg, "pid %d (%s): still running after kill", pend_pid, pend_name);
        else
            snprintf(status_msg, sizeof status_msg, "pid %d (%s): refused to terminate, try kill (k)", pend_pid, pend_name);
        status_at = now_ms();
    }
    pend_pid = 0;
}

static void sb_set(int my) {
    int span = hit.rows - 1, maxo = nview - hit.rows;
    if (maxo <= 0) return;
    int t = my - (hit.iy + 1);
    if (t < 0) t = 0;
    if (t > span) t = span;
    scroll_off = span > 0 ? t * maxo / span : 0;
    if (sel_pid < 0) return;
    if (sel_idx < scroll_off) sel_idx = scroll_off;
    if (sel_idx >= scroll_off + hit.rows) sel_idx = scroll_off + hit.rows - 1;
    sel_pid = pr[view[sel_idx]].pid;
}

void proc_release(void) { sb_drag = 0; }

void proc_mouse(int mx, int my, int drag) {
    if (drag) {
        if (sb_drag) sb_set(my);
        return;
    }
    sb_drag = 0;
    if (my == hit.y) {
        if (in(hit.filt, mx)) filter_edit = 1;
        else if (in(hit.sort, mx)) proc_sort_open();
    } else if (hit.rows > 0 && my == hit.iy) {
        for (int k = 0; k < S_N; k++)
            if (hit.cw[k] && mx >= hit.cx[k] && mx < hit.cx[k] + hit.cw[k]) {
                sort_key = k;
                proc_rebuild();
                break;
            }
    } else if (my > hit.iy && my <= hit.iy + hit.rows && mx >= hit.ix && mx <= hit.ix + hit.iw) {
        if (nview > hit.rows && mx == hit.ix + hit.iw) sb_drag = 1, sb_set(my);
        else {
            int vi = scroll_off + my - hit.iy - 1;
            if (vi < nview) {
                sel_idx = vi, sel_pid = pr[view[vi]].pid;
                long t = now_ms();
                if (sel_pid == dc_pid && t - dc_t < 450) proc_info_open(), dc_pid = -1; /* second click: details */
                else dc_pid = sel_pid, dc_t = t;
            }
        }
    }
}

void proc_ask(int sig) {
    if (!nview || sel_pid < 0) return;
    confirm_sig = sig;
    confirm_pid = sel_pid;
    snprintf(confirm_name, sizeof confirm_name, "%s", pr[view[sel_idx]].comm);
}

/* ---- popups ---- */
static void pbox_open(int w, int h, u32 bg) {
    if (w > W - 2) w = W - 2;
    pbox.w = w, pbox.h = h;
    pbox.x = (W - w) / 2, pbox.y = (H - h) / 2;
    cur_bg = bg;
    fill(pbox.x, pbox.y, w, h, 0);
}

/* terminate / kill confirmation */
static void confirm_draw(void) {
    int kl = confirm_sig == SIGKILL;
    u32 sev = mix(theme.dim, theme.bright, 0.55f); /* neutral tint */
    pbox_open(46, 7, theme.bg);
    u32 bg = cur_bg;
    int x = pbox.x, y = pbox.y, lx = x + 3, room = pbox.w - 6;
    txt(lx, y + 1, room, BOLD(sev), 0, kl ? "kill this process?" : "terminate this process?");
    char b[32];
    snprintf(b, sizeof b, " \xc2\xb7 pid %d", confirm_pid);
    int pw = ucols(b), nw = room - pw;
    if (nw < 3) nw = room, pw = 0;
    int n = txt(lx, y + 3, nw, BOLD(C_BRIGHT), 0, confirm_name);
    if (pw) txt(lx + n, y + 3, pw, C_DIM, 0, b);
    int by = y + 5;
    int kw = keycap(lx, by, "y", sev, theme.bg);
    int ln = txt(lx + kw + 1, by, 9, C_TEXT, 0, kl ? "kill" : "terminate");
    pop.y = by;
    pop.yes = (Span){lx, kw + 1 + ln};
    int nx = lx + kw + 1 + ln + 3;
    kw = keycap(nx, by, "n", mix(bg, theme.bright, 0.14f), 0);
    txt(nx + kw + 1, by, 8, C_DIM, 0, "cancel");
}

static const char *sort_labels[S_N] = {"pid", "program", "command", "user", "threads", "memory", "cpu"};
static const int sort_text[S_N] = {0, 1, 1, 1, 0, 0, 0}; /* text columns read z to a */

static void sort_draw(void) {
    pbox_open(34, S_N + 6, theme.popup_bg);
    int x = pbox.x, y = pbox.y, w = pbox.w;
    txt(x + 2, y + 1, w - 4, BOLD(C_BRIGHT), 0, "sort by");
    for (int k = 0; k < S_N; k++) {
        int ry = y + 3 + k, sel = k == sm_sel, cur = k == sort_key;
        u32 bg = sel ? theme.sel_bg : 0, fg = sel ? theme.sel_text : cur ? C_BRIGHT : C_TEXT;
        if (sel) fill(x + 1, ry, w - 2, 1, bg);
        if (cur) cell(x + 2, ry, 0x25cf, sel ? theme.sel_text : theme.accent, bg);
        txt(x + 4, ry, 10, cur ? BOLD(fg) : fg, bg, sort_labels[k]);
        if (cur) {
            const char *dir = sort_text[k] ? "z to a" : "high to low";
            rtxt(x + w - 3, ry, sel ? theme.sel_text : C_DIM, bg, dir);
        }
    }
    txt(x + 2, y + S_N + 4, w - 4, C_DIM, 0, "enter apply \xc2\xb7 esc");
}


/* ---- process details, from /proc/<pid> ---- */
typedef struct {
    char label[12], val[200];
} IRow;
static IRow irow[32];
static int nirow, info_gone;
static char info_name[24];

static void irow_add(const char *label, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void irow_add(const char *label, const char *fmt, ...) {
    if (nirow >= 32) return;
    snprintf(irow[nirow].label, sizeof irow[0].label, "%s", label);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(irow[nirow].val, sizeof irow[0].val, fmt, ap);
    va_end(ap);
    nirow++;
}
static double st_num(const char *buf, const char *key) { /* buf starts with a newline */
    const char *p = strstr(buf, key);
    return p ? atof(p + strlen(key)) : 0;
}
static void fmt_dur(char *o, size_t n, double s) {
    long t = (long)s;
    if (t >= 86400) snprintf(o, n, "%ldd %ldh %02ldm", t / 86400, t / 3600 % 24, t / 60 % 60);
    else if (t >= 3600) snprintf(o, n, "%ldh %02ldm", t / 3600, t / 60 % 60);
    else snprintf(o, n, "%ldm %02lds", t / 60, t % 60);
}
static void fmt_kb(char *o, size_t n, double kb) { fmt_mem(o, n, kb * 1024.0); }

void proc_info_open(void) {
    if (!nview || sel_pid < 0) return;
    info_pid = sel_pid;
    info_open = 1;
}

/* load info_pid into irow[]; vw = value width */
static void info_load(int vw) {
    nirow = info_gone = 0;
    static long hz;
    if (!hz) hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    char path[64], sb[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", info_pid);
    int n = slurp(path, sb, sizeof sb);
    char *lp = n > 0 ? strchr(sb, '(') : NULL, *rp = n > 0 ? strrchr(sb, ')') : NULL;
    char state = '?';
    int ppid = 0;
    unsigned long long ut = 0, st = 0, start = 0;
    long prio = 0, nice = 0, thr = 0;
    if (!lp || !rp || rp < lp ||
        sscanf(rp + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %ld %ld %ld %*d %llu", &state, &ppid,
               &ut, &st, &prio, &nice, &thr, &start) < 8) {
        info_gone = 1;
        return;
    }
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof info_name) cl = sizeof info_name - 1;
    memcpy(info_name, lp + 1, cl);
    info_name[cl] = 0;

    static char buf[4096];
    buf[0] = '\n';
    snprintf(path, sizeof path, "/proc/%d/status", info_pid);
    if (slurp(path, buf + 1, sizeof buf - 1) <= 0) buf[1] = 0;

    const char *sn = "unknown";
    switch (state) {
    case 'R': sn = "running"; break;
    case 'S': sn = "sleeping"; break;
    case 'D': sn = "waiting on disk / io"; break;
    case 'Z': sn = "zombie (exited, not yet reaped)"; break;
    case 'T': case 't': sn = "stopped"; break;
    case 'I': sn = "idle"; break;
    case 'X': sn = "dead"; break;
    }
    irow_add("state", "%s", sn);

    struct stat sbuf;
    snprintf(path, sizeof path, "/proc/%d", info_pid);
    uid_t uid = stat(path, &sbuf) == 0 ? sbuf.st_uid : 0;
    char un[12];
    user_of(uid, un);
    irow_add("user", "%s  (uid %u)", un, (unsigned)uid);

    const char *pn = "";
    float cpu = 0;
    for (int i = 0; i < npr; i++) {
        if (pr[i].pid == ppid) pn = pr[i].comm;
        if (pr[i].pid == info_pid) cpu = pr[i].cpu;
    }
    irow_add("parent", "%s%s%d", pn, pn[0] ? "  \xc2\xb7  pid " : "pid ", ppid);

    double el = cpu_uptime() - (double)start / (double)hz;
    if (el < 0) el = 0;
    time_t t0 = time(NULL) - (time_t)el;
    struct tm tmv;
    localtime_r(&t0, &tmv);
    char sd[48], dur[32];
    strftime(sd, sizeof sd, "%b %-d, %-I:%M %p", &tmv);
    fmt_dur(dur, sizeof dur, el);
    irow_add("started", "%s  \xc2\xb7  running %s", sd, dur);

    long tt = (long)((ut + st) / (unsigned long long)hz);
    irow_add("cpu", "%.1f%% now  \xc2\xb7  %ld:%02ld:%02ld total", cpu, tt / 3600, tt / 60 % 60, tt % 60);

    char m1[24], m2[24], m3[24], m4[24];
    fmt_kb(m1, sizeof m1, st_num(buf, "\nVmRSS:"));
    fmt_kb(m2, sizeof m2, st_num(buf, "\nVmSize:"));
    fmt_kb(m3, sizeof m3, st_num(buf, "\nVmSwap:"));
    fmt_kb(m4, sizeof m4, st_num(buf, "\nVmHWM:"));
    irow_add("memory", "%s resident  \xc2\xb7  %s virtual  \xc2\xb7  %s swap  \xc2\xb7  %s peak", m1, m2, m3, m4);

    int nfd = -1;
    snprintf(path, sizeof path, "/proc/%d/fd", info_pid);
    DIR *d = opendir(path);
    if (d) {
        nfd = 0;
        for (struct dirent *e; (e = readdir(d));)
            if (e->d_name[0] != '.') nfd++;
        closedir(d);
    }
    char fds[24];
    if (nfd >= 0) snprintf(fds, sizeof fds, "%d open files", nfd);
    else snprintf(fds, sizeof fds, "files: no access");
    irow_add("threads", "%ld  \xc2\xb7  %s  \xc2\xb7  nice %ld  \xc2\xb7  priority %ld", thr, fds, nice, prio);
    irow_add("switches", "%.0f voluntary  \xc2\xb7  %.0f forced", st_num(buf, "\nvoluntary_ctxt_switches:"),
             st_num(buf, "\nnonvoluntary_ctxt_switches:"));

    char lk[200];
    ssize_t r;
    snprintf(path, sizeof path, "/proc/%d/exe", info_pid);
    r = readlink(path, lk, sizeof lk - 1);
    irow_add("program", "%s", r > 0 ? (lk[r] = 0, lk) : "(no access)");
    snprintf(path, sizeof path, "/proc/%d/cwd", info_pid);
    r = readlink(path, lk, sizeof lk - 1);
    irow_add("folder", "%s", r > 0 ? (lk[r] = 0, lk) : "(no access)");

    char cb[1024];
    snprintf(path, sizeof path, "/proc/%d/cmdline", info_pid);
    int cn = slurp(path, cb, sizeof cb);
    for (int i = 0; i < cn; i++)
        if (cb[i] == 0 || (unsigned char)cb[i] < 0x20) cb[i] = ' ';
    while (cn > 0 && cb[cn - 1] == ' ') cb[--cn] = 0;
    if (cn <= 0) irow_add("command", "(none, kernel thread)");
    else { /* wrap the command */
        int pos = 0, first = 1;
        if (vw < 10) vw = 10;
        while (pos < cn && nirow < 31) {
            int len = cn - pos < vw ? cn - pos : vw;
            while (len > 1 && pos + len < cn && ((unsigned char)cb[pos + len] & 0xc0) == 0x80) len--; /* keep utf-8 whole */
            char line[200];
            snprintf(line, sizeof line, "%.*s", len, cb + pos);
            irow_add(first ? "command" : "", "%s", line);
            first = 0;
            pos += len;
        }
    }
}

static void info_draw(void) {
    int w = W - 4 < 70 ? W - 4 : 70, vw = w - 4 - 11;
    info_load(vw);
    int cap = H - 2 - 6; /* rows that fit */
    int rows = nirow < cap ? nirow : cap;
    if (info_gone) rows = 1;
    pbox_open(w, rows + 6, theme.bg);
    int x = pbox.x, y = pbox.y;
    char b[48];
    snprintf(b, sizeof b, "  \xc2\xb7  pid %d", info_pid);
    int n = txt(x + 3, y + 1, w - 6, BOLD(C_BRIGHT), 0, info_gone ? "process" : info_name);
    txt(x + 3 + n, y + 1, w - 6 - n, C_DIM, 0, b);
    if (info_gone) {
        txt(x + 3, y + 3, w - 6, C_TEXT, 0, "this process has exited");
    } else {
        for (int i = 0; i < rows; i++) {
            txt(x + 3, y + 3 + i, 10, C_DIM, 0, irow[i].label);
            txt(x + 3 + 11, y + 3 + i, vw, !strcmp(irow[i].label, "command") || !irow[i].label[0] ? C_TEXT : C_BRIGHT, 0, irow[i].val);
        }
    }
    txt(x + 3, y + rows + 4, w - 6, C_DIM, 0, "esc close  \xc2\xb7  \xe2\x86\x91\xe2\x86\x93 other process  \xc2\xb7  t terminate  \xc2\xb7  k kill");
}

void proc_popup(void) {
    pop.y = -1;
    if (info_open) info_draw();
    if (confirm_sig) confirm_draw();
    else if (sort_menu) sort_draw();
    cur_bg = theme.bg;
}

static void sort_apply(int k) {
    sort_key = k;
    proc_rebuild();
}
void proc_sort_open(void) {
    sort_menu = 1;
    sm_sel = sort_key;
}
void proc_sort_key(const struct tb_event *ev) {
    if (ev->key == TB_KEY_ESC) sort_menu = 0;
    else if (ev->key == TB_KEY_ARROW_UP) sm_sel = (sm_sel + S_N - 1) % S_N;
    else if (ev->key == TB_KEY_ARROW_DOWN) sm_sel = (sm_sel + 1) % S_N;
    else if (ev->key == TB_KEY_HOME) sm_sel = 0;
    else if (ev->key == TB_KEY_END) sm_sel = S_N - 1;
    else if (ev->key == TB_KEY_ENTER || ev->ch == ' ') sort_apply(sm_sel), sort_menu = 0;
    else if (ev->ch == 's' || ev->ch == 'q') sort_menu = 0;
}
void proc_sort_click(int mx, int my) {
    if (mx >= pbox.x && mx < pbox.x + pbox.w && my >= pbox.y + 3 && my < pbox.y + 3 + S_N) sort_apply(my - pbox.y - 3);
    sort_menu = 0;
}

void proc_confirm_click(int mx, int my) {
    if (my == pop.y && in(pop.yes, mx)) proc_signal(confirm_pid, confirm_sig);
    confirm_sig = 0;
}

void proc_update(void) {
    read_procs();
    pend_check();
    proc_rebuild();
}
