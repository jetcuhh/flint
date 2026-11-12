#include "flint.h"

Theme theme;

static const char default_ini[] =
#include "defcfg.h"
    ;

static const char *pal_names[16] = {"black",       "red",       "green",       "yellow",       "blue",
                                    "magenta",     "cyan",      "white",       "light_black",  "light_red",
                                    "light_green", "light_yellow", "light_blue", "light_magenta", "light_cyan",
                                    "light_white"};
static const u32 pal_default[16] = {0x222733, 0xE37B66, 0x8DA882, 0xE0A568, 0x8999AA, 0xA58AA8, 0x809D9E, 0xC6CED8,
                                    0x98A0AE, 0xF09A88, 0xA9C29F, 0xEBBE8A, 0xA5B3C2, 0xBFA6C2, 0x9DB8B9, 0xEAEFF5};
#define BG_DEFAULT 0x101318
#define FG_DEFAULT 0xEAEFF5

typedef struct {
    char sec[16], key[40], val[64];
} KV;
static KV kv[320];
static int nkv, kv_dropped;

static void on_kv(void *ud, const char *sec, const char *key, const char *val) {
    (void)ud;
    if (nkv >= (int)(sizeof kv / sizeof kv[0])) {
        kv_dropped++;
        return;
    }
    KV *e = &kv[nkv++];
    snprintf(e->sec, sizeof e->sec, "%s", sec);
    snprintf(e->key, sizeof e->key, "%s", key);
    snprintf(e->val, sizeof e->val, "%s", val);
}
static const char *lookup(const char *sec, const char *key) {
    const char *r = NULL;
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].sec, sec) && !strcmp(kv[i].key, key)) r = kv[i].val;
    return r;
}

int parse_color(const char *s, u32 *out) {
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    size_t n = strlen(s);
    if (n != 6 && n != 3) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    u32 v = (u32)strtoul(s, NULL, 16);
    if (n == 3) v = ((v & 0xf00) * 0x1100) | ((v & 0x0f0) * 0x110) | ((v & 0x00f) * 0x11);
    *out = v ? v : 1;
    return 1;
}
static u32 get(const char *key, u32 def) {
    const char *v = lookup("theme", key);
    u32 c;
    return v && parse_color(v, &c) ? c : def;
}

static void build(void) {
    Theme *t = &theme;
    memset(t, 0, sizeof *t);
    u32 pal[16];
    t->bg = get("background", BG_DEFAULT);
    t->fg = get("foreground", FG_DEFAULT);
    for (int i = 0; i < 16; i++) pal[i] = get(pal_names[i], pal_default[i]);
    memcpy(t->pal, pal, sizeof pal);
    const u32 *p = pal;
    const u32 white = 0xFFFFFF;

    t->tile = get("tile", mix(t->bg, white, 0.04f));
    t->tile_proc = get("tile_proc", mix(t->bg, white, 0.06f));
    t->text = get("text", p[P_WHITE]);
    t->dim = get("dim", p[P_LBLACK]);
    t->bright = get("bright", t->fg);
    t->rule = get("rule", mix(t->bg, p[P_BLACK], 0.65f));
    t->track = get("track", mix(t->bg, t->dim, 0.3f));
    t->keycap = get("keycap", mix(t->bg, p[P_BLACK], 0.6f));
    t->sel_bg = get("select_bg", mix(t->bg, p[P_BLUE], 0.3f));
    t->sel_text = get("select_text", mix(t->bright, white, 0.4f));
    t->accent = get("accent", p[P_LCYAN]);
    t->warn = get("warn", p[P_YELLOW]);
    t->hot = get("hot", p[P_RED]);
    t->meter = get("meter", mix(t->text, t->dim, 0.25f));
    t->meter_dim = get("meter_dim", mix(t->dim, t->bg, 0.15f));
    t->popup_bg = get("popup_bg", mix(t->bg, p[P_BLACK], 0.8f));
    t->idle = get("proc_idle", mix(t->dim, t->bg, 0.1f));
    t->mark_cpu = get("mark_cpu", p[P_BLUE]);
    t->mark_mem = get("mark_mem", p[P_MAGENTA]);
    t->mark_net = get("mark_net", p[P_CYAN]);
    t->mark_proc = get("mark_proc", p[P_LBLUE]);
    t->net_up = get("net_up", p[P_GREEN]);
}

const char *config_default_path(void) {
    static char path[512];
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(path, sizeof path, "%s/flint/flint.ini", x);
    else if (h && *h) snprintf(path, sizeof path, "%s/.config/flint/flint.ini", h);
    else return NULL;
    return path;
}

static void make_default_config(const char *path) {
    if (access(path, F_OK) == 0) return;
    mkdirs(path);
    FILE *f = fopen(path, "wx");
    if (!f) return;
    fputs(default_ini, f);
    fclose(f);
}

int theme_load(const char *path) {
    int explicit_ = path != NULL;
    nkv = kv_dropped = 0;
    if (!path) {
        path = config_default_path();
        if (path) make_default_config(path);
    }
    int rc = 0;
    if (path) {
        int r = ini_parse(path, on_kv, NULL);
        if (r < 0 && explicit_) rc = -1;
        else if (r > 0) fprintf(stderr, "flint: %d malformed line(s) in %s\n", r, path);
    }
    if (kv_dropped) fprintf(stderr, "flint: too many settings in config, ignored the last %d\n", kv_dropped);
    build();
    const char *v = lookup("general", "refresh_ms");
    if (v) {
        int n = atoi(v);
        if (n >= 100 && n <= 10000) interval = n;
    }
    for (int i = 0; i < nkv; i++) {
        u32 c;
        if (!strcmp(kv[i].sec, "theme") && !parse_color(kv[i].val, &c))
            fprintf(stderr, "flint: bad colour '%s' for '%s'\n", kv[i].val, kv[i].key);
    }
    return rc;
}
