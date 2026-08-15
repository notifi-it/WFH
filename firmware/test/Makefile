# Host build for the derive tests (§11.1). No ESP-IDF, no board.
#   make        build and run
#   make gen    regenerate actions.g.h / fixtures.g.h from JSON
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -I../main
ROOT     = ../..
CORE     = ../main/derive.c ../main/config.c
GENERATED = ../main/actions.g.h fixtures.g.h

.PHONY: run gen check-gen clean
run: test_derive test_card
	./test_derive
	@echo
	./test_card

test_derive: test_derive.c $(CORE) $(GENERATED)
	$(CC) $(CFLAGS) -o $@ test_derive.c $(CORE)

test_card: test_card.c ../main/card.c $(CORE) $(GENERATED)
	$(CC) $(CFLAGS) -o $@ test_card.c ../main/card.c $(CORE)

gen:
	node $(ROOT)/tools/gen-config.mjs
	node $(ROOT)/tools/gen-fixtures.mjs

# CI gate (§2.1): a hand-edit of a generated file fails the build.
check-gen: gen
	git -C $(ROOT) diff --exit-code -- firmware/main/actions.g.h firmware/test/fixtures.g.h

clean:
	rm -f test_derive test_card
