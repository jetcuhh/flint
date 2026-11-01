/* cpu.c - cpu panel */
#include "flint.h"

typedef struct {
    unsigned long long v[8];
} Jif;
static int ncpu;
static Jif jt_prev, *jc_prev;
static float *cpu_pct; /* 0 = total, then cores */
static Hist *cpu_hist;
static int *cpu_core_id;
static unsigned char *cpu_ok, *cpu_seen; /* per core: valid prev sample, seen this read */
static int cpu_primed;
unsigned long long dtotal_all;
static char cpu_model[64];
static float cpu_mhz;
static char pkg_path[160];
#define MAXPKG 8
static char core_tpath[MAXPKG][256][48];
static char *cpu_tpath; /* core temp paths */
static float temp_pkg = -1, *temp_core;
static double uptime_s, load[3];

static void strip_all(char *s, const char *pat) {
    size_t l = strlen(pat);
    char *p;
    while ((p = strstr(s, pat))) memmove(p, p + l, strlen(p + l) + 1);
}
static void shorten_model(char *s) {
    static const char *junk[] = {"Intel", "AMD", "CPU", "Processor", "Genuine", NULL};
    strip_all(s, "(R)");
    strip_all(s, "(TM)");
    char *at = strchr(s, '@');
    if (at) *at = 0;
    char *w = strstr(s, " with ");
    if (w) *w = 0;
    char out[64] = "", *save, *tok;
    for (tok = strtok_r(s, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        size_t tl = strlen(tok);
        int skip = tl > 5 && !strcmp(tok + tl - 5, "-Core");
        for (int k = 0; junk[k]; k++)
            if (!strcmp(tok, junk[k])) skip = 1;
        if (skip) continue;
        if (out[0]) strncat(out, " ", sizeof out - strlen(out) - 1);
        strncat(out, tok, sizeof out - strlen(out) - 1);
    }
    snprintf(s, 64, "%s", out[0] ? out : "CPU");
}

static void read_cpuinfo(int first) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[512];
    double sum = 0;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        /* x86 "model name", arm "Model"/"Hardware" */
        if (first && !cpu_model[0] &&
            (!strncmp(line, "model name", 10) || !strncmp(line, "Model\t", 6) || !strncmp(line, "Hardware", 8))) {
            char *c = strchr(line, ':');
            if (c) {
                c++;
                while (*c == ' ') c++;
                c[strcspn(c, "\n")] = 0;
                snprintf(cpu_model, sizeof cpu_model, "%s", c);
                shorten_model(cpu_model);
            }
        }
        if (!strncmp(line, "cpu MHz", 7)) {
            char *c = strchr(line, ':');
            if (c) sum += atof(c + 1), n++;
        }
    }
    fclose(f);
    if (n) cpu_mhz = (float)(sum / n);
    else { /* arm: cpufreq, in kHz */
        char b[32];
        if (slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", b, sizeof b) > 0) cpu_mhz = (float)(atof(b) / 1000.0);
    }
}

static int cpu_slot(int id, int hint) {
    if (hint < ncpu && cpu_core_id[hint] == id) return hint;
    for (int i = 0; i < ncpu; i++)
        if (cpu_core_id[i] == id) return i;
    return -1;
}

