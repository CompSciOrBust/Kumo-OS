#include <kumo/detail.hpp>
#include <kumo/mutex.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <kumo/pci.hpp>
#include <kumo/platform.hpp>

namespace kumo::pci {

    namespace {

        constexpr int DevicesPerBus      = 32;
        constexpr int FunctionsPerDevice = 8;
        constexpr int BarsPerDevice      = 6;

        constinit bool g_initialized = false;
        constinit platform::PciHostInfo g_host = {};
        constinit uintptr_t g_ecam = 0;
        constinit ::kumo::Mutex g_mutex;

        inline uintptr_t GetConfigAddress(const Location &loc, u32 offset) {
            KUMO_ASSERT(g_initialized);
            KUMO_ASSERT(g_host.start_bus <= loc.bus && loc.bus <= g_host.end_bus);
            KUMO_ASSERT(offset < 0x1000);
            return g_ecam + (static_cast<uintptr_t>(loc.bus - g_host.start_bus) << 20) + (static_cast<uintptr_t>(loc.device) << 15) + (static_cast<uintptr_t>(loc.function) << 12) + offset;
        }

        template<typename T>
        inline T Read(const Location &loc, u32 offset) {
            return *reinterpret_cast<const volatile T *>(GetConfigAddress(loc, offset));
        }

        template<typename T>
        inline void Write(const Location &loc, u32 offset, T value) {
            *reinterpret_cast<volatile T *>(GetConfigAddress(loc, offset)) = value;
        }

        bool ReadDeviceInfo(DeviceInfo *out, const Location &loc) {
            const u16 vendor_id = Read<u16>(loc, ConfigOffset_VendorId);
            if (vendor_id == 0xFFFF) {
                return false;
            }

            out->location   = loc;
            out->vendor_id  = vendor_id;
            out->device_id  = Read<u16>(loc, ConfigOffset_DeviceId);
            out->revision   = Read<u8>(loc, ConfigOffset_Revision);
            out->prog_if    = Read<u8>(loc, ConfigOffset_ProgIf);
            out->sub_class  = Read<u8>(loc, ConfigOffset_SubClass);
            out->base_class = Read<u8>(loc, ConfigOffset_BaseClass);
            return true;
        }

        /* Visit every function present. The firmware has numbered the buses, so walking the whole */
        /* ECAM range finds devices behind bridges without following the bridges. */
        template<typename F>
        void ForEachDevice(F f) {
            for (int bus = g_host.start_bus; bus <= g_host.end_bus; ++bus) {
                for (int device = 0; device < DevicesPerBus; ++device) {
                    DeviceInfo info;
                    if (!ReadDeviceInfo(std::addressof(info), { static_cast<u8>(bus), static_cast<u8>(device), 0 })) {
                        continue;
                    }
                    if (!f(info)) {
                        return;
                    }

                    /* Functions 1-7 exist only on multi-function devices. */
                    if ((Read<u8>(info.location, ConfigOffset_HeaderType) & 0x80) == 0) {
                        continue;
                    }
                    for (int function = 1; function < FunctionsPerDevice; ++function) {
                        if (ReadDeviceInfo(std::addressof(info), { static_cast<u8>(bus), static_cast<u8>(device), static_cast<u8>(function) })) {
                            if (!f(info)) {
                                return;
                            }
                        }
                    }
                }
            }
        }

    }

    ::Result Initialize() {
        std::scoped_lock lk(g_mutex);
        if (g_initialized) return 0;

        KUMO_UNLESS(platform::GetPciHostInfo(std::addressof(g_host)), MAKERESULT(Module_Kernel, KernelError_NotFound));

        const size_t size = (static_cast<size_t>(g_host.end_bus) - g_host.start_bus + 1) * (1024 * 1024);
        KUMO_TRY(platform::MapIo(std::addressof(g_ecam), g_host.ecam_physical_address, size));

        g_initialized = true;
        return 0;
    }

    int EnumerateDevices(DeviceInfo *out, int max) {
        int count = 0;
        ForEachDevice([&](const DeviceInfo &info) {
            if (count < max) {
                out[count] = info;
            }
            ++count;
            return true;
        });
        return count;
    }

    bool FindDeviceByClass(DeviceInfo *out, u8 base_class, u8 sub_class, u8 prog_if, int index) {
        bool found = false;
        ForEachDevice([&](const DeviceInfo &info) {
            if (info.base_class == base_class && info.sub_class == sub_class && info.prog_if == prog_if) {
                if (index-- == 0) {
                    *out  = info;
                    found = true;
                    return false;
                }
            }
            return true;
        });
        return found;
    }

