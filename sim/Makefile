# Host simulator: the real firmware UI, rendered to a PNG on the laptop.
#
#   make            build
#   make grid       render the grid
#   make popup      render the action popup
#
# Compiles ui.c unchanged. The stubs/ directory supplies just enough of
# ESP-IDF and the BSP for it to link off-target, so the simulator cannot
# drift from the firmware the way a re-implementation would.
CC      ?= cc
LVGL     = ../firmware/managed_components/lvgl__lvgl
FW       = ../firmware/main
CFLAGS   = -std=c11 -O0 -g -w -DLV_CONF_INCLUDE_SIMPLE -I. -Istubs -I$(LVGL) -I$(FW) -I$(FW)/icons
LDFLAGS  = -lz

LVGL_SRC = $(shell find $(LVGL)/src -name '*.c')
SRC      = main.c $(FW)/ui.c $(FW)/card.c $(FW)/config.c $(FW)/derive.c $(FW)/icons/icons.c $(LVGL_SRC)

board: $(SRC) lv_conf.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

grid: board
	./board grid.png grid
popup: board
	./board popup.png popup
clean:
	rm -f board grid.png popup.png