static void read_stat(void) {
    static char buf[1 << 16];
    if (slurp("/proc/stat", buf, sizeof buf) < 0) return;
    memset(cpu_seen, 0, (size_t)ncpu);
    char *p = buf;
    int nline = 0;
    while (p && *p) {
        if (!strncmp(p, "cpu", 3)) {
            Jif j;
            memset(&j, 0, sizeof j);
            char *q = p + 3;
            int total = (*q == ' '), slot = -1;
            if (!total) slot = cpu_slot((int)strtol(q, &q, 10), nline++);
            sscanf(q, " %llu %llu %llu %llu %llu %llu %llu %llu", &j.v[0], &j.v[1], &j.v[2], &j.v[3], &j.v[4],
                   &j.v[5], &j.v[6], &j.v[7]);
            Jif *pv = total ? &jt_prev : (slot >= 0 ? &jc_prev[slot] : NULL);
            if (pv) {
                int valid = total ? cpu_primed : cpu_ok[slot];
                unsigned long long tot = 0, d;
                for (int i = 0; i < 8; i++) tot += j.v[i] - pv->v[i];
                d = (j.v[3] - pv->v[3]) + (j.v[4] - pv->v[4]);
                float pct = (valid && tot) ? (float)(tot - d) / (float)tot : 0;
                int ci = total ? 0 : slot + 1;
                cpu_pct[ci] = pct;
                if (valid) hpush(&cpu_hist[ci], pct);
                if (total) dtotal_all = valid ? tot : 0;
                *pv = j;
                if (!total) cpu_seen[slot] = 1;
            }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    for (int i = 0; i < ncpu; i++) {
        if (!cpu_seen[i]) cpu_pct[i + 1] = 0;
        cpu_ok[i] = cpu_seen[i];
    }
    cpu_primed = 1;
}

static float rd_temp(const char *p) {
    char b[32];
    if (!p[0] || slurp(p, b, sizeof b) <= 0) return -1;
    return atoi(b) / 1000.0f;
}

static void init_temps(void) {
    for (int h = 0; h < 32; h++) {
        char path[200], name[64];
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
        if (slurp(path, name, sizeof name) <= 0) continue;
        name[strcspn(name, "\n")] = 0;
        int core = !strcmp(name, "coretemp"), amd = !strcmp(name, "k10temp") || !strcmp(name, "zenpower");
        if (!core && !amd) continue;
        int pkg = 0;
        for (int k = 1; k < 128 && core; k++) { /* one coretemp per package */
            char lp[200], lab[64];
            snprintf(lp, sizeof lp, "/sys/class/hwmon/hwmon%d/temp%d_label", h, k);
            if (slurp(lp, lab, sizeof lab) > 0 && !strncmp(lab, "Package id ", 11)) {
                pkg = atoi(lab + 11);
                break;
            }
        }
        if (pkg < 0 || pkg >= MAXPKG) pkg = -1;
        for (int k = 1; k < 128; k++) {
            char lp[200], ip[200], lab[64];
            snprintf(lp, sizeof lp, "/sys/class/hwmon/hwmon%d/temp%d_label", h, k);
            snprintf(ip, sizeof ip, "/sys/class/hwmon/hwmon%d/temp%d_input", h, k);
            if (slurp(lp, lab, sizeof lab) <= 0) continue;
            if ((!strncmp(lab, "Package", 7) && !pkg_path[0]) || !strncmp(lab, "Tctl", 4) ||
                (!pkg_path[0] && !strncmp(lab, "Tdie", 4)))
                snprintf(pkg_path, sizeof pkg_path, "%s", ip);
            else if (!strncmp(lab, "Core ", 5) && pkg >= 0) {
                int id = atoi(lab + 5);
                if (id >= 0 && id < 256) snprintf(core_tpath[pkg][id], sizeof core_tpath[pkg][id], "%s", ip);
            }
        }
        if (amd && pkg_path[0]) break;
    }
    if (!pkg_path[0]) {
        for (int z = 0; z < 32; z++) {
            char path[200], ty[64];
            snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
            if (slurp(path, ty, sizeof ty) <= 0) continue;
            snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
            if (!strncmp(ty, "x86_pkg_temp", 12)) {
                snprintf(pkg_path, sizeof pkg_path, "%s", path);
                break;
            }
            if (!pkg_path[0]) snprintf(pkg_path, sizeof pkg_path, "%s", path);
        }
    }
    for (int i = 0; i < ncpu; i++) {
        char path[200], b[16];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu_core_id[i]);
        if (slurp(path, b, sizeof b) <= 0) continue;
        int id = atoi(b), pkg = 0;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu_core_id[i]);
        if (slurp(path, b, sizeof b) > 0) pkg = atoi(b);
        if (id >= 0 && id < 256 && pkg >= 0 && pkg < MAXPKG)
            snprintf(cpu_tpath + (size_t)i * 160, 160, "%s", core_tpath[pkg][id]);
    }
}

