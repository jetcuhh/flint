#define TB_IMPL
#include "flint.h"

int interval = 2000;
int paused;
long page_sz = 4096;

static volatile sig_atomic_t quit_sig;
static void on_signal(int sig) { quit_sig = sig; }
static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

static struct timespec t_prev;
static int have_prev;

static void collect(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = have_prev ? (double)(now.tv_sec - t_prev.tv_sec) + (now.tv_nsec - t_prev.tv_nsec) / 1e9 : 0;
    t_prev = now;
    have_prev = 1;
    cpu_update();
    gpu_update();
    mem_update();
    disk_update();
    net_update(dt);
    proc_update();
}

enum { V_CPU, V_MEM, V_NET, V_PROC, V_N };
static int show[V_N] = {1, 1, 1, 1};
static int proc_h;
static int split_pct = 44;
static int split_gx = -1, split_ty, split_drag;

static const char *panel_keys[V_N] = {"cpu", "mem", "net", "proc"};

static const char *state_path(void) {
    static char path[512];
    const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(path, sizeof path, "%s/flint/state", x);
    else if (h && *h) snprintf(path, sizeof path, "%s/.local/state/flint/state", h);
    else return NULL;
    return path;
}

static void state_cb(void *ud, const char *sec, const char *key, const char *val) {
    (void)ud, (void)sec;
    for (int i = 0; i < V_N; i++)
        if (!strcmp(key, panel_keys[i])) show[i] = atoi(val) != 0;
    if (!strcmp(key, "split")) {
        int v = atoi(val);
        if (v >= 20 && v <= 75) split_pct = v;
    }
}

