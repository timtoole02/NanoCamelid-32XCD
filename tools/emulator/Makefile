PICODRIVE_SRC ?= /Volumes/Untitled/emu/picodrive
OUT := ../../build/nc-headless

$(OUT): headless.c
	mkdir -p ../../build
	cc -O2 -Wall -Wextra -I$(PICODRIVE_SRC)/platform/libretro/libretro-common/include \
	   -o $(OUT) headless.c

clean:
	rm -f $(OUT)
