/*
 * Test for powering the machine off and rebooting it from userland. It asks
 * spl to set Atmosphere's ExosphereNeedsShutdown or ExosphereNeedsReboot
 * config item, through libnx's splSetConfig, which is how a system module
 * asks the secure monitor to do either. spl passes the request on with the
 * SetConfig SMC, which this board's kernel turns into PSCI SYSTEM_OFF or
 * SYSTEM_RESET.
 *
 * What it does is read from /power_test.txt on the SD card:
 *   off      power off (also the default, when the file is missing)
 *   reboot   reboot
 * optionally followed by a delay in seconds ("off 60"; default 3, at most
 * 3600). It logs, waits that long so the log can be seen (and so the wait
 * can be timed from outside), then makes the request. If the call returns at all, the machine was not powered off or
 * reset, and the test logs a failure.
 *
 * Put it on an image only for testing: with "reboot" it reboots the machine
 * every time it starts.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

/* libnx leaves the service headers out of switch.h, and they have no C linkage guard of their own. */
extern "C" {
    #include <switch/services/spl.h>
}

namespace {

    namespace {

        /* Atmosphere's config items (spl::ConfigItem); libnx names them the same. */
        constexpr u32 ConfigItem_ExosphereNeedsReboot   = 65001;
        constexpr u32 ConfigItem_ExosphereNeedsShutdown = 65002;

        /* The value of ExosphereNeedsReboot that asks for an ordinary reboot (Exosphere's UserRebootType_ByPmic). */
        /* On this board every reboot type is the same PSCI SYSTEM_RESET. */
        constexpr u64 UserRebootType_ByPmic = 4;

        constexpr s64 DefaultDelaySeconds = 3;
        constexpr s64 MaxDelaySeconds     = 3600;

        enum class Action {
            Off,
            Reboot,
        };

        struct Request {
            Action action;
            s64 delay_seconds;
        };

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        /* Reads the request from /power_test.txt on the SD card; anything that goes wrong means "off" after the default delay. */
        Request ReadRequest() {
            constexpr Request Default = { Action::Off, DefaultDelaySeconds };

            if (const Result rc = ::fsInitialize(); R_FAILED(rc)) {
                Log("[power_test] fsInitialize failed (rc=0x%x)\n", rc);
                return Default;
            }

            ::FsFileSystem sd;
            if (const Result rc = ::fsOpenSdCardFileSystem(std::addressof(sd)); R_FAILED(rc)) {
                Log("[power_test] no SD card (rc=0x%x)\n", rc);
                return Default;
            }
            kumo::ScopeExit close_sd([&] { ::fsFsClose(std::addressof(sd)); });

            /* libnx sends FS_MAX_PATH bytes for every path argument. */
            char path[FS_MAX_PATH] = "/power_test.txt";
            ::FsFile file;
            if (const Result rc = ::fsFsOpenFile(std::addressof(sd), path, FsOpenMode_Read, std::addressof(file)); R_FAILED(rc)) {
                Log("[power_test] no /power_test.txt (rc=0x%x)\n", rc);
                return Default;
            }
            kumo::ScopeExit close_file([&] { ::fsFileClose(std::addressof(file)); });

            char text[16] = {};
            u64 read = 0;
            if (R_FAILED(::fsFileRead(std::addressof(file), 0, text, sizeof(text) - 1, FsReadOption_None, std::addressof(read)))) {
                return Default;
            }

            Request request = Default;
            request.action = std::strncmp(text, "reboot", 6) == 0 ? Action::Reboot : Action::Off;

            /* An optional delay after the first word. */
            const char *p = text;
            while (*p != '\x00' && *p != ' ') { ++p; }
            while (*p == ' ') { ++p; }
            if ('0' <= *p && *p <= '9') {
                s64 seconds = 0;
                while ('0' <= *p && *p <= '9' && seconds <= MaxDelaySeconds) { seconds = seconds * 10 + (*p++ - '0'); }
                request.delay_seconds = std::min(seconds, MaxDelaySeconds);
            }
            return request;
        }

    }

    void RunTest() {
        Log("[power_test] starting\n");

        const Request request = ReadRequest();
        const Action action = request.action;
        const char * const name = action == Action::Reboot ? "reboot" : "power off";
        Log("[power_test] will %s in %ld seconds\n", name, request.delay_seconds);

        /* Through spl, as system modules are meant to reach the secure monitor. (libstratosphere also makes */
        /* GetConfig SMCs directly at start-up, which is why this module has svcCallSecureMonitor.) */
        Result rc = ::splInitialize();
        if (R_SUCCEEDED(rc)) {
            /* Sleep in steps of up to 10 seconds, logging each, so that the wait can be timed from */
            /* outside even when the last lines before the machine goes off are lost. */
            constexpr s64 StepSeconds = 10;
            for (s64 elapsed = 0; elapsed < request.delay_seconds; ) {
                const s64 step = std::min(StepSeconds, request.delay_seconds - elapsed);
                svcSleepThread(INT64_C(1000000000) * (step));
                elapsed += step;
                if (elapsed < request.delay_seconds) {
                    Log("[power_test] %ld of %ld seconds\n", elapsed, request.delay_seconds);
                }
            }

            Log("[power_test] requesting %s\n", name);
            if (action == Action::Reboot) {
                rc = ::splSetConfig(static_cast<::SplConfigItem>(ConfigItem_ExosphereNeedsReboot), UserRebootType_ByPmic);
            } else {
                rc = ::splSetConfig(static_cast<::SplConfigItem>(ConfigItem_ExosphereNeedsShutdown), 1);
            }
            Log("[power_test] FAIL: splSetConfig returned (rc=0x%x); the machine is still running\n", rc);
        } else {
            Log("[power_test] FAIL: splInitialize (rc=0x%x)\n", rc);
        }

        /* Idle forever; this KIP is immortal. */
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
