#include "uefi_min.h"
#include "core_abi.h"
#include "elf_loader.h"

#define CMP_VENDOR_ID 0x10deU
#define CMP90HX_DEVICE_ID 0x220dU
#define CMP40HX_DEVICE_ID 0x1f0bU
#define NV_PMC_BOOT_0 0x00000000U
#define BOOT0_ARCH_MASK (0x1fU << 24)
#define BOOT0_ARCH_GA10X (0x17U << 24)
#define BOOT0_IMPL_MASK (0x0fU << 20)
#define BOOT0_IMPL_GA102 (0x02U << 20)
#define BOOT0_ARCH_TU10X (0x1aU << 24)   /* (upstream label) */
#define BOOT0_ARCH_TU106 (0x16U << 24)   /* Turing family: TU106/CMP40HX BOOT0=0x16xxxxxx */
#define PCI_COMMAND_OFFSET 0x04U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_MASTER 0x0004U
#define PCI_BAR0_OFFSET 0x10U
#define PCI_BRIDGE_BUS_OFFSET 0x18U
#define PCI_BRIDGE_CONTROL_OFFSET 0x3eU
#define PCI_BRIDGE_CTL_BUS_RESET 0x0040U
#define PCI_CLASS_BRIDGE_PCI 0x0604U
#define FB_WINDOW_SIZE 0x10000000ULL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE 4096ULL

extern const UINT8 _binary_nvpermissive_core_o_start[];
extern const UINT8 _binary_nvpermissive_core_o_end[];

static EFI_SYSTEM_TABLE *g_system_table;
static EFI_BOOT_SERVICES *g_boot_services;

struct pci_location {
    EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb;
    UINT16 segment;
    UINT8 bus;
    UINT8 device;
    UINT8 function;
};

struct platform_gpu {
    struct pci_location gpu;
    struct pci_location bridge;
    UINT64 bar0;
    UINT64 bar1;
    UINT32 saved_bars[6];
    UINT16 saved_command;
};

enum requested_mode {
    REQUEST_EXIT,
    REQUEST_INSPECT,
    REQUEST_FULL,
    REQUEST_COMPUTE,
    REQUEST_GRAPHICS,
    REQUEST_GR_RESET,
    REQUEST_GR_ACR
};

static void console_write(const char *text)
{
    CHAR16 wide[256];
    UINTN used = 0;

    if (!g_system_table || !g_system_table->ConOut)
        return;
    while (*text) {
        if (*text == '\n') {
            if (used + 2 >= ARRAY_SIZE(wide)) {
                wide[used] = 0;
                g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
                used = 0;
            }
            wide[used++] = '\r';
        }
        wide[used++] = (UINT8)*text++;
        if (used + 1 >= ARRAY_SIZE(wide)) {
            wide[used] = 0;
            g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
            used = 0;
        }
    }
    if (used) {
        wide[used] = 0;
        g_system_table->ConOut->OutputString(g_system_table->ConOut, wide);
    }
}

static UINTN append_char(char *out, UINTN cap, UINTN pos, char value)
{
    if (pos + 1 < cap)
        out[pos] = value;
    return pos + 1;
}

static UINTN append_string(char *out, UINTN cap, UINTN pos, const char *text)
{
    if (!text)
        text = "(null)";
    while (*text)
        pos = append_char(out, cap, pos, *text++);
    return pos;
}

static UINTN append_number(char *out, UINTN cap, UINTN pos, UINT64 value,
                           UINT32 base, int upper, UINTN width, char pad,
                           int negative)
{
    char digits[32];
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    UINTN count = 0;
    UINTN total;

    do {
        digits[count++] = alphabet[value % base];
        value /= base;
    } while (value && count < ARRAY_SIZE(digits));
    total = count + (negative ? 1U : 0U);
    if (negative && pad == '0')
        pos = append_char(out, cap, pos, '-');
    while (total < width) {
        pos = append_char(out, cap, pos, pad);
        total++;
    }
    if (negative && pad != '0')
        pos = append_char(out, cap, pos, '-');
    while (count)
        pos = append_char(out, cap, pos, digits[--count]);
    return pos;
}

