#include <mesosphere.hpp>
#include "kern_debug_log_impl.hpp"

namespace ams::kern {

#if defined(MESOSPHERE_DEBUG_LOG_USE_UART)

    namespace {

        constexpr u32 PL011_DR    = 0x000;
        constexpr u32 PL011_FR    = 0x018;
        constexpr u32 PL011_IBRD  = 0x024;
        constexpr u32 PL011_FBRD  = 0x028;
        constexpr u32 PL011_LCR_H = 0x02C;
        constexpr u32 PL011_CR    = 0x030;

        constexpr u32 PL011_FR_TXFF = (1u << 5);
        constexpr u32 PL011_FR_TXFE = (1u << 7);

        constinit uintptr_t s_uart_base = 0;

        ALWAYS_INLINE volatile u32 &Reg(u32 offset) {
            return *reinterpret_cast<volatile u32 *>(s_uart_base + offset);
        }

    }

    /* Stashed pre-MMU by SetupDevicePhysicalMemoryRegions. */
    namespace board::generic::aarch64 {
        extern u32 g_uart_clock_hz;
    }

    bool KDebugLogImpl::Initialize() {
        s_uart_base = GetInteger(KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_Uart));

        const u32 clock = (board::generic::aarch64::g_uart_clock_hz != 0)
                        ? board::generic::aarch64::g_uart_clock_hz
                        : 24000000u;

        /* PL011 baud rate: divisor = clock / (16 * 115200) */
        constexpr u32 Baud16 = 16u * 115200u;
        const u32 ibrd = clock / Baud16;
        const u32 fbrd = ((clock % Baud16) * 64u + Baud16 / 2u) / Baud16;

        Reg(PL011_CR)    = 0;
        Reg(PL011_IBRD)  = ibrd;
        Reg(PL011_FBRD)  = fbrd;
        Reg(PL011_LCR_H) = (3u << 5) | (1u << 4); /* 8-bit, FIFOs enabled */
        Reg(PL011_CR)    = (1u << 0) | (1u << 8) | (1u << 9); /* UARTEN | TXE | RXE */

        return true;
    }

    void KDebugLogImpl::PutChar(char c) {
        while (Reg(PL011_FR) & PL011_FR_TXFF) { /* spin while TX FIFO full */ }
        Reg(PL011_DR) = static_cast<u32>(static_cast<u8>(c));
    }

    void KDebugLogImpl::Flush() {
        while (!(Reg(PL011_FR) & PL011_FR_TXFE)) { /* spin until TX FIFO drained */ }
    }

    void KDebugLogImpl::Save()    { /* ... */ }
    void KDebugLogImpl::Restore() { /* ... */ }

#else
    #error "Unknown Debug device!"
#endif

}