static void state_save(void) {
    const char *path = state_path();
    if (!path) return;
    mkdirs(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("; written by flint\n", f);
    for (int i = 0; i < V_N; i++) fprintf(f, "%s = %d\n", panel_keys[i], show[i]);
    fprintf(f, "split = %d\n", split_pct);
    fclose(f);
}

static void toggle(int v) {
    show[v] = !show[v];
    state_save();
    if (!show[V_PROC]) filter_edit = confirm_sig = sort_menu = info_open = 0;
}

static struct {
    int y, mx, px, qx;
} ivhit;

static void topbar(void) {
    int by = H - 1;
    char host[64] = "", up[32], clk[16], ms[16];
    gethostname(host, sizeof host - 1);
    int s = (int)cpu_uptime(), dd = s / 86400;
    if (dd) snprintf(up, sizeof up, "up %dd %02d:%02d", dd, s / 3600 % 24, s / 60 % 60);
    else snprintf(up, sizeof up, "up %02d:%02d", s / 3600, s / 60 % 60);
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    strftime(clk, sizeof clk, "%-I:%M:%S %p", &tmv);
    if (interval % 1000 == 0) snprintf(ms, sizeof ms, "%ds", interval / 1000);
    else if (interval > 1000) snprintf(ms, sizeof ms, "%d.%ds", interval / 1000, interval % 1000 / 100);
    else snprintf(ms, sizeof ms, "%dms", interval);

    int rw = 3 + 1 + (int)strlen(ms) + 1 + 3 + 2 + 3, kx = W - 1 - rw;
    u32 br = BOLD(C_BRIGHT);
    ivhit.y = by, ivhit.mx = kx;
    int rx = kx;
    txt(kx + 1, by, 1, C_DIM, 0, "\xe2\x88\x92");
    kx += 4;
    kx += txt(kx, by, 16, br, 0, ms) + 1;
    ivhit.px = kx;
    txt(kx + 1, by, 1, C_DIM, 0, "+");
    kx += 5;
    ivhit.qx = kx;
    txt(kx + 1, by, 1, C_DIM, 0, "?");

    int x = 1;
    static const int masks[] = {7, 6, 4, 0};
    const char *part[3] = {host, up, clk};
    const u32 pc[3] = {theme.pal[P_LBLUE], C_DIM, C_TEXT};
    for (int m = 0; m < 4; m++) {
        int need = 0, k = 0;
        for (int i = 0; i < 3; i++)
            if ((masks[m] >> i & 1) && part[i][0]) need += (k++ ? 3 : 0) + ucols(part[i]);
        if (x + need + 2 > rx && masks[m]) continue;
        k = 0;
        for (int i = 0; i < 3; i++)
            if ((masks[m] >> i & 1) && part[i][0]) {
                if (k++) x += 3;
                x += txt(x, by, rx - x - 1, pc[i], 0, part[i]);
            }
        break;
    }
    if (paused && x + 9 <= rx) x += txt(x + 3, by, 6, C_ACCENT, 0, "paused") + 3;
    if (status_msg[0]) {
        int sl = ucols(status_msg), sx = rx - 3 - sl;
        if (sx > x + 2) txt(sx, by, sl, C_ACCENT, 0, status_msg);
    }
}

static int show_help;
static struct {
    int x, y, w, n;
} hp;
static const struct {
    const char *key, *label;
    int act;
} hlp[] = {{"\xe2\x86\x91\xe2\x86\x93", "select", 0}, {"\xe2\x86\x90\xe2\x86\x92", "sort column", 0},
           {"enter", "process details", 0}, {"s", "sort menu", 's'},                   {"esc", "unselect / clear filter", 0},                  {"f", "filter", 'f'},
           {"t", "terminate", 't'},                   {"k", "kill", 'k'},
           {"b n", "network interface", 0},           {"+ -", "refresh interval", 0},
           {"1-4", "show / hide panels", 0},          {"p", "pause / resume", 'p'},
           {"q", "quit", 0}};

static void help_draw(void) {
    int n = (int)(sizeof hlp / sizeof *hlp), pw = 34, ph = n + 3;
    int px = (W - pw) / 2, py = (H - ph) / 2;
    cur_bg = theme.popup_bg;
    fill(px, py, pw, ph, 0);
    txt(px + 2, py, pw - 4, BOLD(C_BRIGHT), 0, "keys");
    for (int i = 0; i < n; i++) {
        int r = py + 2 + i;
        keycap(px + 2, r, hlp[i].key, mix(theme.popup_bg, theme.bright, 0.12f), 0);
        txt(px + 10, r, pw - 12, C_DIM, 0, hlp[i].label);
    }
    hp.x = px, hp.y = py + 2, hp.w = pw, hp.n = n;
    cur_bg = theme.bg;
}

static void draw(void) {
    tb_clear();
    W = tb_width();
    H = tb_height();
    cur_bg = theme.bg;
    net_hide();
    proc_hide();
    proc_h = 0;
    memset(&ivhit, 0, sizeof ivhit);
    ivhit.y = -1;
    if (W < 60 || H < 20) {
        txt(1, 1, W - 1, C_TEXT, 0, "flint: terminal too small (need 60x20)");
        tb_present();
        return;
    }
    if (status_msg[0] && now_ms() - status_at > 3000) status_msg[0] = 0;
    topbar();

    int ty = 0, th = H - 1, X = 1, XW = W - 2;
    int vc = show[V_CPU], vm = show[V_MEM], vn = show[V_NET], vp = show[V_PROC];
    int left = vc || vm || vn, items = vc + vm + vn;
    int lw = !left ? 0 : vp ? XW * split_pct / 100 : XW;
    if (left && vp && lw < 24) lw = 24;
    if (left && vp && lw > XW - 1 - 34) lw = XW - 1 - 34;
    int px = X + lw + (left ? 1 : 0), pw = XW - lw - (left ? 1 : 0);
    split_gx = left && vp ? X + lw : -1;
    split_ty = ty;
    if (left) {
        int avail = th - (items - 1), cap = th * 33 / 100 < 9 ? 9 : th * 33 / 100;
        int ch = !vc ? 0 : items == 1 ? avail : cpu_rows() < cap ? cpu_rows() : cap;
        if (vc && items > 1 && ch > avail - 3 * (items - 1)) ch = avail - 3 * (items - 1);
        int rest = avail - ch, mh = 0, nh = 0;
        if (vm && vn) {
            mh = memdisk_rows(lw);
            if (rest - mh < 5) mh = rest - 5;
            nh = rest - mh;
        } else if (vm) mh = rest;
        else nh = rest;
        int y = ty;
        if (vc) cpu_draw(X, y, lw, ch), y += ch + 1;
        if (vm) memdisk_draw(X, y, lw, mh), y += mh + 1;
        if (vn) net_draw(X, y, lw, nh);
    }
    if (vp) {
        proc_h = th;
        proc_draw(px, ty, pw, th);
    }
    cur_bg = theme.bg;
    if (!left && !vp) txt(1, ty, W - 1, C_TEXT, 0, "all panels hidden: press 1-4");
    if (split_gx >= 0)
        for (int i = -1; i <= 1; i++) cell(split_gx, ty + th / 2 + i, 0x2503, split_drag ? theme.accent : mix(theme.track, theme.dim, 0.5f), 0);
    proc_popup();
    if (show_help) help_draw();
    tb_present();
}

long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
void interval_step(int dir) {
    interval += dir * 100;
    if (interval < 100) interval = 100;
    if (interval > 10000) interval = 10000;
}

static int on_key(const struct tb_event *ev) {
    int rows = proc_h - 2;
    if (rows < 1) rows = 1;
    if (ev->key == TB_KEY_CTRL_Q) return 1;
    if (show_help) {
        show_help = 0;
        return 0;
    }
    if (confirm_sig) {
        if (ev->ch == 'y' || ev->ch == 'Y' || ev->key == TB_KEY_ENTER) proc_signal(confirm_pid, confirm_sig);
        confirm_sig = 0;
        return 0;
    }
    if (info_open) {
        if (ev->key == TB_KEY_ARROW_UP || ev->key == TB_KEY_ARROW_DOWN) {
            proc_move(ev->key == TB_KEY_ARROW_UP ? -1 : 1);
            proc_info_open();
        } else if (ev->ch == 't' || ev->ch == 'k') {
            info_open = 0;
            proc_ask(ev->ch == 'k' ? SIGKILL : SIGTERM);
        } else
            info_open = 0;
        return 0;
    }
    if (sort_menu) {
        proc_sort_key(ev);
        return 0;
    }
    if (filter_edit) {
        size_t n = strlen(filter);
        if (ev->key == TB_KEY_ENTER) {
            filter_edit = 0;
            proc_rebuild();
            if (filter[0]) proc_select_first();
            return 0;
        } else if (ev->key == TB_KEY_ESC) filter_edit = 0, filter[0] = 0;
        else if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
            while (n && ((unsigned char)filter[n - 1] & 0xc0) == 0x80) n--;
            if (n) n--;
            filter[n] = 0;
        } else if (ev->ch >= 0x20 && ev->ch != 0x7f && !(ev->ch >= 0x80 && ev->ch < 0xa0)) {
            char u[8];
            int l = tb_utf8_unicode_to_char(u, ev->ch);
            if (l > 0 && n + (size_t)l < sizeof filter) {
                memcpy(filter + n, u, (size_t)l);
                filter[n + (size_t)l] = 0;
            }
        }
        proc_rebuild();
        return 0;
    }
    switch (ev->key) {
    case TB_KEY_ENTER:
        if (show[V_PROC]) proc_info_open();
        return 0;
    case TB_KEY_ARROW_UP: proc_move(-1); return 0;
    case TB_KEY_ARROW_DOWN: proc_move(1); return 0;
    case TB_KEY_PGUP: proc_scroll(-rows); return 0;
    case TB_KEY_PGDN: proc_scroll(rows); return 0;
    case TB_KEY_HOME: proc_scroll(-nview); return 0;
    case TB_KEY_END: proc_scroll(nview); return 0;
    case TB_KEY_ARROW_LEFT: sort_key = (sort_key + S_N - 1) % S_N; proc_rebuild(); return 0;
    case TB_KEY_ARROW_RIGHT: sort_key = (sort_key + 1) % S_N; proc_rebuild(); return 0;
    case TB_KEY_ESC:
        if (sel_pid >= 0) proc_deselect();
        else filter[0] = 0;
        proc_rebuild();
        return 0;
    default: break;
    }
    switch (ev->ch) {
    case 'q': return 1;
    case '?': show_help = 1; break;
    case '+': case '=': interval_step(1); break;
    case '-': interval_step(-1); break;
    case 's': if (show[V_PROC]) proc_sort_open(); break;
    case 'f': filter_edit = show[V_PROC]; break;
    case 'b': net_switch(-1); break;
    case 'n': net_switch(1); break;
    case '1': toggle(V_CPU); break;
    case '2': toggle(V_MEM); break;
    case '3': toggle(V_NET); break;
    case '4': toggle(V_PROC); break;
    case 't': case 'k':
        if (show[V_PROC]) proc_ask(ev->ch == 'k' ? SIGKILL : SIGTERM);
        break;
    case 'p': paused = !paused; break;
    default: break;
    }
    return 0;
}

