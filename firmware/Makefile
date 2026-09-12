# Host test build. Nothing here touches an STM32 toolchain — the point is
# that every module in src/core and src/drivers compiles and passes tests on
# a plain PC compiler, so logic bugs are found before the hardware exists.
#
#   make          build and run all tests
#   make asan     same, with address+UB sanitizers
#   make clean

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion \
           -Wno-sign-conversion -O2 -g
BUILD   := build

.DEFAULT_GOAL := all

CORE    := src/core/ringbuf.c src/core/wav.c src/core/audio.c \
           src/core/encoder.c src/core/player.c \
           src/core/font.c src/core/font_data.c src/core/gfx.c \
           src/core/theme.c src/core/decoder.c src/core/decoder_wav.c \
           src/core/decoder_flac.c \
           src/core/decoder_mp3.c \
           src/core/library_index.c

DRIVERS := src/drivers/st7789.c
UI      := src/ui/screen_library.c

# dr_flac is third-party and will not survive -Werror -Wconversion. It gets
# its own relaxed rule rather than us editing someone else's library, which
# would have to be redone on every update.
VENDOR_OBJ := $(BUILD)/dr_flac_impl.o $(BUILD)/dr_mp3_impl.o
VENDOR_CFLAGS := -std=c11 -O2 -g -w

$(BUILD)/dr_flac_impl.o: src/core/dr_flac_impl.c third_party/dr_flac.h | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

$(BUILD)/dr_mp3_impl.o: src/core/dr_mp3_impl.c third_party/dr_mp3.h | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c -o $@ $<

TESTS   := test_ringbuf test_wav test_audio test_encoder \
           test_player test_st7789 test_font test_gfx test_decoder \
           test_decoder_flac test_decoder_mp3 test_library_index

BINS    := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test asan clean preview

all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: tests/%.c $(CORE) $(DRIVERS) $(UI) $(VENDOR_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(CORE) $(DRIVERS) $(UI) $(VENDOR_OBJ)

# Render real 240x240 screens to images you can look at.
preview: | $(BUILD)
	@mkdir -p $(BUILD)/preview
	$(CC) $(CFLAGS) -o $(BUILD)/preview_gen tools/preview.c $(CORE) $(UI)
	./$(BUILD)/preview_gen

test: $(BINS)
	@fail=0; for t in $(BINS); do ./$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; else \
	 echo "TESTS FAILED"; exit 1; fi

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan: clean test

clean:
	@rm -rf $(BUILD)
