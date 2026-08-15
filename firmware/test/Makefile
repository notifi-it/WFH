# Host build for the derive tests (§11.1). No ESP-IDF, no board.
#   make        build and run
#   make gen    regenerate actions.g.h / fixtures.g.h from JSON
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -I../main
ROOT     = ../..
SRCS     = test_derive.c ../main/derive.c ../main/config.c
GENERATED = ../main/actions.g.h fixtures.g.h

.PHONY: run gen check-gen clean
run: test_derive
	./test_derive

test_derive: $(SRCS) $(GENERATED)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

gen:
	node $(ROOT)/tools/gen-config.mjs
	node $(ROOT)/tools/gen-fixtures.mjs

# CI gate (§2.1): a hand-edit of a generated file fails the build.
check-gen: gen
	git -C $(ROOT) diff --exit-code -- firmware/main/actions.g.h firmware/test/fixtures.g.h

clean:
	rm -f test_derive
