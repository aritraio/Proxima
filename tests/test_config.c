#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

int main(void)
{
    struct proxy_config cfg;
    char err[256];
    const char *tmp_conf = "/tmp/test_pxlb.conf";
    FILE *f;

    config_defaults(&cfg);
    assert(cfg.listen_port == 8080);
    assert(strcmp(cfg.listen_host, "127.0.0.1") == 0);
    assert(cfg.algo == BAL_WEIGHTED_LEAST_CONN);

    /* Test non-existent file */
    assert(config_load(&cfg, "/tmp/non_existent_file_123.conf", err, sizeof err) != 0);

    /* Write sample config file */
    f = fopen(tmp_conf, "w");
    assert(f != NULL);
    fprintf(f, "# Sample test config\n");
    fprintf(f, "listen          0.0.0.0:8000\n");
    fprintf(f, "workers         2\n");
    fprintf(f, "max_conns       1024\n");
    fprintf(f, "max_head        32768\n");
    fprintf(f, "buf_cap         16384\n");
    fprintf(f, "connect_timeout 1500\n");
    fprintf(f, "idle_timeout    45000\n");
    fprintf(f, "balance         round-robin\n");
    fprintf(f, "health_enabled  1\n");
    fprintf(f, "health_interval 5000\n");
    fprintf(f, "health_path     /ping\n");
    fprintf(f, "backend         127.0.0.1:9001 weight=1\n");
    fprintf(f, "backend         127.0.0.1:9002 weight=3 # secondary\n");
    fclose(f);

    assert(config_load(&cfg, tmp_conf, err, sizeof err) == 0);
    assert(strcmp(cfg.listen_host, "0.0.0.0") == 0);
    assert(cfg.listen_port == 8000);
    assert(cfg.workers == 2);
    assert(cfg.max_conns == 1024);
    assert(cfg.max_head == 32768);
    assert(cfg.buf_cap == 16384);
    assert(cfg.connect_timeout_ms == 1500);
    assert(cfg.idle_timeout_ms == 45000);
    assert(cfg.algo == BAL_ROUND_ROBIN);
    assert(cfg.health_enabled == 1);
    assert(cfg.health_interval_ms == 5000);
    assert(strcmp(cfg.health_path, "/ping") == 0);
    assert(cfg.nbackends == 2);
    assert(strcmp(cfg.backends[0].host, "127.0.0.1") == 0 && cfg.backends[0].port == 9001 && cfg.backends[0].weight == 1);
    assert(strcmp(cfg.backends[1].host, "127.0.0.1") == 0 && cfg.backends[1].port == 9002 && cfg.backends[1].weight == 3);

    unlink(tmp_conf);
    printf("test_config: ALL PASS\n");
    return 0;
}
