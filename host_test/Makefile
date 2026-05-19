# Host-side smoke test for color_pipeline.c + dither.c + palette.c.
# Builds a small `host_test` binary that takes 400x600 RGB888 input and
# emits a BMP preview after dithering.  Use it to compare against the
# Python reference output.
#
#   make
#   ./host_test ../../demo_rgb888.h        # uses demoRGB888 array
#   ./host_test --raw ../../demo.raw      # raw 400x600 RGB888 from disk

CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -I.. -I../main -Iinclude_shim
LDFLAGS ?= -lm

SRCS = \
    host_main.c \
    ../main/color_pipeline.c \
    ../main/dither.c \
    ../main/palette.c

OBJS = $(SRCS:.c=.o)

host_test: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) host_test out_preview.bmp out_indices.bin
