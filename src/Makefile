# Console binaries — NanoCamelid 32XCD
SH_PREFIX   ?= /Volumes/Untitled/toolchains/sh-elf/bin/sh-elf-
M68K_PREFIX ?= m68k-elf-

BUILD := ../build

ROM := $(BUILD)/nanocamelid32x_m1.32x

all: $(ROM)

$(BUILD)/genesis68k_boot.bin: genesis68k/boot.s
	@mkdir -p $(BUILD)
	$(M68K_PREFIX)as -m68000 $< -o $(BUILD)/genesis68k_boot.o
	$(M68K_PREFIX)ld -Ttext=0 --oformat binary -e 0 $(BUILD)/genesis68k_boot.o -o $@

$(BUILD)/genesis68k_main.bin: genesis68k/main.c genesis68k/main.ld
	@mkdir -p $(BUILD)
	$(M68K_PREFIX)gcc -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin \
	    -Wall -Wextra -c genesis68k/main.c -o $(BUILD)/genesis68k_main.o
	$(M68K_PREFIX)ld -T genesis68k/main.ld -e cmain $(BUILD)/genesis68k_main.o \
	    -o $(BUILD)/genesis68k_main.elf
	$(M68K_PREFIX)objcopy -O binary $(BUILD)/genesis68k_main.elf $@

$(BUILD)/font.bin: ../scripts/mkfont.py
	@mkdir -p $(BUILD)
	python3 ../scripts/mkfont.py $@

$(BUILD)/sh2_master.bin: sh2_master/heartbeat.s
	@mkdir -p $(BUILD)
	$(SH_PREFIX)as --isa=sh2 --big $< -o $(BUILD)/sh2_master.o
	$(SH_PREFIX)ld -Ttext=0x22000800 -e _start $(BUILD)/sh2_master.o -o $(BUILD)/sh2_master.elf
	$(SH_PREFIX)objcopy -O binary --only-section=.text $(BUILD)/sh2_master.elf $@

$(BUILD)/sh2_slave.bin: sh2_slave/heartbeat.s
	@mkdir -p $(BUILD)
	$(SH_PREFIX)as --isa=sh2 --big $< -o $(BUILD)/sh2_slave.o
	$(SH_PREFIX)ld -Ttext=0x22001000 -e _start $(BUILD)/sh2_slave.o -o $(BUILD)/sh2_slave.elf
	$(SH_PREFIX)objcopy -O binary --only-section=.text $(BUILD)/sh2_slave.elf $@

$(ROM): $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin $(BUILD)/sh2_slave.bin \
        $(BUILD)/genesis68k_main.bin $(BUILD)/font.bin ../scripts/mkrom.py
	python3 ../scripts/mkrom.py $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin \
	    $(BUILD)/sh2_slave.bin $(BUILD)/genesis68k_main.bin $(BUILD)/font.bin $@

clean:
	rm -f $(BUILD)/genesis68k_*.* $(BUILD)/sh2_master.* $(BUILD)/sh2_slave.* $(BUILD)/font.bin $(ROM)
