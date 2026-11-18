/*
 * Test and benchmark for how userland sees the CPU's counter (CNTPCT_EL0 and
 * CNTFRQ_EL0). Programs built for the Switch assume a 19.2 MHz counter; on
 * this board the kernel can trap userland's reads and answer them in 19.2 MHz
 * ticks (kernel patch 0013). This KIP reports what it sees either way, so it
 * can be run on a kernel with or without the trap and the two compared:
 *
 *   - the frequency CNTFRQ_EL0 reports, and the rate the counter actually
 *     advances at, measured against svcGetSystemTick (which the kernel
 *     always answers in 19.2 MHz ticks);
 *   - that reads never go backwards;
 *   - what one read costs, and what one SVC costs for comparison.
 *
 * It also checks the core numbers userland sees (kernel patch 0014): on a
 * machine with fewer than four cores the kernel folds the four virtual cores
 * onto the cores present, and a thread pinned to any of them must run and get
 * back the numbers it set.
 *
 * Checks print PASS or FAIL; measurements print as "[counter_test] measure:".
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>

namespace {

    namespace {

        constexpr s64 NominalTicksPerSecond = 19'200'000;

        constinit int g_checks = 0;
        constinit int g_failures = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        void Check(const char *what, bool ok) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[counter_test] %s: %s\n", ok ? "PASS" : "FAIL", what);
        }

        /* The raw instructions, as libnx and libstratosphere use them. */
        inline __attribute__((always_inline)) u64 ReadCounter() {
            u64 v;
            __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(v) :: "memory");
            return v;
        }

        inline __attribute__((always_inline)) u64 ReadFrequency() {
            u64 v;
            __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v) :: "memory");
            return v;
        }

        /* The kernel's own time, always in nominal ticks. */
        inline __attribute__((always_inline)) s64 KernelTick() {
            return static_cast<s64>(::svcGetSystemTick());
        }

        s64 NanosecondsBetween(s64 from, s64 to) {
            return ((to - from) * 1'000'000'000) / NominalTicksPerSecond;
        }

        void TestFrequency() {
            const u64 frequency = ReadFrequency();
            Log("[counter_test] measure: CNTFRQ_EL0 reads %lu Hz\n", frequency);
            Check("CNTFRQ_EL0 reports 19.2 MHz", frequency == static_cast<u64>(NominalTicksPerSecond));
        }

        void TestRate() {
            /* Two seconds by the kernel's clock, which patch 0012 makes accurate. */
            const s64 k0 = KernelTick();
            const u64 c0 = ReadCounter();
            svcSleepThread(INT64_C(1000000000) * (2));
            const s64 k1 = KernelTick();
            const u64 c1 = ReadCounter();

            const s64 kernel_ns = NanosecondsBetween(k0, k1);
            const u64 rate = static_cast<u64>((static_cast<unsigned __int128>(c1 - c0) * 1'000'000'000) / static_cast<u64>(kernel_ns));
            Log("[counter_test] measure: the counter advanced %lu in %ld ms, %lu Hz\n", c1 - c0, kernel_ns / 1'000'000, rate);
            Check("the counter advances at 19.2 MHz (within 1%)", rate > NominalTicksPerSecond * 99 / 100 && rate < NominalTicksPerSecond * 101 / 100);
        }

        void TestMonotonic() {
            constexpr int Reads = 100'000;
            u64 last = ReadCounter();
            bool ok = true;
            for (int i = 0; i < Reads; ++i) {
                const u64 now = ReadCounter();
                if (now < last) {
                    ok = false;
                    Log("[counter_test] read %d went backwards: %lu after %lu\n", i, now, last);
                    break;
                }
                last = now;
            }
            Check("100000 reads in a row never go backwards", ok);
        }

        void MeasureCost() {
            /* Several rounds, reporting the fastest, so that being scheduled out does not count. */
            constexpr int Reads  = 200'000;
            constexpr int Rounds = 5;

            s64 best_read = std::numeric_limits<s64>::max();
            for (int r = 0; r < Rounds; ++r) {
                const s64 start = KernelTick();
                u64 sink = 0;
                for (int i = 0; i < Reads; ++i) {
                    sink += ReadCounter();
                }
                const s64 end = KernelTick();
                __asm__ __volatile__("" :: "r"(sink));
                best_read = std::min(best_read, NanosecondsBetween(start, end));
            }

            s64 best_svc = std::numeric_limits<s64>::max();
            for (int r = 0; r < Rounds; ++r) {
                const s64 start = KernelTick();
                s64 sink = 0;
                for (int i = 0; i < Reads; ++i) {
                    sink += KernelTick();
                }
                const s64 end = KernelTick();
                __asm__ __volatile__("" :: "r"(sink));
                best_svc = std::min(best_svc, NanosecondsBetween(start, end));
            }

            Log("[counter_test] measure: one counter read takes %ld ns (best of %d rounds of %d)\n", best_read / Reads, Rounds, Reads);
            Log("[counter_test] measure: one svcGetSystemTick takes %ld ns, for comparison\n", best_svc / Reads);
        }

        /* svcGetInfo types used below (svc::InfoType). */
        constexpr u32 InfoType_IdleTickCount   = 10;
        constexpr u32 InfoType_ThreadTickCount = 25;
        constexpr Handle CurrentThreadHandle   = 0xFFFF8000;

        /* Pins this thread to one virtual core and checks what the kernel reports back. */
        void TestPinnedTo(s32 core) {
            const Result set_rc = ::svcSetThreadCoreMask(CurrentThreadHandle, core, 1u << core);

            s32 ideal = -1;
            u64 mask = 0;
            const Result get_rc = ::svcGetThreadCoreMask(std::addressof(ideal), std::addressof(mask), CurrentThreadHandle);
            const s32 current = static_cast<s32>(::svcGetCurrentProcessorNumber());

            u64 idle = 0, thread_ticks = 0;
            const Result idle_rc   = ::svcGetInfo(std::addressof(idle), InfoType_IdleTickCount, INVALID_HANDLE, static_cast<u64>(current));
            const Result thread_rc = ::svcGetInfo(std::addressof(thread_ticks), InfoType_ThreadTickCount, CurrentThreadHandle, static_cast<u64>(core));

            Log("[counter_test] measure: pinned to core %d: set 0x%x, core mask (%d, 0x%lx), processor number %d, idle ticks 0x%x, thread ticks on core 0x%x\n",
                core, set_rc, ideal, mask, current, idle_rc, thread_rc);

            char what[0x80];
            std::snprintf(what, sizeof(what), "a thread pinned to core %d runs there and reads back its core", core);
            Check(what, R_SUCCEEDED(set_rc) && R_SUCCEEDED(get_rc) && ideal == core && mask == (1ul << core) && current == core && R_SUCCEEDED(idle_rc) && R_SUCCEEDED(thread_rc));
        }

        void TestCores() {
            /* System modules run on core 3; check it first, as this thread starts there. */
            Check("the main thread starts on core 3", ::svcGetCurrentProcessorNumber() == 3);

            for (s32 core = 0; core < 4; ++core) {
                TestPinnedTo(core);
            }

            /* Allowed on all four, the number reported must be one of them. */
            const Result rc = ::svcSetThreadCoreMask(CurrentThreadHandle, 3, 0xF);
            svcSleepThread(INT64_C(1000000) * (10));
            const s32 current = static_cast<s32>(::svcGetCurrentProcessorNumber());
            Log("[counter_test] measure: allowed on cores 0-3, processor number %d\n", current);
            Check("a thread allowed on cores 0-3 reports one of them", R_SUCCEEDED(rc) && 0 <= current && current <= 3);

            /* Back where it started. */
            TestPinnedTo(3);
        }

    }

    void RunTest() {
        Log("[counter_test] starting\n");

        TestFrequency();
        TestRate();
        TestMonotonic();
        MeasureCost();
        TestCores();

        Log("[counter_test] done: %d checks, %d failure(s)\n", g_checks, g_failures);

        while (true) {
            svcSleepThread(INT64_C(1000000000) * (60));
        }
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        alignas(16) static char heap[1024 * 1024];
        extern char *fake_heap_start, *fake_heap_end;
        fake_heap_start = heap;
        fake_heap_end = heap + sizeof(heap);
    }
    void __appInit() {
        SecmonArgs config{{0xC3000002, 65011}};
        svcCallSecureMonitor(&config);
        KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0);
        hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
        KUMO_ABORT_RESULT(smInitialize());
    }
    void __appExit() { smExit(); }
    void __libnx_exit(int) { for (;;) svcSleepThread(1000000000); }
}
int main(int, char **) { RunTest(); return 0; }
