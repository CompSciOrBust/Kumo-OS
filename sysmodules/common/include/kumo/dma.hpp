/*
 * Memory that devices read and write directly (DMA), for driver system modules.
 *
 * An Arena hands out zeroed, physically contiguous pieces of one block of
 * memory the caller owns, usually a static array in the module's image. It
 * exists because a module's memory is only guaranteed to be contiguous page by
 * page: a descriptor ring larger than a page (gVNIC's, ENA's) needs one
 * physical address for the whole ring. The arena checks contiguity with the
 * kernel and skips over any break in the physical backing.
 *
 * Memory is never given back: drivers allocate once, when they bind a
 * device, and keep it for the life of the process. The arena takes no lock;
 * allocate from one thread, at start-up.
 *
 * Like the rest of this library it owns no memory, so a module that links it
 * without using it pays nothing.
 */
#pragma once
#include <switch.h>
#include <cstddef>
#include <cstdint>

namespace kumo::dma {

    constexpr inline size_t PageSize = 0x1000;

    /* One allocation: where the driver sees it and where the device sees it. */
    struct Buffer {
        u8 *address;
        u64 physical_address;
        size_t size;

        constexpr bool IsValid() const { return address != nullptr; }

        template<typename T>
        T *As() const { return reinterpret_cast<T *>(address); }

        u8 *At(size_t offset) const { return address + offset; }
        u64 PhysicalAt(size_t offset) const { return physical_address + offset; }
    };

    class Arena {
        private:
            uintptr_t m_base;
            size_t m_size;
            size_t m_used;
        public:
            constexpr Arena() : m_base(0), m_size(0), m_used(0) { /* ... */ }

            /* `memory` is page-aligned, `size` a whole number of pages, owned by the caller for good. */
            void Initialize(void *memory, size_t size);

            /* `size` zeroed bytes, physically contiguous, aligned to `alignment` (a power of two, at */
            /* most PageSize) both virtually and physically. Fails with ResultOutOfMemory when no */
            /* contiguous run of that size is left. */
            ::Result Allocate(Buffer *out, size_t size, size_t alignment = PageSize);

            size_t GetSize() const { return m_size; }
            size_t GetUsedSize() const { return m_used; }
    };

}
