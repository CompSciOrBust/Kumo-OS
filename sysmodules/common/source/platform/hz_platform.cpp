#include <kumo/detail.hpp>
#include <kumo/mutex.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <kumo/platform.hpp>

namespace kumo::platform {

    namespace {

        /* GetConfig items the generic-aarch64 kernel answers itself (it has no secure monitor). */
        constexpr u64 SmcFunction_GetConfig      = 0xC3000002;
        constexpr u64 ConfigItem_HzPciEcamBase   = 0x485A0001;
        constexpr u64 ConfigItem_HzPciBusRange   = 0x485A0002;

        constexpr size_t MaxIoRegions = 16;

        constinit ::kumo::Mutex g_mutex;
        constinit IoPool g_pool = IoPool_PciStorage;
        constinit ::Handle g_pool_handle = INVALID_HANDLE;
        constinit size_t g_region_count = 0;

        /* Find unmapped address space for a mapping of `size` bytes. */
        /* libstratosphere does not export its own address space allocator, so walk the memory map. */
        ::Result FindFreeAddressSpace(uintptr_t *out, size_t size) {
            u64 aslr_base, aslr_size;
            KUMO_TRY(::svcGetInfo(std::addressof(aslr_base), InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0));
            KUMO_TRY(::svcGetInfo(std::addressof(aslr_size), InfoType_AslrRegionSize,    CUR_PROCESS_HANDLE, 0));

            /* The kernel keeps the heap, alias and stack regions for their own kinds of mapping. They are */
            /* placed at random inside the address space and look unmapped, but I/O cannot be mapped there. */
            struct Region { u64 base; u64 size; };
            Region reserved[3] = {};
            constexpr InfoType ReservedRegionInfo[][2] = {
                { InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
                { InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
                { InfoType_StackRegionAddress, InfoType_StackRegionSize },
            };
            for (size_t i = 0; i < std::size(reserved); ++i) {
                KUMO_TRY(::svcGetInfo(std::addressof(reserved[i].base), ReservedRegionInfo[i][0], CUR_PROCESS_HANDLE, 0));
                KUMO_TRY(::svcGetInfo(std::addressof(reserved[i].size), ReservedRegionInfo[i][1], CUR_PROCESS_HANDLE, 0));
            }

            /* Large mappings are aligned so the kernel can use block mappings. */
            const size_t alignment = (size >= (2 * 1024 * 1024)) ? (2 * 1024 * 1024) : 0x1000;
            const u64 end = aslr_base + aslr_size;

            u64 address = ::kumo::detail::AlignUp(aslr_base, alignment);
            while (address < end && size <= end - address) {
                /* Step over a reserved region the candidate range touches. */
                bool stepped = false;
                for (const auto &region : reserved) {
                    if (region.size != 0 && address < region.base + region.size && region.base < address + size) {
                        address = ::kumo::detail::AlignUp(region.base + region.size, alignment);
                        stepped = true;
                        break;
                    }
                }
                if (stepped) {
                    continue;
                }

                ::MemoryInfo info;
                u32 page_info;
                KUMO_TRY(::svcQueryMemory(std::addressof(info), std::addressof(page_info), address));

                const u64 block_end = info.addr + info.size;
                if (info.type == MemType_Unmapped && size <= block_end - address) {
                    *out = static_cast<uintptr_t>(address);
                    return 0;
                }

                /* Guard against a block that ends at the top of the address space. */
                KUMO_UNLESS(block_end > address, MAKERESULT(Module_Kernel, KernelError_OutOfMemory));
                address = ::kumo::detail::AlignUp(block_end, alignment);
            }

            return (MAKERESULT(Module_Kernel, KernelError_OutOfMemory));
        }

        bool GetConfig(u64 *out, u64 item) {
            ::SecmonArgs args = {};
            args.X[0] = SmcFunction_GetConfig;
            args.X[1] = item;
            ::svcCallSecureMonitor(std::addressof(args));
            if (args.X[0] != 0) {
                return false;
            }

            *out = args.X[1];
            return true;
        }

    }

    void Initialize(IoPool pool) {
        std::scoped_lock lk(g_mutex);
        KUMO_ASSERT(g_pool_handle == INVALID_HANDLE);
        g_pool = pool;
    }

    bool GetPciHostInfo(PciHostInfo *out) {
        u64 ecam_base, bus_range;
        if (!GetConfig(std::addressof(ecam_base), ConfigItem_HzPciEcamBase) || !GetConfig(std::addressof(bus_range), ConfigItem_HzPciBusRange)) {
            return false;
        }

        out->ecam_physical_address = ecam_base;
        out->start_bus             = static_cast<u8>(bus_range);
        out->end_bus               = static_cast<u8>(bus_range >> 8);
        return ecam_base != 0 && out->start_bus <= out->end_bus;
    }

    ::Result MapIo(uintptr_t *out, u64 physical_address, size_t size) {
        std::scoped_lock lk(g_mutex);

        /* Open our pool on first use. */
        if (g_pool_handle == INVALID_HANDLE) {
            ::Handle handle;
            KUMO_TRY(::svcCreateIoPool(std::addressof(handle), g_pool));
            g_pool_handle = handle;
        }

        KUMO_UNLESS(g_region_count < MaxIoRegions, MAKERESULT(Module_Kernel, KernelError_ResourceExhausted));

        /* Regions are whole pages. */
        const u64 base    = ::kumo::detail::AlignDown(physical_address, 0x1000);
        const size_t span = ::kumo::detail::AlignUp(physical_address + size, 0x1000) - base;

        ::Handle region;
        KUMO_TRY(::svcCreateIoRegion(std::addressof(region), g_pool_handle, base, span, MemoryMapping_IoRegister, Perm_Rw));

        uintptr_t address = 0;
        ::Result rc = FindFreeAddressSpace(std::addressof(address), span);
        if (R_SUCCEEDED(rc)) {
            rc = ::svcMapIoRegion(region, reinterpret_cast<void *>(address), span, Perm_Rw);
        }
        if (R_FAILED(rc)) {
            ::svcCloseHandle(region);
            return (rc);
        }

        /* The region handle is kept open for the life of the process. */
        ++g_region_count;
        *out = address + (physical_address - base);
        return 0;
    }

    ::Result GetPhysicalAddress(u64 *out, const void *address) {
        ::PhysicalMemoryInfo info = {};
        KUMO_TRY(::svcQueryPhysicalAddress(std::addressof(info), reinterpret_cast<u64>(address)));

        *out = info.physical_address + (reinterpret_cast<u64>(address) - info.virtual_address);
        return 0;
    }

    ::Result GetPhysicalRange(u64 *out_address, size_t *out_contiguous_size, const void *address) {
        /* The kernel reports the whole physically contiguous run that contains the address. */
        ::PhysicalMemoryInfo info = {};
        KUMO_TRY(::svcQueryPhysicalAddress(std::addressof(info), reinterpret_cast<u64>(address)));

        const u64 offset = reinterpret_cast<u64>(address) - info.virtual_address;
        KUMO_UNLESS(offset < info.size, MAKERESULT(Module_Kernel, KernelError_InvalidMemoryState));

        *out_address         = info.physical_address + offset;
        *out_contiguous_size = info.size - offset;
        return 0;
    }

}