static UINTN format_message(char *out, UINTN cap, const char *format,
                            __builtin_va_list args)
{
    UINTN pos = 0;

    while (*format) {
        char pad = ' ';
        UINTN width = 0;
        int length = 0;
        char spec;

        if (*format != '%') {
            pos = append_char(out, cap, pos, *format++);
            continue;
        }
        format++;
        if (*format == '%') {
            pos = append_char(out, cap, pos, *format++);
            continue;
        }
        while (*format == '#' || *format == '-' || *format == '+' || *format == ' ')
            format++;
        if (*format == '0') {
            pad = '0';
            format++;
        }
        while (*format >= '0' && *format <= '9')
            width = width * 10 + (UINTN)(*format++ - '0');
        if (*format == '.') {
            format++;
            while (*format >= '0' && *format <= '9')
                format++;
        }
        if (*format == 'l') {
            length = 1;
            format++;
            if (*format == 'l') {
                length = 2;
                format++;
            }
        } else if (*format == 'z') {
            length = 3;
            format++;
        }
        spec = *format ? *format++ : 0;
        if (spec == 's') {
            pos = append_string(out, cap, pos, __builtin_va_arg(args, const char *));
        } else if (spec == 'c') {
            pos = append_char(out, cap, pos,
                              (char)__builtin_va_arg(args, int));
        } else if (spec == 'p') {
            UINT64 value = (UINT64)(uintptr_t)__builtin_va_arg(args, void *);
            pos = append_string(out, cap, pos, "0x");
            pos = append_number(out, cap, pos, value, 16, 0,
                                width ? width : 1, '0', 0);
        } else if (spec == 'x' || spec == 'X' || spec == 'u') {
            UINT64 value;
            if (length == 2)
                value = __builtin_va_arg(args, unsigned long long);
            else if (length == 1)
                value = __builtin_va_arg(args, unsigned long);
            else if (length == 3)
                value = __builtin_va_arg(args, size_t);
            else
                value = __builtin_va_arg(args, unsigned int);
            pos = append_number(out, cap, pos, value,
                                spec == 'u' ? 10U : 16U,
                                spec == 'X', width, pad, 0);
        } else if (spec == 'd' || spec == 'i') {
            INT64 signed_value;
            UINT64 value;
            int negative;
            if (length == 2)
                signed_value = __builtin_va_arg(args, long long);
            else if (length == 1)
                signed_value = __builtin_va_arg(args, long);
            else if (length == 3)
                signed_value = (INT64)__builtin_va_arg(args, ptrdiff_t);
            else
                signed_value = __builtin_va_arg(args, int);
            negative = signed_value < 0;
            value = negative ? (UINT64)(-(signed_value + 1)) + 1U
                             : (UINT64)signed_value;
            pos = append_number(out, cap, pos, value, 10, 0, width, pad,
                                negative);
        } else {
            pos = append_char(out, cap, pos, '%');
            if (spec)
                pos = append_char(out, cap, pos, spec);
        }
    }
    if (cap)
        out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

static void console_printf(const char *format, ...)
{
    char buffer[1024];
    __builtin_va_list args;
    __builtin_va_start(args, format);
    format_message(buffer, sizeof(buffer), format, args);
    __builtin_va_end(args);
    console_write(buffer);
}

void SYSVABI gpu_log(struct gpu_dev *g, const char *format, ...)
{
    char buffer[1024];
    __builtin_va_list args;
    (void)g;
    __builtin_va_start(args, format);
    format_message(buffer, sizeof(buffer), format, args);
    __builtin_va_end(args);
    console_write("[core] ");
    console_write(buffer);
}

void *SYSVABI memcpy(void *destination, const void *source, size_t size)
{
    UINT8 *dst = (UINT8 *)destination;
    const UINT8 *src = (const UINT8 *)source;
    while (size--)
        *dst++ = *src++;
    return destination;
}

void *SYSVABI memset(void *destination, int value, size_t size)
{
    UINT8 *dst = (UINT8 *)destination;
    while (size--)
        *dst++ = (UINT8)value;
    return destination;
}

void SYSVABI memcpy_toio(volatile void *destination, const void *source,
                         size_t size)
{
    volatile UINT8 *dst = (volatile UINT8 *)destination;
    const UINT8 *src = (const UINT8 *)source;
    while (size--)
        *dst++ = *src++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint32_t SYSVABI ioread32(const volatile void *address)
{
    UINT32 value = *(const volatile UINT32 *)address;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return value;
}

void SYSVABI iowrite32(uint32_t value, volatile void *address)
{
    *(volatile UINT32 *)address = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static EFI_STATUS pci_read(const struct pci_location *location, UINT32 offset,
                           EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
                           void *value)
{
    return location->rb->Pci.Read(location->rb, width,
        EFI_PCI_ADDRESS(location->bus, location->device,
                        location->function, offset), 1, value);
}

static EFI_STATUS pci_write(const struct pci_location *location, UINT32 offset,
                            EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_WIDTH width,
                            void *value)
{
    return location->rb->Pci.Write(location->rb, width,
        EFI_PCI_ADDRESS(location->bus, location->device,
                        location->function, offset), 1, value);
}

static int find_cmp90hx(struct platform_gpu *platform)
{
    EFI_HANDLE *handles = 0;
    UINTN handle_count = 0;
    EFI_STATUS status;
    UINTN h;
    int matches = 0;

    status = g_boot_services->LocateHandleBuffer(
        ByProtocol, (EFI_GUID *)&EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID,
        0, &handle_count, &handles);
    if (EFI_ERROR(status))
        return -1;

    for (h = 0; h < handle_count; h++) {
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *rb = 0;
        UINT32 bus;
        status = g_boot_services->HandleProtocol(
            handles[h], (EFI_GUID *)&EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID,
            (void **)&rb);
        if (EFI_ERROR(status) || !rb)
            continue;
        for (bus = 0; bus < 256; bus++) {
            UINT32 dev;
            for (dev = 0; dev < 32; dev++) {
                UINT32 func;
                for (func = 0; func < 8; func++) {
                    struct pci_location location = {
                        rb, (UINT16)rb->SegmentNumber,
                        (UINT8)bus, (UINT8)dev, (UINT8)func
                    };
                    UINT32 id = 0xffffffffU;
                    status = pci_read(&location, 0, EfiPciWidthUint32, &id);
                    if (EFI_ERROR(status) || (id & 0xffffU) == 0xffffU)
                        continue;
                    if ((id & 0xffffU) == CMP_VENDOR_ID &&
                        ((id >> 16) == CMP90HX_DEVICE_ID ||
                         (id >> 16) == CMP40HX_DEVICE_ID)) {
                        if (matches == 0) {
                            matches = 1;
                            platform->gpu = location;
                        } else if (platform->gpu.segment != location.segment ||
                                   platform->gpu.bus != location.bus ||
                                   platform->gpu.device != location.device ||
                                   platform->gpu.function != location.function) {
                            matches = 2;
                        }
                    }
                }
            }
        }
    }
    g_boot_services->FreePool(handles);
    return matches;
}

static int find_upstream_bridge(struct platform_gpu *platform)
{
    UINT32 bus;
    UINT32 best_span = 0xffffffffU;
    UINT32 best_secondary = 0;
    int found = 0;

    for (bus = 0; bus < 256; bus++) {
        UINT32 dev;
        for (dev = 0; dev < 32; dev++) {
            UINT32 func;
            for (func = 0; func < 8; func++) {
                struct pci_location location = {
                    platform->gpu.rb, platform->gpu.segment,
                    (UINT8)bus, (UINT8)dev, (UINT8)func
                };
                UINT32 id = 0xffffffffU;
                UINT32 class_revision = 0;
                UINT32 buses = 0;
                UINT32 secondary;
                UINT32 subordinate;
                UINT32 span;
                if (EFI_ERROR(pci_read(&location, 0, EfiPciWidthUint32, &id)) ||
                    (id & 0xffffU) == 0xffffU)
                    continue;
                if (EFI_ERROR(pci_read(&location, 0x08, EfiPciWidthUint32,
                                       &class_revision)) ||
                    ((class_revision >> 16) & 0xffffU) != PCI_CLASS_BRIDGE_PCI)
                    continue;
                if (EFI_ERROR(pci_read(&location, PCI_BRIDGE_BUS_OFFSET,
                                       EfiPciWidthUint32, &buses)))
                    continue;
                secondary = (buses >> 8) & 0xffU;
                subordinate = (buses >> 16) & 0xffU;
                if (secondary > platform->gpu.bus ||
                    subordinate < platform->gpu.bus)
                    continue;
                span = subordinate - secondary;
                if (!found || span < best_span ||
                    (span == best_span && secondary > best_secondary)) {
                    platform->bridge = location;
                    best_span = span;
                    best_secondary = secondary;
                    found = 1;
                }
            }
        }
    }
    return found ? 0 : -1;
}

static int decode_memory_bar(struct platform_gpu *platform, UINT32 offset,
                             UINT64 *address, UINT32 *consumed)
{
    UINT32 low;
    UINT32 high = 0;
    if (EFI_ERROR(pci_read(&platform->gpu, offset,
                           EfiPciWidthUint32, &low)) || (low & 1U))
        return -1;
    *consumed = 1;
    if (((low >> 1) & 3U) == 2U) {
        if (EFI_ERROR(pci_read(&platform->gpu, offset + 4,
                               EfiPciWidthUint32, &high)))
            return -1;
        *consumed = 2;
    }
    *address = ((UINT64)high << 32) | (low & ~0x0fU);
    return *address ? 0 : -1;
}

static int prepare_pci_resources(struct platform_gpu *platform)
{
    UINT32 bar0_slots;
    UINT32 bar1_slots;
    UINT32 bar1_offset;
    UINTN i;

    if (EFI_ERROR(pci_read(&platform->gpu, PCI_COMMAND_OFFSET,
                           EfiPciWidthUint16, &platform->saved_command)))
        return -1;
    for (i = 0; i < ARRAY_SIZE(platform->saved_bars); i++) {
        if (EFI_ERROR(pci_read(&platform->gpu,
                               PCI_BAR0_OFFSET + (UINT32)i * 4,
                               EfiPciWidthUint32,
                               &platform->saved_bars[i])))
            return -1;
    }
    if (decode_memory_bar(platform, PCI_BAR0_OFFSET,
                          &platform->bar0, &bar0_slots))
        return -1;
    bar1_offset = PCI_BAR0_OFFSET + bar0_slots * 4;
    if (decode_memory_bar(platform, bar1_offset,
                          &platform->bar1, &bar1_slots))
        return -1;
    (void)bar1_slots;
    platform->saved_command |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    if (EFI_ERROR(pci_write(&platform->gpu, PCI_COMMAND_OFFSET,
                            EfiPciWidthUint16,
                            &platform->saved_command)))
        return -1;
    return 0;
}

static int restore_gpu_config(struct platform_gpu *platform)
{
    UINTN i;
    for (i = 0; i < ARRAY_SIZE(platform->saved_bars); i++) {
        UINT32 value = platform->saved_bars[i];
        if (EFI_ERROR(pci_write(&platform->gpu,
                                PCI_BAR0_OFFSET + (UINT32)i * 4,
                                EfiPciWidthUint32, &value)))
            return -1;
    }
    if (EFI_ERROR(pci_write(&platform->gpu, PCI_COMMAND_OFFSET,
                            EfiPciWidthUint16,
                            &platform->saved_command)))
        return -1;
    return 0;
}

static void *SYSVABI host_alloc(struct gpu_dev *g, size_t size)
{
    void *memory = 0;
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    (void)platform;
    if (EFI_ERROR(g_boot_services->AllocatePool(EfiLoaderData, size, &memory)))
        return 0;
    memset(memory, 0, size);
    return memory;
}

static void SYSVABI host_free(struct gpu_dev *g, void *memory)
{
    (void)g;
    if (memory)
        g_boot_services->FreePool(memory);
}

static void *SYSVABI dma_alloc_below_4g(struct gpu_dev *g, size_t size,
                                        uint64_t *physical)
{
    EFI_PHYSICAL_ADDRESS address = 0xffffffffULL;
    UINTN pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)g;
    if (!physical || !pages ||
        EFI_ERROR(g_boot_services->AllocatePages(
            AllocateMaxAddress, EfiLoaderData, pages, &address)))
        return 0;
    memset((void *)(uintptr_t)address, 0, pages * PAGE_SIZE);
    *physical = address;
    return (void *)(uintptr_t)address;
}

static void SYSVABI dma_free(struct gpu_dev *g, void *memory, size_t size,
                             uint64_t physical)
{
    UINTN pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)g;
    (void)memory;
    if (physical && pages)
        g_boot_services->FreePages(physical, pages);
}

static int SYSVABI pci_cfg_read32(struct gpu_dev *g, unsigned offset,
                                  uint32_t *value)
{
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    return EFI_ERROR(pci_read(&platform->gpu, offset,
                              EfiPciWidthUint32, value)) ? -1 : 0;
}

static void SYSVABI delay_ms(unsigned milliseconds)
{
    g_boot_services->Stall((UINTN)milliseconds * 1000U);
}

static int sbr_once(struct gpu_dev *g, UINT32 settle_ms)
{
    struct platform_gpu *platform = (struct platform_gpu *)g->platform_data;
    UINT16 bridge_control;
    UINT16 asserted;
    UINT16 restored;
    UINT32 id = 0xffffffffU;

    if (EFI_ERROR(pci_read(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                           EfiPciWidthUint16, &bridge_control)))
        return -1;
    asserted = bridge_control | PCI_BRIDGE_CTL_BUS_RESET;
    restored = bridge_control & ~PCI_BRIDGE_CTL_BUS_RESET;
    if (EFI_ERROR(pci_write(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                            EfiPciWidthUint16, &asserted)))
        return -1;
    g_boot_services->Stall(100000U);
    if (EFI_ERROR(pci_write(&platform->bridge, PCI_BRIDGE_CONTROL_OFFSET,
                            EfiPciWidthUint16, &restored)))
        return -1;
    g_boot_services->Stall(1000000U);
    if (restore_gpu_config(platform))
        return -1;
    if (settle_ms)
        g_boot_services->Stall((UINTN)settle_ms * 1000U);
    if (EFI_ERROR(pci_read(&platform->gpu, 0, EfiPciWidthUint32, &id)) ||
        id != ((CMP90HX_DEVICE_ID << 16) | CMP_VENDOR_ID))
        return -1;
    return 0;
}

static int SYSVABI secondary_bus_reset(struct gpu_dev *g)
{
    gpu_log(g, "SBR via %04x:%02x:%02x.%u; settling 8 seconds\n",
            ((struct platform_gpu *)g->platform_data)->bridge.segment,
            ((struct platform_gpu *)g->platform_data)->bridge.bus,
            ((struct platform_gpu *)g->platform_data)->bridge.device,
            ((struct platform_gpu *)g->platform_data)->bridge.function);
    return sbr_once(g, 8000U);
}

static int dual_sbr(struct gpu_dev *g)
{
    if (g->secondary_bus_reset(g))
        return -1;
    if (g->secondary_bus_reset(g))
        return -1;
    gpu_log(g, "dual SBR complete\n");
    return 0;
}

static int has_word(const CHAR16 *options, UINTN bytes, const char *word)
{
    UINTN chars = bytes / sizeof(CHAR16);
    UINTN i;
    for (i = 0; i < chars; i++) {
        UINTN j = 0;
        if (i && options[i - 1] > ' ')
            continue;
        while (word[j] && i + j < chars &&
               options[i + j] == (CHAR16)(UINT8)word[j])
            j++;
        if (!word[j] && (i + j == chars || options[i + j] <= ' '))
            return 1;
    }
    return 0;
}

static enum requested_mode parse_mode(EFI_HANDLE image_handle)
{
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    if (EFI_ERROR(g_boot_services->HandleProtocol(
            image_handle, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
            (void **)&loaded)) || !loaded || !loaded->LoadOptions) {
        /* No LoadOptions (UEFI boot entry / GRUB exec). */
#ifdef AUTO_MODE
        /* Compiled with -DAUTO_MODE=REQUEST_xxx (GRUB integration). */
        console_write("No LoadOptions; auto mode enabled (build flag).\n");
        return (enum requested_mode)AUTO_MODE;
#else
        /* Safe read-only fallback. */
        return REQUEST_INSPECT;
#endif
    }
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "full"))
        return REQUEST_FULL;
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "compute"))
        return REQUEST_COMPUTE;
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "graphics"))
        return REQUEST_GRAPHICS;
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "gr-reset"))
        return REQUEST_GR_RESET;
    if (has_word((const CHAR16 *)loaded->LoadOptions,
                 loaded->LoadOptionsSize, "gr-acr"))
        return REQUEST_GR_ACR;
    return REQUEST_INSPECT;
}

