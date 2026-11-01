/* ini.c - minimal INI parser */
#include "flint.h"

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* strip inline ; or # comment */
static void cut_comment(char *s) {
    for (char *p = s; *p; p++) {
        int prev_ws = p == s || isspace((unsigned char)p[-1]);
        if (!prev_ws) continue;
        if (*p == ';' || (*p == '#' && (p[1] == 0 || isspace((unsigned char)p[1])))) {
            *p = 0;
            return;
        }
    }
}

static void norm_key(char *k) {
    char tmp[96];
    size_t n = 0;
    int pend = 0;
    for (const char *p = k; *p && n < sizeof tmp - 2; p++) {
        if (isspace((unsigned char)*p) || *p == '-' || *p == '_') {
            pend = n > 0;
            continue;
        }
        if (pend) tmp[n++] = '_', pend = 0;
        tmp[n++] = (char)tolower((unsigned char)*p);
    }
    tmp[n] = 0;
    memcpy(k, tmp, n + 1);
}

int ini_parse(const char *path, ini_cb cb, void *ud) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], section[48] = "";
    int lineno = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        if (lineno == 1 && !strncmp(s, "\xef\xbb\xbf", 3)) s += 3; /* skip UTF-8 BOM */
        s = trim(s);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) {
                bad++;
                continue;
            }
            *e = 0;
            snprintf(section, sizeof section, "%s", trim(s + 1));
            for (char *p = section; *p; p++) *p = (char)tolower((unsigned char)*p);
            continue;
        }
        cut_comment(s);
        s = trim(s);
        char *key = s, *val = NULL;
        char *sep = strpbrk(s, "=:");
        if (sep) {
            *sep = 0;
            val = trim(sep + 1);
            key = trim(key);
        } else { /* "key value": last word is the value */
            char *e = s + strlen(s);
            while (e > s && !isspace((unsigned char)e[-1])) e--;
            if (e == s) {
                bad++;
                continue;
            }
            val = e;
            *(e - 1) = 0;
            key = trim(s);
        }
        size_t vl = strlen(val);
        if (vl >= 2 && (val[0] == '"' || val[0] == '\'') && val[vl - 1] == val[0]) {
            val[vl - 1] = 0;
            val++;
        }
        norm_key(key);
        if (!*key) {
            bad++;
            continue;
        }
        cb(ud, section, key, val);
    }
    fclose(f);
    return bad;
}
