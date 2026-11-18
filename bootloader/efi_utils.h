#pragma once
#include "efi_spec_defs.h"

static void print(EFI_TEXT_OUT *con, const char *s) {
    unsigned short buf[2] = {0, 0};
    while (*s) {
        buf[0] = (unsigned short)*s++;
        con->OutputString(con, buf);
    }
}

static void print_hex(EFI_TEXT_OUT *con, ULL v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> (60 - i * 4)) & 0xF];
    buf[18] = 0;
    print(con, buf);
}

static const int LABEL_WIDTH = 11;

static void print_padded_label(EFI_TEXT_OUT *con, const char *label) {
    int len = 0;
    const char *p = label;
    while (*p++) len++;
    
    print(con, label);
    for (int i = len; i < LABEL_WIDTH; i++) {
        print(con, " ");
    }
}

static void print_address_range(EFI_TEXT_OUT *con, const char *label, ULL start, ULL end) {
    print(con, "Loaded ");
    print_padded_label(con, label);
    print(con, "from ");
    print_hex(con, start);
    print(con, " to ");
    print_hex(con, end);
    print(con, "\n");
}

static void print_hex_labeled(EFI_TEXT_OUT *con, const char *label, ULL value) {
    print_padded_label(con, label);
    print_hex(con, value);
    print(con, "\n");
}

static void print_hex_range(EFI_TEXT_OUT *con, const char *label, ULL start, ULL end) {
    print_padded_label(con, label);
    print_hex(con, start);
    print(con, " - ");
    print_hex(con, end);
    print(con, "\n");
}

static void print_hex_with_size(EFI_TEXT_OUT *con, const char *label, ULL base, ULL size) {
    print_padded_label(con, label);
    print_hex(con, base);
    print(con, " size ");
    print_hex(con, size);
    print(con, "\n");
}

static void memcpy_simple(void *dst, const void *src, ULL n) {
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
}