static int run_one(struct core_image *core, struct gpu_dev *gpu, int mode,
                   const char *name, int reset_after)
{
    int result;
    console_printf("\n== %s ==\n", name);
    result = core->do_permissive(gpu, mode);
    if (result) {
        console_printf("%s failed: %d\n", name, result);
        return result;
    }
    if (reset_after && dual_sbr(gpu)) {
        console_printf("%s completed, but required dual SBR failed\n", name);
        return -1;
    }
    console_printf("%s complete\n", name);
    return 0;
}

static int run_requested(enum requested_mode request, struct core_image *core,
                         struct gpu_dev *gpu)
{
    if (request == REQUEST_EXIT)
        return 0;
    if (request == REQUEST_INSPECT)
        return run_one(core, gpu, NVPERM_MODE_GR_INSPECT, "read-only inspect", 0);
    if (request == REQUEST_COMPUTE)
        return run_one(core, gpu, NVPERM_MODE_COMPUTE, "compute", 1);
    if (request == REQUEST_GRAPHICS)
        return run_one(core, gpu, NVPERM_MODE_GRAPHICS, "graphics", 1);
    if (request == REQUEST_GR_RESET)
        return run_one(core, gpu, NVPERM_MODE_GR_RESET, "gr-reset", 1);
    if (request == REQUEST_GR_ACR)
        return run_one(core, gpu, NVPERM_MODE_GR_ACR, "gr-acr", 0);