static void split_set(int mx) {
    int xw = W - 2, lw = mx - 1;
    split_pct = (lw * 100 + xw / 2) / xw;
    if (split_pct < 20) split_pct = 20;
    if (split_pct > 75) split_pct = 75;
}

static void on_mouse(const struct tb_event *ev) {
    if (ev->key == TB_KEY_MOUSE_WHEEL_UP) proc_scroll(-3);
    else if (ev->key == TB_KEY_MOUSE_WHEEL_DOWN) proc_scroll(3);
    else if (ev->key == TB_KEY_MOUSE_RELEASE) {
        proc_release();
        if (split_drag) split_drag = 0, state_save();
    } else if (ev->key == TB_KEY_MOUSE_LEFT && (ev->mod & TB_MOD_MOTION)) {
        if (split_drag) split_set(ev->x);
        else proc_mouse(ev->x, ev->y, 1);
    }
    else if (ev->key == TB_KEY_MOUSE_LEFT) {
        if (show_help) {
            show_help = 0;
            if (ev->x >= hp.x && ev->x < hp.x + hp.w && ev->y >= hp.y && ev->y < hp.y + hp.n) {
                int act = hlp[ev->y - hp.y].act;
                if (act == 'f') filter_edit = show[V_PROC];
                else if (act == 'p') paused = !paused;
                else if (act == 's' && show[V_PROC]) proc_sort_open();
                else if (act == 't' && show[V_PROC]) proc_ask(SIGTERM);
                else if (act == 'k' && show[V_PROC]) proc_ask(SIGKILL);
            }
            return;
        }
        if (confirm_sig) {
            proc_confirm_click(ev->x, ev->y);
            return;
        }
        if (info_open) {
            info_open = 0;
            return;
        }
        if (sort_menu) {
            proc_sort_click(ev->x, ev->y);
            return;
        }
        if (split_gx >= 0 && ev->y >= split_ty && ev->x >= split_gx - 1 && ev->x <= split_gx + 1) {
            split_drag = 1;
            split_set(ev->x);
            return;
        }
        filter_edit = 0;
        if (ev->y == ivhit.y) {
            if (ev->x >= ivhit.qx && ev->x < ivhit.qx + 3) show_help = 1;
            else if (ev->x >= ivhit.mx && ev->x < ivhit.mx + 3) interval_step(-1);
            else if (ev->x >= ivhit.px && ev->x < ivhit.px + 3) interval_step(1);
            return;
        }
        if (!net_click(ev->x, ev->y)) proc_mouse(ev->x, ev->y, 0);
    }
}

