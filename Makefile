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
INC      = -Iinclude
BUILD    = build

HEADERS := $(wildcard include/*.h)

.PHONY: all check clean

all: check

check: $(BUILD)/header_smoke
	@$(BUILD)/header_smoke

$(BUILD)/header_smoke: tests/header_smoke.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(INC) tests/header_smoke.c -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
