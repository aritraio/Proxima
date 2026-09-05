# pxlb -- L7 HTTP reverse proxy / load balancer.
#
# M0 status: headers + smoke test only.  src/*.c (Linux-only epoll code)
# is added milestone by milestone per DESIGN.md; targets below grow with it.
#
# Hosts without Linux epoll headers can still run `make check` because
# the public headers never include Linux-only headers.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
WARN     = -Wall -Wextra -Wpedantic -Werror
SAN      = -fsanitize=address,undefined
INC      = -Iinclude
BUILD    = build

HEADERS := $(wildcard include/*.h)

.PHONY: all check clean

all: check

check: $(BUILD)/header_smoke $(BUILD)/test_buf $(BUILD)/test_http $(BUILD)/test_event $(BUILD)/test_metrics $(BUILD)/test_pool $(BUILD)/test_pipe_pool $(BUILD)/test_config $(BUILD)/test_health $(BUILD)/test_conn
	@$(BUILD)/header_smoke
	@$(BUILD)/test_buf
	@$(BUILD)/test_http
	@$(BUILD)/test_event
	@$(BUILD)/test_metrics
	@$(BUILD)/test_pool
	@$(BUILD)/test_pipe_pool
	@$(BUILD)/test_config
	@$(BUILD)/test_health
	@$(BUILD)/test_conn

$(BUILD)/header_smoke: tests/header_smoke.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/header_smoke.c -o $@

$(BUILD)/test_buf: tests/test_buf.c src/buf.c include/buf.h include/proxy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_buf.c src/buf.c -o $@

$(BUILD)/test_http: tests/test_http.c src/http.c include/http.h include/proxy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_http.c src/http.c -o $@

$(BUILD)/test_event: tests/test_event.c src/event.c include/event.h include/conn.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_event.c src/event.c -o $@

$(BUILD)/test_metrics: tests/test_metrics.c src/metrics.c include/metrics.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_metrics.c src/metrics.c -o $@

$(BUILD)/test_pool: tests/test_pool.c src/pool.c include/pool.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_pool.c src/pool.c -o $@

$(BUILD)/test_pipe_pool: tests/test_pipe_pool.c src/pipe_pool.c src/metrics.c include/pipe_pool.h include/metrics.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_pipe_pool.c src/pipe_pool.c src/metrics.c -o $@

$(BUILD)/test_config: tests/test_config.c src/config.c include/config.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_config.c src/config.c -o $@

$(BUILD)/test_health: tests/test_health.c src/health.c src/pool.c src/config.c src/event.c include/health.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_health.c src/health.c src/pool.c src/config.c src/event.c -o $@

$(BUILD)/test_conn: tests/test_conn.c src/conn.c src/buf.c src/http.c src/event.c src/metrics.c src/pool.c src/pipe_pool.c src/health.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_conn.c src/conn.c src/buf.c src/http.c src/event.c src/metrics.c src/pool.c src/pipe_pool.c src/health.c -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