void cpu_init(void) {
    static char buf[1 << 16];
    int n = slurp("/proc/stat", buf, sizeof buf);
    ncpu = 0;
    if (n > 0)
        for (char *p = buf; p; p = strchr(p, '\n')) {
            if (*p == '\n') p++;
            if (!strncmp(p, "cpu", 3) && isdigit((unsigned char)p[3])) ncpu++;
        }
    if (ncpu < 1) ncpu = 1;
    jc_prev = calloc(ncpu, sizeof(Jif));
    cpu_pct = calloc(ncpu + 1, sizeof(float));
    cpu_hist = calloc(ncpu + 1, sizeof(Hist));
    cpu_core_id = calloc(ncpu, sizeof(int));
    cpu_ok = calloc(ncpu, 1);
    cpu_seen = calloc(ncpu, 1);
    temp_core = calloc(ncpu, sizeof(float));
    cpu_tpath = calloc(ncpu, 160);
    int slot = 0;
    for (char *p = buf; p && slot < ncpu; p = strchr(p, '\n')) {
        if (*p == '\n') p++;
        if (!strncmp(p, "cpu", 3) && isdigit((unsigned char)p[3])) cpu_core_id[slot++] = atoi(p + 3);
    }
    read_cpuinfo(1);
    if (!cpu_model[0]) {
        char b[64];
        if (slurp("/proc/device-tree/model", b, sizeof b) > 0 && b[0]) snprintf(cpu_model, sizeof cpu_model, "%s", b);
    }
    if (!cpu_model[0]) snprintf(cpu_model, sizeof cpu_model, "CPU");
    init_temps();
}

static void read_cpu_misc(void) {
    char b[256];
    if (slurp("/proc/uptime", b, sizeof b) > 0) uptime_s = atof(b);
    if (slurp("/proc/loadavg", b, sizeof b) > 0) sscanf(b, "%lf %lf %lf", &load[0], &load[1], &load[2]);
    { /* cpuinfo is slow, refresh rarely */
        static time_t last;
        time_t now = time(NULL);
        if (now - last >= 3 || now < last) {
            read_cpuinfo(0);
            last = now;
        }
    }
    temp_pkg = rd_temp(pkg_path);
    for (int i = 0; i < ncpu; i++) temp_core[i] = rd_temp(cpu_tpath + (size_t)i * 160);
}

void cpu_update(void) {
    read_stat();
    read_cpu_misc();
}

double cpu_uptime(void) { return uptime_s; }

#define CPU_GH 4 /* max graph rows */
/* rows wanted: label, graph, cores, load, gpu, blank */
int cpu_rows(void) { return 1 + CPU_GH + (ncpu + 1) / 2 + 1 + (gpu_available() ? 1 : 0) + 1 + !truecolor; }

static void join(char *o, size_t n, const char *p, const char *q) {
    if (p[0] && q[0]) snprintf(o, n, "%s \xc2\xb7 %s", p, q);
    else snprintf(o, n, "%s", p[0] ? p : q);
}

/* name, bar (max 14), value, temp */
static void meter_row(int x, int y, int cw, int lw, const char *name, float pct, float temp, u32 base) {
    char b[16];
    int tw = temp >= 0 ? 4 : 0, m = cw - lw - 4 - tw;
    if (m < 6 && tw) tw = 0, m = cw - lw - 4;
    if (m > 14) m = 14;
    txt(x, y, lw, C_DIM, 0, name);
    if (m < 1) return;
    bar(x + lw, y, m, pct / 100, heat_c(pct, base));
    snprintf(b, sizeof b, "%3.0f", pct);
    txt(x + lw + m + 1, y, 3, heat_c(pct, C_TEXT), 0, b);
    if (tw) {
        snprintf(b, sizeof b, " %.0f\xc2\xb0", temp);
        txt(x + lw + m + 4, y, 4, heat_c(temp - 10, C_DIM), 0, b);
    }
}