static void usage(FILE *f) {
    fputs("usage: flint [options]\n"
          "  -c, --config FILE   read FILE instead of ~/.config/flint/flint.ini\n"
          "  -h, --help          show this help\n"
          "keys:\n"
          "  1 2 3 4        toggle cpu / mem+disks / net / proc panels\n"
          "  up down        select process (pgup pgdn home end, mouse wheel)\n"
          "  s              sort menu; left right change column\n"
          "  enter          details of the selected process (double click a row too)\n"
          "  f              filter processes (enter keeps it, esc clears)\n"
          "  t  k           terminate (SIGTERM) / kill (SIGKILL) the selected process\n"
          "  b  n           previous / next network interface\n"
          "  + -            refresh interval +/- 100 ms\n"
          "  p              pause / resume\n"
          "  q ctrl+q       quit\n",
          f);
}

int main(int argc, char **argv) {
    const char *cfg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            if (++i >= argc) {
                usage(stderr);
                return 2;
            }
            cfg = argv[i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    const char *ct = getenv("COLORTERM");
    truecolor = ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit"));
    if (theme_load(cfg) < 0) {
        fprintf(stderr, "flint: cannot read config file '%s'\n", cfg);
        return 1;
    }
    const char *sp = state_path();
    if (sp) ini_parse(sp, state_cb, NULL);
    page_sz = sysconf(_SC_PAGESIZE);
    cpu_init();
    gpu_init();
    collect();
    install_signals();
    if (tb_init() != TB_OK) {
        fputs("flint: failed to initialise terminal\n", stderr);
        return 1;
    }
    tb_set_output_mode(truecolor ? TB_OUTPUT_TRUECOLOR : TB_OUTPUT_256);
    tb_set_clear_attrs(tcol(theme.fg), tcol(theme.bg));
    tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
    long next = now_ms() + 250;
    while (!quit_sig) {
        draw();
        long left = next - now_ms();
        if (paused || left > 250) left = 250;
        struct tb_event ev;
        int rc = tb_peek_event(&ev, left > 0 ? (int)left : 0);
        if (rc == TB_OK) {
            if (ev.type == TB_EVENT_KEY) {
                if (on_key(&ev)) break;
            } else if (ev.type == TB_EVENT_MOUSE) on_mouse(&ev);
        }
        if (!paused && now_ms() >= next) {
            collect();
            next = now_ms() + interval;
        }
    }
    tb_shutdown();
    return quit_sig ? 128 + quit_sig : 0;
}
