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

$(ROM): $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin $(BUILD)/sh2_slave.bin ../scripts/mkrom.py
	python3 ../scripts/mkrom.py $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin $(BUILD)/sh2_slave.bin $@

clean:
	rm -f $(BUILD)/genesis68k_boot.* $(BUILD)/sh2_master.* $(BUILD)/sh2_slave.* $(ROM)