    console_write("\nEstablishing a clean initial SEC2 session...\n");
    if (dual_sbr(gpu))
        return -1;
    if (run_one(core, gpu, NVPERM_MODE_COMPUTE, "compute", 1))
        return -1;
    if (run_one(core, gpu, NVPERM_MODE_GRAPHICS, "graphics", 1))
        return -1;
    if (run_one(core, gpu, NVPERM_MODE_GR_RESET, "gr-reset", 1))
        return -1;
    if (run_one(core, gpu, NVPERM_MODE_GR_ACR, "gr-acr", 0))
        return -1;
    return 0;
}

/* v29.67: interactive menu + key-wait so the user can SEE the tool running
 * (previously: booting the stick fell through to Windows - tool never ran). */
static int wait_any_key(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin =
        (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)g_system_table->ConIn;
    EFI_INPUT_KEY key;
    UINTN i;
    for (i = 0; i < 6000000U; i++) {
        if (conin && conin->ReadKeyStroke &&
            !EFI_ERROR(conin->ReadKeyStroke(conin, &key)))
            return 1;
        if (g_boot_services && g_boot_services->Stall)
            g_boot_services->Stall(10);
    }
    return 0;
}

static enum requested_mode interactive_menu(void)
{
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin =
        (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)g_system_table->ConIn;
    EFI_INPUT_KEY key;
    console_write("\n======== CMP40HX UEFI Unlock Tool (v29.67) ========\n");
    console_write("  Target : NVIDIA 10de:1f0b (CMP 40HX)\n");
    console_write("  [1] inspect  - 只读检查 (推荐先跑, 零风险)\n");
    console_write("  [2] full     - 解锁序列 (FP32/tensor/Gen2 + PDISP 探测)\n");
    console_write("  [3] 直接引导 Windows (什么也不做)\n");
    console_write("\n  请按键选择: ");
    for (;;) {
        if (conin && conin->ReadKeyStroke &&
            !EFI_ERROR(conin->ReadKeyStroke(conin, &key))) {
            if (key.UnicodeChar == L'1')
                return REQUEST_INSPECT;
            if (key.UnicodeChar == L'2')
                return REQUEST_FULL;
            if (key.UnicodeChar == L'3' || key.ScanCode == 0x17)
                return REQUEST_EXIT;
            if (key.UnicodeChar == 0x0D)
                return REQUEST_INSPECT;
        }
        if (g_boot_services && g_boot_services->Stall)
            g_boot_services->Stall(1000);
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table)
{
    struct platform_gpu platform;
    struct core_image core;
    struct gpu_dev gpu;
    enum requested_mode request;
    UINT32 boot0;
    EFI_STATUS core_status;
    int matches;
    int result;

    g_system_table = system_table;
    g_boot_services = system_table ? system_table->BootServices : 0;
    if (!g_boot_services)
        return EFI_INVALID_PARAMETER;
    memset(&platform, 0, sizeof(platform));
    memset(&core, 0, sizeof(core));
    memset(&gpu, 0, sizeof(gpu));
    request = parse_mode(image_handle);

    /* v29.67: interactive menu when launched from the boot menu without
     * command-line args (stick is now bootable via \EFI\BOOT\BOOTX64.EFI). */
    {
        EFI_LOADED_IMAGE_PROTOCOL *img = NULL;
        EFI_STATUS ist = g_boot_services->HandleProtocol(
            image_handle, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
            (void **)&img);
        if (EFI_ERROR(ist) || !img || img->LoadOptionsSize == 0)
            request = interactive_menu();
    }

    console_write("CMP40HX Windows Unlock / UEFI proof of concept\n");
    console_write("Target: NVIDIA 10de:1f0b / 10de:220d. Default mode is read-only inspect.\n\n");

    if (request == REQUEST_EXIT) {
        console_write("No action requested. Boot Windows.\n");
        return EFI_SUCCESS;
    }

    matches = find_cmp90hx(&platform);
    if (matches != 1) {
        console_printf("Refusing to continue: found %d CMP 90HX devices; expected exactly one.\n",
                       matches);
        return EFI_NOT_FOUND;
    }
    if (find_upstream_bridge(&platform)) {
        console_write("Could not identify the immediate upstream PCI bridge.\n");
        return EFI_NOT_FOUND;
    }
    if (prepare_pci_resources(&platform)) {
        console_write("BAR discovery or PCI command setup failed.\n");
        return EFI_DEVICE_ERROR;
    }

    console_printf("GPU %04x:%02x:%02x.%u 10de:220d\n",
                   platform.gpu.segment, platform.gpu.bus,
                   platform.gpu.device, platform.gpu.function);
    console_printf("Upstream bridge %04x:%02x:%02x.%u\n",
                   platform.bridge.segment, platform.bridge.bus,
                   platform.bridge.device, platform.bridge.function);
    console_printf("BAR0=0x%llx BAR1=0x%llx\n",
                   (unsigned long long)platform.bar0,
                   (unsigned long long)platform.bar1);

    boot0 = ioread32((const volatile void *)(uintptr_t)
                     (platform.bar0 + NV_PMC_BOOT_0));
    console_printf("BOOT0=0x%08x\n", boot0);
    /* 40HX port: GA102 strict check OR Turing-family check (TU106 boot0 arch
     * is not confidently known on this sample - observed 0x0FFFFFFF readback
     * pattern on the 40HX; accept GA10X, TU10X, or the degenerate all-ones
     * readback, otherwise refuse like upstream). */
    if ((boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_GA10X &&
        (boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_TU10X &&
        (boot0 & BOOT0_ARCH_MASK) != BOOT0_ARCH_TU106 &&
        (boot0 & 0xff000000U) != 0x0f000000U &&
        boot0 != 0x0fffffffU) {
        console_write("BOOT0 is not GA102/TU106. No register write was attempted.\n");
        return EFI_UNSUPPORTED;
    }

    core_status = load_nvpermissive_core(
        g_boot_services,
        _binary_nvpermissive_core_o_start,
        (UINTN)(_binary_nvpermissive_core_o_end -
                _binary_nvpermissive_core_o_start),
        &core);
    if (EFI_ERROR(core_status)) {
        console_printf("Core ELF loader failed: 0x%llx\n",
                       (unsigned long long)core_status);
        return core_status;
    }
    console_printf("Core loaded below 2 GiB: %s\n",
                   core.version ? core.version : "version unavailable");

    gpu.mmio = (volatile void *)(uintptr_t)platform.bar0;
    gpu.pci_device_id = CMP90HX_DEVICE_ID;
    gpu.fb_mmio = (void *)(uintptr_t)platform.bar1;
    gpu.fb_base = 0;
    gpu.fb_map_size = FB_WINDOW_SIZE;
    gpu.host_alloc = host_alloc;
    gpu.host_free = host_free;
    gpu.dma_alloc = dma_alloc_below_4g;
    gpu.dma_free = dma_free;
    gpu.dma_alloc_low = dma_alloc_below_4g;
    gpu.pci_cfg_read32 = pci_cfg_read32;
    gpu.delay_ms = delay_ms;
    gpu.secondary_bus_reset = secondary_bus_reset;
    gpu.log = gpu_log;
    gpu.platform_data = &platform;

    result = run_requested(request, &core, &gpu);
    if (result) {
        console_printf("\nFAILED (%d). Power the machine fully off before retrying.\n",
                       result);
        return EFI_DEVICE_ERROR;
    }
    if (request == REQUEST_INSPECT) {
        console_write("\nInspect completed. No unlock mode was requested.\n");
    } else {
        console_write("\nUNLOCK SEQUENCE COMPLETED.\n");
        console_write("Boot Windows without powering off.\n");
    }
    console_write("\n按任意键继续引导 Windows (60秒无按键自动继续)...\n");
    wait_any_key();
    return EFI_SUCCESS;
}
