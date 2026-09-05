#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

void config_defaults(struct proxy_config *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof *cfg);

    snprintf(cfg->listen_host, sizeof cfg->listen_host, "127.0.0.1");
    cfg->listen_port = 8080;
    cfg->backlog = 128;
    cfg->reuseport = 0;
    cfg->workers = 1;

    cfg->max_conns = 4096;
    cfg->max_head = 65536;
    cfg->buf_cap = 65536;

    cfg->connect_timeout_ms = 2000;
    cfg->idle_timeout_ms = 30000;

    cfg->algo = BAL_WEIGHTED_LEAST_CONN;

    cfg->health_enabled = 1;
    cfg->health_interval_ms = 3000;
    cfg->health_timeout_ms = 1000;
    cfg->health_ok_threshold = 2;
    cfg->health_fail_threshold = 3;
    snprintf(cfg->health_path, sizeof cfg->health_path, "/healthz");

    cfg->splice_threshold = 0; /* M7 fast path off by default */

    cfg->nbackends = 0;
}

static char *trim_whitespace(char *str)
{
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

int config_load(struct proxy_config *cfg, const char *path,
                char *err, size_t errsz)
{
    FILE *f;
    char line[512];
    int lineno = 0;

    if (cfg == NULL || path == NULL) {
        if (err && errsz) snprintf(err, errsz, "Invalid config_load parameters");
        return -1;
    }

    config_defaults(cfg);

    f = fopen(path, "r");
    if (f == NULL) {
        if (err && errsz) {
            snprintf(err, errsz, "Failed to open '%s': %s", path, strerror(errno));
        }
        return -1;
    }

    while (fgets(line, sizeof line, f) != NULL) {
        char *p, *key, *val;
        lineno++;

        /* Strip comments */
        p = strchr(line, '#');
        if (p != NULL) *p = '\0';

        p = trim_whitespace(line);
        if (*p == '\0') continue;

        key = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p != '\0') {
            *p = '\0';
            val = trim_whitespace(p + 1);
        } else {
            val = "";
        }

        if (strcmp(key, "listen") == 0) {
            char *colon = strrchr(val, ':');
            if (colon != NULL) {
                *colon = '\0';
                snprintf(cfg->listen_host, sizeof cfg->listen_host, "%s", val);
                cfg->listen_port = (uint16_t)atoi(colon + 1);
            } else {
                cfg->listen_port = (uint16_t)atoi(val);
            }
        } else if (strcmp(key, "workers") == 0) {
            cfg->workers = atoi(val);
        } else if (strcmp(key, "reuseport") == 0) {
            cfg->reuseport = atoi(val);
        } else if (strcmp(key, "backlog") == 0) {
            cfg->backlog = atoi(val);
        } else if (strcmp(key, "max_conns") == 0) {
            cfg->max_conns = (size_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "max_head") == 0) {
            cfg->max_head = (size_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "buf_cap") == 0) {
            cfg->buf_cap = (size_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "connect_timeout") == 0 ||
                   strcmp(key, "connect_timeout_ms") == 0) {
            cfg->connect_timeout_ms = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "idle_timeout") == 0 ||
                   strcmp(key, "idle_timeout_ms") == 0) {
            cfg->idle_timeout_ms = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "balance") == 0) {
            if (strcmp(val, "round-robin") == 0) {
                cfg->algo = BAL_ROUND_ROBIN;
            } else if (strcmp(val, "weighted-least-connections") == 0) {
                cfg->algo = BAL_WEIGHTED_LEAST_CONN;
            } else {
                if (err && errsz) {
                    snprintf(err, errsz, "Line %d: unknown balance algorithm '%s'", lineno, val);
                }
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "health_enabled") == 0) {
            cfg->health_enabled = atoi(val);
        } else if (strcmp(key, "health_interval") == 0 ||
                   strcmp(key, "health_interval_ms") == 0) {
            cfg->health_interval_ms = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "health_timeout") == 0 ||
                   strcmp(key, "health_timeout_ms") == 0) {
            cfg->health_timeout_ms = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "health_ok_threshold") == 0) {
            cfg->health_ok_threshold = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "health_fail_threshold") == 0) {
            cfg->health_fail_threshold = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "health_path") == 0) {
            snprintf(cfg->health_path, sizeof cfg->health_path, "%s", val);
        } else if (strcmp(key, "splice_threshold") == 0) {
            cfg->splice_threshold = (size_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "backend") == 0) {
            char target[128] = {0};
            int weight = 1;
            char *colon, *w_str;
            struct backend_cfg *b;

            if (cfg->nbackends >= PX_MAX_BACKENDS) {
                if (err && errsz) {
                    snprintf(err, errsz, "Line %d: maximum backends (%d) exceeded",
                             lineno, PX_MAX_BACKENDS);
                }
                fclose(f);
                return -1;
            }

            /* Format: backend host:port [weight=N] */
            w_str = strstr(val, "weight=");
            if (w_str != NULL) {
                weight = atoi(w_str + 7);
                if (weight < 1) weight = 1;
                *w_str = '\0';
                val = trim_whitespace(val);
            }

            snprintf(target, sizeof target, "%s", val);
            colon = strrchr(target, ':');
            if (colon == NULL) {
                if (err && errsz) {
                    snprintf(err, errsz, "Line %d: invalid backend format (expected host:port)", lineno);
                }
                fclose(f);
                return -1;
            }
            *colon = '\0';

            b = &cfg->backends[cfg->nbackends++];
            snprintf(b->host, sizeof b->host, "%s", target);
            b->port = (uint16_t)atoi(colon + 1);
            b->weight = weight;
        } else {
            /* Unknown directive */
            if (err && errsz) {
                snprintf(err, errsz, "Line %d: unrecognized directive '%s'", lineno, key);
            }
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}
