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

check: $(BUILD)/header_smoke $(BUILD)/test_buf $(BUILD)/test_http $(BUILD)/test_event
	@$(BUILD)/header_smoke
	@$(BUILD)/test_buf
	@$(BUILD)/test_http
	@$(BUILD)/test_event

$(BUILD)/header_smoke: tests/header_smoke.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/header_smoke.c -o $@

$(BUILD)/test_buf: tests/test_buf.c src/buf.c include/buf.h include/proxy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_buf.c src/buf.c -o $@

$(BUILD)/test_http: tests/test_http.c src/http.c include/http.h include/proxy.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_http.c src/http.c -o $@

$(BUILD)/test_event: tests/test_event.c src/event.c include/event.h include/conn.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SAN) $(INC) tests/test_event.c src/event.c -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