    bool FindDeviceById(DeviceInfo *out, u16 vendor_id, u16 device_id, int index) {
        bool found = false;
        ForEachDevice([&](const DeviceInfo &info) {
            if (info.vendor_id == vendor_id && info.device_id == device_id) {
                if (index-- == 0) {
                    *out  = info;
                    found = true;
                    return false;
                }
            }
            return true;
        });
        return found;
    }

    u32 FindCapability(const Location &loc, u8 capability_id, u32 after) {
        if ((Read<u16>(loc, ConfigOffset_Status) & Status_CapabilityList) == 0) {
            return 0;
        }

        /* Each capability is { u8 id; u8 next; ... }. The low two bits of a pointer are reserved. */
        u32 offset = (after == 0) ? Read<u8>(loc, ConfigOffset_CapabilityPointer) : Read<u8>(loc, after + 1);

        /* A well-formed list has at most 48 entries; the bound guards against a loop. */
        for (int i = 0; i < 48; ++i) {
            offset &= ~3u;
            if (offset < 0x40) {
                return 0;
            }
            if (Read<u8>(loc, offset) == capability_id) {
                return offset;
            }
            offset = Read<u8>(loc, offset + 1);
        }
        return 0;
    }

    u8   ReadConfig8(const Location &loc, u32 offset)  { return Read<u8>(loc, offset); }
    u16  ReadConfig16(const Location &loc, u32 offset) { return Read<u16>(loc, offset); }
    u32  ReadConfig32(const Location &loc, u32 offset) { return Read<u32>(loc, offset); }
    void WriteConfig16(const Location &loc, u32 offset, u16 value) { Write<u16>(loc, offset, value); }
    void WriteConfig32(const Location &loc, u32 offset, u32 value) { Write<u32>(loc, offset, value); }

    ::Result GetBar(BarInfo *out, const Location &loc, int bar) {
        KUMO_UNLESS(0 <= bar && bar < BarsPerDevice, MAKERESULT(Module_Kernel, KernelError_OutOfRange));

        std::scoped_lock lk(g_mutex);

        const u32 offset = ConfigOffset_Bar0 + 4 * bar;
        const u32 low    = Read<u32>(loc, offset);

        const bool is_memory = (low & 1) == 0;
        const bool is_64_bit = is_memory && ((low >> 1) & 3) == 2;
        KUMO_UNLESS(!is_64_bit || bar + 1 < BarsPerDevice, MAKERESULT(Module_Kernel, KernelError_OutOfRange));

        const u32 high = is_64_bit ? Read<u32>(loc, offset + 4) : 0;

        /* Size the BAR by writing all ones and reading back which bits stick. */
        /* Decoding is switched off meanwhile so the device never answers at a bogus address. */
        const u16 command = Read<u16>(loc, ConfigOffset_Command);
        Write<u16>(loc, ConfigOffset_Command, command & ~(Command_IoSpace | Command_MemorySpace));

        Write<u32>(loc, offset, 0xFFFFFFFF);
        const u32 low_mask = Read<u32>(loc, offset);
        Write<u32>(loc, offset, low);

        u32 high_mask = 0xFFFFFFFF;
        if (is_64_bit) {
            Write<u32>(loc, offset + 4, 0xFFFFFFFF);
            high_mask = Read<u32>(loc, offset + 4);
            Write<u32>(loc, offset + 4, high);
        }

        Write<u16>(loc, ConfigOffset_Command, command);

        const u64 flag_mask = is_memory ? 0xF : 0x3;
        const u64 mask      = ((static_cast<u64>(high_mask) << 32) | low_mask) & ~flag_mask;

        out->is_memory = is_memory;
        out->is_64_bit = is_64_bit;
        out->address   = ((static_cast<u64>(high) << 32) | low) & ~flag_mask;
        out->size      = (mask == 0) ? 0 : (~mask + 1);
        if (!is_64_bit) {
            out->size &= 0xFFFFFFFF;
        }
        return 0;
    }

    ::Result MapBar(uintptr_t *out_address, size_t *out_size, const Location &loc, int bar) {
        BarInfo info;
        KUMO_TRY(GetBar(std::addressof(info), loc, bar));

        /* An unimplemented or unassigned BAR cannot be mapped. */
        KUMO_UNLESS(info.is_memory,                    MAKERESULT(Module_Kernel, KernelError_InvalidState));
        KUMO_UNLESS(info.size != 0 && info.address != 0, MAKERESULT(Module_Kernel, KernelError_InvalidState));

        KUMO_TRY(platform::MapIo(out_address, info.address, info.size));
        *out_size = info.size;
        return 0;
    }

    void EnableMemoryAndBusMaster(const Location &loc) {
        std::scoped_lock lk(g_mutex);
        const u16 command = Read<u16>(loc, ConfigOffset_Command);
        Write<u16>(loc, ConfigOffset_Command, command | Command_MemorySpace | Command_BusMaster);
    }

}
