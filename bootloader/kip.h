#pragma once
#include "efi_spec_defs.h"

#define INI1_MAGIC        0x31494E49u
#define KIP1_MAGIC        0x3150494Bu
#define KIP1_NUM_SECTIONS 6
#define MAX_KIPS          32

typedef struct { unsigned int offset, size_decomp, size_comp, attrib; } Kip1Section;

typedef struct {
    unsigned int       magic;
    char               name[12];
    unsigned long long tid;
    unsigned int       proc_cat;
    unsigned char      main_thrd_prio, def_cpu_core, res, flags;
    Kip1Section        sections[KIP1_NUM_SECTIONS];
    unsigned int       caps[32];
} Kip1Header;

typedef struct { unsigned int magic, size, num_procs, pad; } Ini1Header;

typedef struct { void *data; ULL size; char name[13]; } KipEntry;

void collect_kips(EFI_BOOT_SERVICES *BS, EFI_FILE_PROTOCOL *root,
                  EFI_TEXT_OUT *console,
                  KipEntry *kips, int *count_out, ULL *total_out);

void inject_kips(EFI_TEXT_OUT *console, void *buf, ULL buf_size,
                 KipEntry *kips, int kip_count);