void cpu_draw(int x, int y, int w, int h) {
    int t = tile(x, y, w, h, theme.tile);
    y += t, h -= t;
    char clk[24] = "", tmp[16] = "", rest[48], full[128];
    if (cpu_mhz >= 1000) snprintf(clk, sizeof clk, "%.1f GHz", cpu_mhz / 1000);
    else if (cpu_mhz > 0) snprintf(clk, sizeof clk, "%.0f MHz", cpu_mhz);
    if (temp_pkg >= 0) snprintf(tmp, sizeof tmp, "%.0f\xc2\xb0", temp_pkg);
    join(rest, sizeof rest, clk, tmp);
    join(full, sizeof full, cpu_model, rest);
    const char *info[] = {full, rest, tmp};
    label(x, y, w, theme.mark_cpu, "cpu", info, 3, C_DIM, NULL);

    int ix = x + 1, iw = w - 2;
    if (iw < 8) return;
    int gr = gpu_available() ? 1 : 0, cores = (ncpu + 1) / 2;
    int a = h - 1; /* rows below label */
    int base = cores + 1 + gr; /* cores, load, gpu */
    if (a >= base + CPU_GH + 1) a--; /* drop the blank row first */
    int gh = a >= base + 1 ? (a - base < CPU_GH ? a - base : CPU_GH) : 1; /* graph takes spare rows */
    int has_load = a >= gh + 1, has_gpu = gr && a >= gh + 1 + has_load;
    int corerows = a - gh - has_load - has_gpu;
    if (corerows > cores) corerows = cores;
    if (corerows < 0) corerows = 0;
    if (a < 1) return;

    char b[48];
    int r = y + 1;
    float tot = cpu_pct[0] * 100;
    snprintf(b, sizeof b, "%.0f%%", tot);
    txt(ix, r, 4, BOLD(heat_c(tot, C_BRIGHT)), 0, b);
    graph(ix + 6, r, iw - 6, gh, cpu_hist[0].v, cpu_hist[0].n, 1.0f, theme.pal[P_CYAN], theme.pal[P_YELLOW], theme.pal[P_RED]);
    r += gh;

    int cwid = (iw - 2) / 2, lw = ncpu > 10 ? 4 : 3;
    for (int i = 0; i < ncpu && i / 2 < corerows; i++) {
        int cx = ix + (i % 2) * (cwid + 2);
        snprintf(b, sizeof b, "c%d", cpu_core_id[i]);
        meter_row(cx, r + i / 2, cwid, lw, b, cpu_pct[i + 1] * 100, temp_core[i], theme.pal[P_CYAN]);
    }
    r += corerows;

    if (has_load) {
        txt(ix, r, 4, C_DIM, 0, "load");
        int lx = ix + 6;
        for (int k = 0; k < 3; k++) {
            snprintf(b, sizeof b, "%.2f", load[k]);
            int vl = (int)strlen(b);
            if (lx + vl > ix + iw) break;
            u32 c = k ? C_DIM : BOLD(heat_c((float)(load[0] / ncpu * 100), C_BRIGHT));
            txt(lx, r, vl, c, 0, b);
            lx += vl + 2;
        }
        r++;
    }
    if (has_gpu) {
        meter_row(ix, r, iw, 4, "gpu", gpu_pct(), gpu_temp(), theme.pal[P_MAGENTA]);
        /* gpu graph right of the meter */
        int tw = gpu_temp() >= 0 ? 4 : 0, m = iw - 8 - tw;
        if (m < 6 && tw) tw = 0, m = iw - 8;
        if (m > 14) m = 14;
        int gx = ix + 4 + (m > 0 ? m : 0) + 4 + tw + 2, gw = ix + iw - gx;
        if (gw >= 6) graph(gx, r, gw, 1, gpu_hist.v, gpu_hist.n, 100.0f, theme.pal[P_MAGENTA], 0, theme.pal[P_LMAGENTA]);
    }
}
