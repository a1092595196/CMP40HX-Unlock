CC       ?= gcc
LD       ?= ld
OBJCOPY  ?= objcopy
PYTHON   ?= python3

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-pie -fno-ident -fcf-protection=none -mcmodel=large \
	-maccumulate-outgoing-args -mno-red-zone -Iinclude

BUILD := build
CORE  := build/40hx-core.o

# Optional compile-time default mode for GRUB integration (no LoadOptions):
#   make clean && make                       -> NVPermissiveEFI.efi        (inspect fallback)
#   make clean && make AUTO=REQUEST_COMPUTE  -> NVPermissiveEFI-compute.efi
#   make clean && make AUTO=REQUEST_FULL     -> NVPermissiveEFI-full.efi
AUTO ?=
CFLAGS += $(if $(AUTO),-DAUTO_MODE=$(AUTO))

ifneq ($(AUTO),)
NAME := NVPermissiveEFI-$(subst REQUEST_,,$(AUTO)).efi
else
NAME := NVPermissiveEFI.efi
endif

.PHONY: all clean verify fetch test
all: $(BUILD)/$(NAME)

fetch:
	./tools/fetch-core.sh 2>/dev/null || true

verify: $(CORE)
	@echo "40HX core present"

test: $(BUILD)/test_elf_loader
	$(BUILD)/test_elf_loader

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: src/main.c include/uefi_min.h include/core_abi.h include/elf_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/elf_loader.o: src/elf_loader.c include/uefi_min.h include/core_abi.h include/elf64_min.h include/elf_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# TU106 core: our own SysV .o (replaces the GA102 nvpermissive core)
# mingw64 gcc -c emits COFF; convert to ELF relocatable first (elf_loader
# inspects e_ident/e_type/e_machine), then wrap as binary data.
$(CORE): src/core_40hx.c include/core_abi.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/core_blob.o: $(CORE) | $(BUILD)
	$(OBJCOPY) -I pe-x86-64 -O elf64-x86-64 $(CORE) build/40hx-core.elf.o
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 build/40hx-core.elf.o $@
	$(OBJCOPY) --rename-section .data=.core,alloc,load,readonly,data,contents $@
	$(OBJCOPY) --redefine-sym _binary_build_40hx_core_elf_o_start=_binary_nvpermissive_core_o_start $@
	$(OBJCOPY) --redefine-sym _binary_build_40hx_core_elf_o_end=_binary_nvpermissive_core_o_end $@

$(BUILD)/$(NAME): $(BUILD)/main.o $(BUILD)/elf_loader.o $(BUILD)/core_blob.o
	$(LD) -mi386pep --subsystem 10 --entry efi_main --image-base 0x10000000 \
		--file-alignment 0x200 --section-alignment 0x1000 \
		-o $@ $^
	$(PYTHON) tools/verify_efi.py $@

$(BUILD)/test_elf_loader: tests/test_elf_loader.c src/elf_loader.c $(BUILD)/core_blob.o | $(BUILD)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -fno-builtin \
		-fno-stack-protector -fcf-protection=none -mno-red-zone -Iinclude \
		tests/test_elf_loader.c src/elf_loader.c $(BUILD)/core_blob.o \
		-Wl,-z,noexecstack -o $@

clean:
	rm -f $(BUILD)/main.o $(BUILD)/elf_loader.o $(BUILD)/core_blob.o \
		$(BUILD)/NVPermissiveEFI*.efi $(BUILD)/test_elf_loader
