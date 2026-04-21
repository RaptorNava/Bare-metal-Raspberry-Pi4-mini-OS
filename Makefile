# ===========================================================================
# Makefile — Сборка bare-metal ОС для Raspberry Pi 4 (AArch64)
# ===========================================================================

CROSS   ?= aarch64-linux-gnu

CC      := $(CROSS)-gcc
AS      := $(CROSS)-gcc
LD      := $(CROSS)-ld
OBJCOPY := $(CROSS)-objcopy
OBJDUMP := $(CROSS)-objdump
NM      := $(CROSS)-nm

GCC_INC := $(shell $(CC) -print-file-name=include)

# ---------------------------------------------------------------------------
# Флаги компилятора
# ---------------------------------------------------------------------------
CFLAGS  := \
    -Wall \
    -Wextra \
    -ffreestanding \
    -nostdinc \
    -isystem $(GCC_INC) \
    -nostdlib \
    -fno-builtin \
    -fno-stack-protector \
    -fno-pie \
    -fno-pic \
    -march=armv8-a \
    -mcpu=cortex-a72 \
    -mlittle-endian \
    -O2 \
    -I./include

ASFLAGS := \
    -ffreestanding \
    -nostdinc \
    -isystem $(GCC_INC) \
    -nostdlib \
    -march=armv8-a \
    -mcpu=cortex-a72 \
    -mlittle-endian \
    -I./include

LDFLAGS := \
    -nostdlib \
    -T linker.ld

# ---------------------------------------------------------------------------
# Исходные файлы
# ---------------------------------------------------------------------------
BOOT_SRC    := src/boot.S

KERNEL_SRCS := \
    src/kernel/kernel.c \
    src/kernel/memory.c \
    src/kernel/shell.c  \
    src/kernel/timer.c

DRIVER_SRCS := \
    src/drivers/gpio.c    \
    src/drivers/mailbox.c \
    src/drivers/uart.c \
    src/drivers/usb.c

LIB_SRCS := \
    src/lib/printf.c \
    src/lib/string.c \
    src/lib/font.c

MEDIA_SRCS := \
    src/media/audio.c \
    src/media/video.c \
    src/media/video_data.c

ALL_SRCS := $(KERNEL_SRCS) $(DRIVER_SRCS) $(LIB_SRCS) $(MEDIA_SRCS)

# ---------------------------------------------------------------------------
# Объектные файлы
# ---------------------------------------------------------------------------
BUILD_DIR   := build

BOOT_OBJ    := $(BUILD_DIR)/boot.o
KERNEL_OBJS := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(ALL_SRCS))
ALL_OBJS    := $(BOOT_OBJ) $(KERNEL_OBJS)

# ---------------------------------------------------------------------------
# Выходные файлы
# ---------------------------------------------------------------------------
TARGET_ELF  := $(BUILD_DIR)/kernel.elf
TARGET_IMG  := kernel8.img
TARGET_LST  := $(BUILD_DIR)/kernel.lst
TARGET_MAP  := $(BUILD_DIR)/kernel.map

# ---------------------------------------------------------------------------
# QEMU Флаги
# ---------------------------------------------------------------------------
QEMU         := qemu-system-aarch64

QEMU_FLAGS_RASPI := \
    -M raspi4b \
    -m 2G \
    -kernel $(TARGET_IMG) \
    -serial stdio \
    -display none

QEMU_FLAGS_VIRT := \
    -M virt \
    -cpu cortex-a72 \
    -m 512M \
    -kernel $(TARGET_ELF) \
    -nographic \
    -serial mon:stdio

# ===========================================================================
# Правила сборки
# ===========================================================================

.PHONY: all run run-virt clean dump size nm help

all: $(TARGET_IMG)

$(TARGET_IMG): $(TARGET_ELF)
	@echo "--- Copying ELF to BIN ---"
	$(OBJCOPY) -O binary $< $@
	@echo "kernel8.img ready ($$(wc -c < $@) bytes)"

$(TARGET_ELF): $(ALL_OBJS) linker.ld
	@echo "--- Linking ---"
	$(LD) $(LDFLAGS) -Map=$(TARGET_MAP) -o $@ $(ALL_OBJS)

$(BUILD_DIR)/boot.o: src/boot.S | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling: $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/kernel
	@mkdir -p $(BUILD_DIR)/drivers
	@mkdir -p $(BUILD_DIR)/lib
	@mkdir -p $(BUILD_DIR)/media

# ===========================================================================
# Команды
# ===========================================================================

run: $(TARGET_IMG)
	$(QEMU) $(QEMU_FLAGS_RASPI)

run-virt: $(TARGET_ELF)
	$(QEMU) $(QEMU_FLAGS_VIRT)

clean:
	@rm -rf $(BUILD_DIR) $(TARGET_IMG)
	@echo "Cleaned."

dump: $(TARGET_ELF)
	$(OBJDUMP) -d -S $(TARGET_ELF) > $(TARGET_LST)

size: $(TARGET_ELF)
	@$(CROSS)-size $(TARGET_ELF)

nm: $(TARGET_ELF)
	$(NM) -n $(TARGET_ELF)

help:
	@echo "Usage: make [target]"
	@echo "Targets: all, run, run-virt, clean, dump, size"
