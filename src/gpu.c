#include "flint.h"
#include <sys/wait.h>

enum { G_NONE, G_SYSFS, G_NVIDIA, G_INTEL };
static int kind = G_NONE;
static char busy_path[200], temp_path[200], act_path[200], max_path[200], min_path[200], rc6_path[200];
static float pct, temp = -1;
Hist gpu_hist;

static float rd_milli(const char *p) {
    char b[32];
    if (!p[0] || slurp(p, b, sizeof b) <= 0) return -1;
    return atoi(b) / 1000.0f;
}

void gpu_init(void) {
    for (int c = 0; c < 8; c++) {
        char b[16];
        snprintf(busy_path, sizeof busy_path, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        if (slurp(busy_path, b, sizeof b) <= 0) continue;
        kind = G_SYSFS;
        for (int h = 0; h < 8; h++) {
            snprintf(temp_path, sizeof temp_path, "/sys/class/drm/card%d/device/hwmon/hwmon%d/temp1_input", c, h);
            if (slurp(temp_path, b, sizeof b) > 0) return;
        }
        temp_path[0] = 0;
        return;
    }
    busy_path[0] = 0;
    if (access("/proc/driver/nvidia", F_OK) == 0) {
        kind = G_NVIDIA;
        return;
    }
    static const char *const rc6[] = {"power/rc6_residency_ms", "gt/gt0/rc6_residency_ms"};
    static const char *const act[] = {"gt_act_freq_mhz", "gt/gt0/rps_act_freq_mhz", "gt_cur_freq_mhz"};
    static const char *const mx[] = {"gt_RP0_freq_mhz", "gt/gt0/rps_RP0_freq_mhz", "gt_max_freq_mhz"};
    static const char *const mn[] = {"gt_RPn_freq_mhz", "gt/gt0/rps_RPn_freq_mhz", "gt_min_freq_mhz"};
    for (int c = 0; c < 8; c++) {
        char b[16];
        int found = 0;
        for (int i = 0; i < 2 && !rc6_path[0]; i++) {
            snprintf(rc6_path, sizeof rc6_path, "/sys/class/drm/card%d/%s", c, rc6[i]);
            if (slurp(rc6_path, b, sizeof b) > 0) found = 1;
            else rc6_path[0] = 0;
        }
        for (int i = 0; i < 3 && !act_path[0]; i++) {
            snprintf(act_path, sizeof act_path, "/sys/class/drm/card%d/%s", c, act[i]);
            snprintf(max_path, sizeof max_path, "/sys/class/drm/card%d/%s", c, mx[i]);
            snprintf(min_path, sizeof min_path, "/sys/class/drm/card%d/%s", c, mn[i]);
            if (slurp(act_path, b, sizeof b) > 0 && slurp(max_path, b, sizeof b) > 0) {
                found = 1;
                if (slurp(min_path, b, sizeof b) <= 0) min_path[0] = 0;
            } else
                act_path[0] = max_path[0] = min_path[0] = 0;
        }
        if (found) {
            kind = G_INTEL;
            return;
        }
    }
}

static pid_t smi_pid;
static int smi_fd = -1;
static char smi_buf[128];
static size_t smi_len;
static time_t smi_start;
static int smi_fails;
static pid_t smi_zombie;

static void smi_reap(void) {
    if (smi_fd >= 0) close(smi_fd);
    smi_fd = -1;
    if (smi_pid > 0) {
        kill(smi_pid, SIGKILL);
        if (waitpid(smi_pid, NULL, WNOHANG) == 0 && !smi_zombie) smi_zombie = smi_pid;
    }
    smi_pid = 0;
}

static void smi_start_query(void) {
    int pf[2];
    if (pipe(pf) != 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(pf[0]);
        close(pf[1]);
        return;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 2);
        dup2(pf[1], 1);
        for (int fd = 3; fd < 256; fd++) close(fd);
        execlp("nvidia-smi", "nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu", "--format=csv,noheader,nounits",
               (char *)NULL);
        _exit(127);
    }
    close(pf[1]);
    fcntl(pf[0], F_SETFL, O_NONBLOCK);
    fcntl(pf[0], F_SETFD, FD_CLOEXEC);
    smi_pid = pid;
    smi_fd = pf[0];
    smi_len = 0;
    smi_start = time(NULL);
}

static void smi_poll(void) {
    if (smi_zombie && waitpid(smi_zombie, NULL, WNOHANG) != 0) smi_zombie = 0;
    if (smi_pid > 0) {
        for (;;) {
            ssize_t r = read(smi_fd, smi_buf + smi_len, sizeof smi_buf - 1 - smi_len);
            if (r > 0) {
                smi_len += (size_t)r;
                if (smi_len >= sizeof smi_buf - 1) r = 0;
                else continue;
            }
            if (r == 0) {
                smi_buf[smi_len] = 0;
                float u, t;
                if (sscanf(smi_buf, "%f, %f", &u, &t) == 2) pct = u, temp = t, smi_fails = 0;
                else if (++smi_fails >= 3) kind = G_NONE;
                smi_reap();
            } else if ((errno == EAGAIN || errno == EWOULDBLOCK) && time(NULL) - smi_start < 5) {
                return;
            } else {
                smi_reap();
            }
            break;
        }
    }
    if (smi_pid == 0 && !smi_zombie && kind == G_NVIDIA) smi_start_query();
}

void gpu_update(void) {
    if (kind == G_NONE) return;
    if (kind == G_SYSFS) {
        char b[16];
        if (slurp(busy_path, b, sizeof b) > 0) pct = (float)atoi(b);
        temp = rd_milli(temp_path);
    } else if (kind == G_NVIDIA) {
        smi_poll();
    } else if (kind == G_INTEL) {
        static long long rc6_prev = -1;
        static long rc6_t;
        static int rc6_moved;
        static float rc6_pct;
        char b[24];
        float a = act_path[0] && slurp(act_path, b, sizeof b) > 0 ? (float)atoi(b) : 0, hi = 0, lo = 0;
        if (max_path[0] && slurp(max_path, b, sizeof b) > 0) hi = (float)atoi(b);
        if (min_path[0] && slurp(min_path, b, sizeof b) > 0) lo = (float)atoi(b);
        float fq = a <= 0 || hi <= lo ? 0 : (a - lo) / (hi - lo) * 100;
        if (rc6_path[0] && slurp(rc6_path, b, sizeof b) > 0) {
            long long cur = atoll(b);
            long t = now_ms();
            if (rc6_prev >= 0 && t > rc6_t && cur >= rc6_prev) {
                if (cur > rc6_prev) rc6_moved = 1;
                rc6_pct = 100.0f - (float)(cur - rc6_prev) * 100.0f / (float)(t - rc6_t);
            }
            rc6_prev = cur, rc6_t = t;
        }
        pct = rc6_moved ? rc6_pct : fq;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        temp = -1;
    }
    hpush(&gpu_hist, pct);
}

int gpu_available(void) { return kind != G_NONE; }
float gpu_pct(void) { return pct; }
float gpu_temp(void) { return temp; }
