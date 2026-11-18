/*
 * Asks pm to launch an installed program, the way boot2 or a shell would.
 * pm asks the loader, the loader asks ncm's location resolver and then FS
 * for the program's code, and the program starts. Needs sm, pm, fs, spl,
 * ncm and loader, and the program installed on SYSTEM (scripts/hznca.py).
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

    namespace {

        constexpr u64 TestProgramId = 0x0100000000001000;
        constexpr u64 MissingProgramId = 0x0100000000001fff;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        Result Launch(u64 *out_process_id, u64 program_id) {
            const ::NcmProgramLocation location = { .program_id = program_id, .storageID = NcmStorageId_BuiltInSystem };
            return ::pmshellLaunchProgram(0, std::addressof(location), out_process_id);
        }

    }

    void RunTest() {
        Log("[launch_test] starting\n");

        Result rc = ::pmshellInitialize();
        Log("[launch_test] %s: connect to pm:shell (rc=0x%x)\n", R_SUCCEEDED(rc) ? "PASS" : "FAIL", rc);

        if (R_SUCCEEDED(rc)) {
            u64 process_id = 0;
            rc = Launch(std::addressof(process_id), TestProgramId);
            Log("[launch_test] %s: launch program %016lx (rc=0x%x, process %lu)\n", R_SUCCEEDED(rc) ? "PASS" : "FAIL", TestProgramId, rc, process_id);

            rc = Launch(std::addressof(process_id), MissingProgramId);
            Log("[launch_test] %s: launching a program that is not installed fails (rc=0x%x)\n", R_FAILED(rc) ? "PASS" : "FAIL", rc);
        }

        Log("[launch_test] done\n");

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
