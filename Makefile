.RECIPEPREFIX := >

# Toolchain prefix. On an AArch64 machine (a Pi 5 host) the native compiler is
# used; elsewhere the cross compiler. Override with  make CROSS=prefix-  (or CROSS=).
CROSS ?= $(if $(filter aarch64 arm64,$(shell uname -m)),,aarch64-linux-gnu-)
CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy
# -mstrict-align: the code runs with the MMU off, where the CPU treats all data
# accesses as device memory and faults on unaligned ones.
CFLAGS := -Iinclude -Wall -Wextra -O0 -ffreestanding -nostdlib -nostartfiles -g -fno-omit-frame-pointer -mgeneral-regs-only -mstrict-align

BOOT_SRC := start.S main.c uart.c mailbox.c power.c gdb_stub.c
PAYLOAD_SRC := loaded_start.S loaded_kernel.c uart.c gdb.c power.c gdb_stub.c

# objs DIR SRCS: DIR/x.o for every src/x.S or src/x.c in SRCS
objs = $(addprefix $(1)/,$(addsuffix .o,$(basename $(2))))

.PHONY: all qemu test run run-pty clean

all: kernel8.bin kernel_payload.bin

# The same sources are built twice, with one rule set (BUILD_VARIANT):
#   board: objects in build/,      images in ./            (make)
#   QEMU:  objects+images in build-qemu/, with -DQEMU_CONSOLE, which puts the
#          console (UART2 on the board) on the mini UART, QEMU's second serial
#          port                                             (make qemu)
# $(1) object dir   $(2) image dir prefix ('' or 'build-qemu/')   $(3) extra CFLAGS
define BUILD_VARIANT
$(1)/%.o: src/%.S
>@mkdir -p $$(@D)
>$$(CC) $$(CFLAGS) $(3) -MMD -MP -c $$< -o $$@
$(1)/%.o: src/%.c
>@mkdir -p $$(@D)
>$$(CC) $$(CFLAGS) $(3) -MMD -MP -c $$< -o $$@
$(2)kernel8.elf: $(call objs,$(1),$(BOOT_SRC)) linker.ld
>$$(LD) -T linker.ld -o $$@ $(call objs,$(1),$(BOOT_SRC))
$(2)kernel_payload.elf: $(call objs,$(1),$(PAYLOAD_SRC)) linker_kernel.ld
>$$(LD) -T linker_kernel.ld -o $$@ $(call objs,$(1),$(PAYLOAD_SRC))
$(2)%.bin: $(2)%.elf
>$$(OBJCOPY) -O binary $$< $$@
endef

$(eval $(call BUILD_VARIANT,build,,))
$(eval $(call BUILD_VARIANT,build-qemu,build-qemu/,-DQEMU_CONSOLE))

# Rebuild when a header changes.
-include $(wildcard build/*.d build-qemu/*.d)

qemu: build-qemu/kernel8.bin build-qemu/kernel_payload.bin

clean:
>rm -rf build build-qemu tests/build tests/__pycache__ src/*.o *.elf *.img *.bin

test:
>python3 tests/run_tests.py --cross '$(CROSS)'

run:
>./scripts/run-qemu.sh

run-pty:
>./scripts/run-qemu.sh pty
