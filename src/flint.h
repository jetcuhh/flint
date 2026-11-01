/* flint.h - shared types and interfaces */
#ifndef FLINT_H
#define FLINT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define TB_OPT_ATTR_W 32
#include "termbox2.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t u32;

/* theme.c */
enum { P_BLACK, P_RED, P_GREEN, P_YELLOW, P_BLUE, P_MAGENTA, P_CYAN, P_WHITE, P_LBLACK, P_LRED, P_LGREEN, P_LYELLOW,
       P_LBLUE, P_LMAGENTA, P_LCYAN, P_LWHITE };
typedef struct {
    u32 bg, fg;
    u32 tile, tile_proc, text, dim, bright, rule, track, keycap;
    u32 sel_bg, sel_text, accent, warn, hot, meter, meter_dim, popup_bg, idle;
    u32 mark_cpu, mark_mem, mark_net, mark_proc, net_up;
    u32 pal[16]; /* the 16 palette colours */
} Theme;
extern Theme theme;
int theme_load(const char *path); /* NULL = default path */
const char *config_default_path(void);
int parse_color(const char *s, u32 *out);

#define C_TEXT (theme.text)
#define C_DIM (theme.dim)
#define C_BRIGHT (theme.bright)
#define C_ACCENT (theme.accent)
#define BOLD(c) ((c) | TB_BOLD)

/* ini.c */
typedef void (*ini_cb)(void *ud, const char *section, const char *key, const char *val);
int ini_parse(const char *path, ini_cb cb, void *ud); /* -1 open error, >0 bad lines */

/* history ring */
#define HN 1024
typedef struct {
    float v[HN];
    int n;
} Hist;

/* sort columns, in the order they are drawn */
enum { S_PID, S_PROG, S_CMD, S_USER, S_THR, S_MEM, S_CPU, S_N };

/* ui.c */
extern int W, H;      /* terminal size */
extern int truecolor; /* 24-bit colour available */
extern u32 cur_bg;    /* bg for cells drawn with 0 */
u32 rgb(int r, int g, int b);
u32 mix(u32 a, u32 b, float t);
u32 tcol(u32 c); /* colour for the output mode */
u32 heat_c(float pct, u32 base); /* base below 60, warn to 84, hot from 85 */
void hpush(Hist *h, float x);
int slurp(const char *path, char *buf, size_t n);
void *xrealloc(void *p, size_t n);
void mkdirs(const char *path);
void fmt_bytes(char *o, size_t n, double b);
void fmt_rate(char *o, size_t n, double b);
void fmt_bits(char *o, size_t n, double bytes);
void fmt_mem(char *o, size_t n, double b);
void cell(int x, int y, u32 ch, u32 fg, u32 bg);
int txt(int x, int y, int maxw, u32 fg, u32 bg, const char *s);
int ucols(const char *s);
void rtxt(int xr, int y, u32 fg, u32 bg, const char *s);
void fill(int x, int y, int w, int h, u32 bg);
int tile(int x, int y, int w, int h, u32 bg);
int label(int x, int y, int w, u32 mark, const char *name, const char *const *info, int n, u32 ifg, int *iw_out);
int keycap(int x, int y, const char *s, u32 bg, u32 fg);
void bar(int x, int y, int w, float f, u32 col);
void graph(int x, int y, int w, int h, const float *v, int n, float scale, u32 lo, u32 mid, u32 hi);

/* flint.c */
extern int interval; /* refresh, ms */
extern int paused;   /* freeze updates */
void interval_step(int dir);
long now_ms(void);
extern long page_sz;

/* cpu.c */
extern unsigned long long dtotal_all; /* total jiffies since last update */
void cpu_init(void);
void cpu_update(void);
void cpu_draw(int x, int y, int w, int h);
int cpu_rows(void); /* rows the cpu tile wants */
double cpu_uptime(void);

/* gpu.c */
void gpu_init(void);
void gpu_update(void);
int gpu_available(void);
extern Hist gpu_hist; /* one sample per refresh */
float gpu_pct(void);  /* 0..100 */
float gpu_temp(void); /* deg C, or -1 */

/* mem.c, disk.c */
extern double m_swt, m_swf; /* swap total/free, bytes */
void mem_update(void);
void mem_draw(int x, int y, int w, int h);
void memdisk_draw(int x, int y, int w, int h);
int memdisk_rows(int w); /* rows wanted at width w */
void disk_update(void);
void disk_draw(int x, int y, int w, int h);
int disk_count(void);

/* net.c */
void net_update(double dt);
void net_draw(int x, int y, int w, int h);
void net_switch(int dir);
int net_click(int mx, int my);
void net_hide(void);

/* proc.c */
extern int sort_key, sel_pid, nview, filter_edit, confirm_sig, confirm_pid, sort_menu, info_open;
extern char filter[48], status_msg[96];
extern long status_at;
void proc_update(void);
void proc_draw(int x, int y, int w, int h);
void proc_move(int d);
void proc_deselect(void);
void proc_scroll(int d);
void proc_rebuild(void);
void proc_signal(int pid, int sig);
void proc_mouse(int mx, int my, int drag);
void proc_release(void);
void proc_hide(void);
void proc_ask(int sig);
void proc_popup(void);
void proc_sort_open(void);
void proc_info_open(void);
void proc_sort_key(const struct tb_event *ev);
void proc_sort_click(int mx, int my);
void proc_confirm_click(int mx, int my);

#endif
