#include <switch.h>

extern "C" void __libnx_initheap() {
    // All FS workers share newlib's locked allocator. Archive readers also
    // enforce per-mount metadata and scratch limits.
    alignas(16) static unsigned char heap[26 * 1024 * 1024];
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = reinterpret_cast<char *>(heap);
    fake_heap_end = reinterpret_cast<char *>(heap + sizeof(heap));
}
