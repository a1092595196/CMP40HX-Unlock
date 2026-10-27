#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "uefi_min.h"
#include "core_abi.h"

struct core_image {
    EFI_PHYSICAL_ADDRESS allocation;
    UINTN pages;
    do_permissive_fn do_permissive;
    const char *version;
};

EFI_STATUS load_nvpermissive_core(
    EFI_BOOT_SERVICES *bs,
    const void *elf,
    UINTN elf_size,
    struct core_image *out);

#endif

