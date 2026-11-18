#include <kumo/detail.hpp>
#include <kumo/mutex.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <kumo/dma.hpp>
#include <kumo/platform.hpp>

namespace kumo::dma {

    void Arena::Initialize(void *memory, size_t size) {
        KUMO_ASSERT(m_base == 0);
        KUMO_ASSERT(::kumo::detail::IsAligned(reinterpret_cast<uintptr_t>(memory), PageSize));
        KUMO_ASSERT(::kumo::detail::IsAligned(size, PageSize));

        m_base = reinterpret_cast<uintptr_t>(memory);
        m_size = size;
        m_used = 0;
    }

    ::Result Arena::Allocate(Buffer *out, size_t size, size_t alignment) {
        KUMO_ASSERT(m_base != 0);
        KUMO_ASSERT(::kumo::detail::IsPowerOfTwo(alignment) && alignment <= PageSize);
        KUMO_UNLESS(size > 0, MAKERESULT(Module_Kernel, KernelError_InvalidSize));

        /* Alignment within a page is the same virtually and physically, so aligning the */
        /* virtual address is enough. */
        const uintptr_t end = m_base + m_size;
        uintptr_t candidate = ::kumo::detail::AlignUp(m_base + m_used, alignment);
        while (candidate < end && size <= end - candidate) {
            u64 physical;
            size_t contiguous;
            KUMO_TRY(platform::GetPhysicalRange(std::addressof(physical), std::addressof(contiguous), reinterpret_cast<const void *>(candidate)));

            if (size <= contiguous) {
                std::memset(reinterpret_cast<void *>(candidate), 0, size);

                out->address          = reinterpret_cast<u8 *>(candidate);
                out->physical_address = physical;
                out->size             = size;
                m_used = (candidate + size) - m_base;
                return 0;
            }

            /* The physical backing breaks before `size` bytes; start again after the break. */
            candidate = ::kumo::detail::AlignUp(candidate + contiguous, alignment);
        }

        return (MAKERESULT(Module_Kernel, KernelError_OutOfMemory));
    }

}
