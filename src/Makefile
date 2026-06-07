# Console binaries — NanoCamelid 32XCD
SH_PREFIX   ?= /Volumes/Untitled/toolchains/sh-elf/bin/sh-elf-
M68K_PREFIX ?= m68k-elf-

BUILD := ../build

ROM := $(BUILD)/nanocamelid32x.32x

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

SH2_CFLAGS := -m2 -mb -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -Wall -Wextra
SH2_LIBGCC := $(shell $(SH_PREFIX)gcc -m2 -mb -print-libgcc-file-name)
SHARED_HDRS := shared/mailbox.h shared/model.h shared/infer.h

$(BUILD)/sh2_master.bin: sh2_master/crt0.s sh2_master/main.c sh2_master/link.ld shared/infer.c $(SHARED_HDRS)
	@mkdir -p $(BUILD)
	$(SH_PREFIX)as --isa=sh2 --big sh2_master/crt0.s -o $(BUILD)/sh2_master_crt0.o
	$(SH_PREFIX)gcc $(SH2_CFLAGS) -c sh2_master/main.c -o $(BUILD)/sh2_master_main.o
	$(SH_PREFIX)gcc $(SH2_CFLAGS) -c shared/infer.c -o $(BUILD)/sh2_master_infer.o
	$(SH_PREFIX)ld -T sh2_master/link.ld -e _start \
	    $(BUILD)/sh2_master_crt0.o $(BUILD)/sh2_master_main.o $(BUILD)/sh2_master_infer.o $(SH2_LIBGCC) \
	    -o $(BUILD)/sh2_master.elf
	$(SH_PREFIX)objcopy -O binary $(BUILD)/sh2_master.elf $@

$(BUILD)/sh2_slave.bin: sh2_slave/crt0.s sh2_slave/main.c sh2_slave/link.ld shared/infer.c $(SHARED_HDRS)
	@mkdir -p $(BUILD)
	$(SH_PREFIX)as --isa=sh2 --big sh2_slave/crt0.s -o $(BUILD)/sh2_slave_crt0.o
	$(SH_PREFIX)gcc $(SH2_CFLAGS) -c sh2_slave/main.c -o $(BUILD)/sh2_slave_main.o
	$(SH_PREFIX)gcc $(SH2_CFLAGS) -c shared/infer.c -o $(BUILD)/sh2_slave_infer.o
	$(SH_PREFIX)ld -T sh2_slave/link.ld -e _start \
	    $(BUILD)/sh2_slave_crt0.o $(BUILD)/sh2_slave_main.o $(BUILD)/sh2_slave_infer.o $(SH2_LIBGCC) \
	    -o $(BUILD)/sh2_slave.elf
	$(SH_PREFIX)objcopy -O binary $(BUILD)/sh2_slave.elf $@

MODELDIR := ../model

$(ROM): $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin $(BUILD)/sh2_slave.bin \
        $(BUILD)/genesis68k_main.bin $(BUILD)/font.bin ../scripts/mkrom.py \
        $(MODELDIR)/candidates.bin $(MODELDIR)/weights.bin $(MODELDIR)/vocab.bin
	python3 ../scripts/mkrom.py $(BUILD)/genesis68k_boot.bin $(BUILD)/sh2_master.bin \
	    $(BUILD)/sh2_slave.bin $(BUILD)/genesis68k_main.bin $(BUILD)/font.bin $(MODELDIR) $@

clean:
	rm -f $(BUILD)/genesis68k_*.* $(BUILD)/sh2_master.* $(BUILD)/sh2_slave.* $(BUILD)/font.bin $(ROM)
