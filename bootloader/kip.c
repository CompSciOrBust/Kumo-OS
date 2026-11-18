#include "kip.h"
#include "efi_utils.h"

static int is_kip_file(unsigned short *name) {
    int len = 0;
    while (name[len]) len++;
    if (len >= 4 && name[len-4] == '.' &&
        (name[len-3] | 0x20) == 'k' &&
        (name[len-2] | 0x20) == 'i' &&
        (name[len-1] | 0x20) == 'p') return 1;
    if (len >= 5 && name[len-5] == '.' &&
        (name[len-4] | 0x20) == 'k' &&
        (name[len-3] | 0x20) == 'i' &&
        (name[len-2] | 0x20) == 'p' &&
        name[len-1] == '1') return 1;
    return 0;
}

void collect_kips(EFI_BOOT_SERVICES *BS, EFI_FILE_PROTOCOL *root,
                  EFI_TEXT_OUT *console,
                  KipEntry *kips, int *count_out, ULL *total_out) {
    *count_out = 0;
    *total_out = 0;

    EFI_FILE_PROTOCOL *dir = 0;
    if (root->Open(root, &dir, L"\\kips", EFI_FILE_MODE_READ, 0) != EFI_SUCCESS)
        return;

    unsigned char info_buf[640];
    for (;;) {
        ULL info_size = sizeof(info_buf);
        if (dir->Read(dir, &info_size, info_buf) != EFI_SUCCESS || info_size == 0)
            break;

        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)info_buf;
        if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
        if (!is_kip_file(fi->FileName)) continue;
        if (*count_out >= MAX_KIPS) { print(console, "KIP limit reached\n"); break; }

        EFI_FILE_PROTOCOL *f = 0;
        if (dir->Open(dir, &f, fi->FileName, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS)
            continue;

        ULL sz = fi->FileSize;
        void *buf = 0;
        if (BS->AllocatePool(EfiLoaderData, sz, &buf) != EFI_SUCCESS) {
            f->Close(f);
            continue;
        }

        EFI_STATUS r = f->Read(f, &sz, buf);
        f->Close(f);

        if (r != EFI_SUCCESS || sz < sizeof(Kip1Header) ||
            ((Kip1Header *)buf)->magic != KIP1_MAGIC) {
            ((EFI_STATUS (*)(void*))BS->FreePool)(buf);
            continue;
        }

        char kip_name[13];
        for (int i = 0; i < 12; i++) kip_name[i] = ((Kip1Header *)buf)->name[i];
        kip_name[12] = 0;
        print(console, "KIP: "); print(console, kip_name); print(console, "\n");

        kips[*count_out].data = buf;
        kips[*count_out].size = sz;
        for (int i = 0; i < 13; i++) kips[*count_out].name[i] = kip_name[i];
        (*count_out)++;
        *total_out += sz;
    }

    dir->Close(dir);
}

// Mesosphere image layout (see build_mesosphere.py):
//   kernel | INI1 header + KIPs | <page align> | kernel_ldr (+ its bss/stack)
// The kernel metadata block holds two self-relative u64 offsets:
//   meta + 0x00: INI1 offset, relative to meta
//   meta + 0x08: kernel_ldr offset, relative to meta + 8
// kernel_ldr sits directly after the INI1, so growing the INI1 means moving
// kernel_ldr out of the way first and re-pointing the metadata at it.
void inject_kips(EFI_TEXT_OUT *console, void *kernel_address, ULL kernel_size,
                 KipEntry *kips, int kip_count) {
    if (kip_count == 0) return;

    unsigned char *b = (unsigned char *)kernel_address;
    if (kernel_size < 16 ||
        b[4] != 'M' || b[5] != 'S' || b[6] != 'S' || b[7] != '1') {
        print(console, "No MSS1 metadata, skipping KIP injection\n");
        return;
    }

    ULL meta_off = *(unsigned int *)(b + 8);
    if (meta_off + 16 > kernel_size) {
        print(console, "Bad MSS1 metadata offset, skipping KIP injection\n");
        return;
    }
    ULL *ini1_rel = (ULL *)(b + meta_off);
    ULL *kldr_rel = (ULL *)(b + meta_off + 8);
    ULL ini1_off  = meta_off + *ini1_rel;
    ULL kldr_off  = meta_off + 8 + *kldr_rel;

    if (ini1_off + sizeof(Ini1Header) > kernel_size || kldr_off > kernel_size) {
        print(console, "Bad INI1/kernel_ldr offsets, skipping KIP injection\n");
        return;
    }

    Ini1Header *ini1 = (Ini1Header *)(b + ini1_off);
    if (ini1->magic != INI1_MAGIC || ini1->size < sizeof(Ini1Header) ||
        ini1_off + ini1->size > kernel_size) {
        print(console, "INI1 not found, creating fresh\n");
        ini1->magic     = INI1_MAGIC;
        ini1->size      = sizeof(Ini1Header);
        ini1->num_procs = 0;
        ini1->pad       = 0;
    }

    ULL kips_total = 0;
    for (int i = 0; i < kip_count; i++) kips_total += kips[i].size;

    // Relocate kernel_ldr (everything from its offset to the end of the image)
    // past the grown INI1. Regions overlap with dst > src, so copy backwards.
    ULL new_ini1_end = ini1_off + ini1->size + kips_total;
    ULL new_kldr_off = (new_ini1_end + 0xFFFULL) & ~0xFFFULL;
    if (kldr_off >= ini1_off && new_kldr_off > kldr_off) {
        ULL tail = kernel_size - kldr_off;
        for (ULL n = tail; n > 0; n--)
            b[new_kldr_off + n - 1] = b[kldr_off + n - 1];
        *kldr_rel = new_kldr_off - (meta_off + 8);
        print_address_range(console, "kldr", (ULL)(b + new_kldr_off),
                            (ULL)(b + new_kldr_off + tail));
    }

    char *append_ptr = (char *)ini1 + ini1->size;
    for (int i = 0; i < kip_count; i++) {
        memcpy_simple(append_ptr, kips[i].data, kips[i].size);
        print_address_range(console, kips[i].name, (ULL)append_ptr, (ULL)append_ptr + kips[i].size);
        append_ptr += kips[i].size;
        ini1->size     += (unsigned int)kips[i].size;
        ini1->num_procs++;
    }

    // Clear the padding between the INI1 and the relocated kernel_ldr.
    if (kldr_off >= ini1_off)
        while (append_ptr < (char *)b + new_kldr_off) *append_ptr++ = 0;

    print(console, "Injected ");
    print_hex(console, (ULL)kip_count);
    print(console, " KIP(s)\n");
}
