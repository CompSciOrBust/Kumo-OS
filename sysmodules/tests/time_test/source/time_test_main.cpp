/*
 * Test client for the time services (psc's time core and glue). Runs as a KIP,
 * uses libnx's time API the way an application would (timeInitialize, time(),
 * clock_gettime, timeGetCurrentTime, timeToCalendarTimeWithMyRule) and reports
 * each check on the kernel debug log, one PASS, FAIL or SKIP line each, then
 * "done: N checks, M failure(s), K skipped".
 *
 * What it covers:
 *   - timeInitialize on every service name (time:u, time:a, time:s, time:r, time:su), the shared
 *     memory page, and a second Initialize and Exit;
 *   - the steady clock: the shared memory and IPC answers agree, and it runs at the rate of the
 *     tick counter across a 2 s sleep;
 *   - the system clocks: IPC and shared memory agree; what each name may set; SetCurrentTime then
 *     GetCurrentTime; automatic correction; the context; the operation event;
 *   - the time zone service (UTC only) and calendar conversions, for known instants and back;
 *   - newlib: time(), gettimeofday(), clock_gettime() through libnx's __libnx_init_time;
 *   - the commands that are declared but not supported, which answer an error and leave the session
 *     working; the clock snapshot and difference commands;
 *   - the network clock, if an SNTP server answers (otherwise SKIP).
 * The IPC checks send commands with libnx's own dispatch, the way libnx's wrappers do.
 *
 * It needs sm, pm, psc, glue and bsdsockets (for SNTP); see scripts/run-time-test.sh.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#include <time.h>
#include <sys/time.h>

#include "../../../psc/native/time/timesrv_types.hpp"

/* libnx time initialization and newlib heap bounds. */
extern "C" {
    extern TimeServiceType __nx_time_service_type;
    void __libnx_init_time(void);
    extern char *fake_heap_start;
    extern char *fake_heap_end;
}

namespace {

    namespace timesrv = kumo::horizon::timesrv;

    namespace {

        /* nn::time results (module 116): value = 116 | (description << 9). */
        constexpr u32 ResultClockMismatch    = 0x0CC74;    /* 116-102 */
        constexpr u32 ResultNotSupported     = 0x7BC74;    /* 116-990 */
        constexpr u32 ResultPermissionDenied = 0x7D274;    /* 116-1001 */

        constexpr s64 NanoSecondsPerSecond = INT64_C(1000000000);

        /* Sanity floor for a time from the network: 2024-01-01. */
        constexpr s64 PlausibleNetworkTime = 1704067200;

        constexpr int NetworkWaitSeconds = 30;

        constinit int g_failures = 0;
        constinit int g_checks   = 0;
        constinit int g_skipped  = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        /* `value` is whatever explains a failure: usually a result code. */
        void Check(const char *what, bool ok, long value = 0) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[time_test] %s: %s (0x%lx)\n", ok ? "PASS" : "FAIL", what, value);
        }

        void Skip(const char *what, const char *reason) {
            ++g_skipped;
            Log("[time_test] SKIP: %s: %s\n", what, reason);
        }

        bool Near(s64 a, s64 b, s64 tolerance) {
            const s64 d = a - b;
            return d >= -tolerance && d <= tolerance;
        }

        alignas(0x1000) constinit char g_newlib_heap[1024 * 1024];

        /* ===== Sessions ===== */

        struct Name {
            const char *name;
            TimeServiceType type;
            bool can_set_user;      /* time:s, time:su and time:a */
            bool can_set_local;     /* time:s and time:a */
            bool can_set_network;   /* time:s */
        };

        constexpr Name Names[] = {
            { "time:u",  TimeServiceType_User,       false, false, false },
            { "time:a",  TimeServiceType_Menu,       true,  true,  false },
            { "time:s",  TimeServiceType_System,     true,  true,  true  },
            { "time:r",  TimeServiceType_Repair,     false, false, false },
            { "time:su", TimeServiceType_SystemUser, true,  false, false },
        };

        const Name &NameOf(TimeServiceType type) {
            for (const auto &n : Names) {
                if (n.type == type) {
                    return n;
                }
            }
            KUMO_ABORT("no such name");
        }

        Result Open(TimeServiceType type) {
            __nx_time_service_type = type;
            return ::timeInitialize();
        }

        /* A connection for a scope. */
        class Session {
            private:
                Result m_result;
            public:
                explicit Session(TimeServiceType type) : m_result(Open(type)) {
                    Check(NameOf(type).name, R_SUCCEEDED(m_result), m_result);
                }
                ~Session() {
                    if (R_SUCCEEDED(m_result)) {
                        ::timeExit();
                    }
                }
                bool IsOpen() const { return R_SUCCEEDED(m_result); }
        };

        /* ===== Raw IPC, as libnx's own wrappers send it ===== */

        ::Service *Static()   { return ::timeGetServiceSession(); }
        ::Service *Steady()   { return ::timeGetServiceSession_SteadyClock(); }
        ::Service *Zone()     { return ::timeGetServiceSession_TimeZoneService(); }
        ::Service *Clock(::TimeType type) { return ::timeGetServiceSession_SystemClock(type); }

        u32 IpcGetTime(::TimeType type, s64 *out) {
            *out = 0;
            return serviceDispatchOut(Clock(type), 0, *out);
        }

        u32 IpcSetTime(::TimeType type, s64 value) {
            return serviceDispatchIn(Clock(type), 1, value);
        }

        u32 IpcGetContext(::TimeType type, timesrv::SystemClockContext *out) {
            return serviceDispatchOut(Clock(type), 2, *out);
        }

        u32 IpcGetSteady(timesrv::SteadyClockTimePoint *out) {
            return serviceDispatchOut(Steady(), 0, *out);
        }

        u32 IpcGetBool(u32 command, bool *out) {
            u8 value = 0;
            const u32 rc = serviceDispatchOut(Static(), command, value);
            *out = value != 0;
            return rc;
        }

        u32 IpcSetAutomaticCorrection(bool enabled) {
            const u8 value = enabled ? 1 : 0;
            return serviceDispatchIn(Static(), 101, value);
        }

        u32 IpcGetSnapshot(timesrv::ClockSnapshot *out, u8 type) {
            return serviceDispatchIn(Static(), 400, type,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize },
                .buffers      = { { out, sizeof(*out) } },
            );
        }

        /* The TimeType numbers libnx uses for the three clocks. */
        constexpr ::TimeType User    = TimeType_UserSystemClock;
        constexpr ::TimeType Network = TimeType_NetworkSystemClock;
        constexpr ::TimeType Local   = TimeType_LocalSystemClock;

        /* libnx's Uuid has .uuid; the time-service layout uses .data. */
        bool IsZeroUuid(const u8 *uuid) {
            for (int i = 0; i < 16; ++i) {
                if (uuid[i] != 0) {
                    return false;
                }
            }
            return true;
        }

        bool SameUuid(const u8 *a, const u8 *b) {
            return std::memcmp(a, b, 16) == 0;
        }
        bool SameUuid(const timesrv::Uuid &a, const timesrv::Uuid &b) { return SameUuid(a.data, b.data); }
        bool IsZeroUuid(const timesrv::Uuid &a) { return IsZeroUuid(a.data); }

        /* ===== Steady clock ===== */

        void TestInitialize() {
            for (const auto &n : Names) {
                Session session(n.type);
                if (!session.IsOpen()) {
                    continue;
                }

                Check("  the shared memory page is mapped (hosversion >= 6.0.0)", ::timeGetSharedmemAddr() != nullptr);

                ::TimeSteadyClockTimePoint tp = {};
                const Result rc = ::timeGetStandardSteadyClockTimePoint(std::addressof(tp));
                Check("  timeGetStandardSteadyClockTimePoint works", R_SUCCEEDED(rc) && !IsZeroUuid(tp.source_id.uuid), rc);
            }

            /* libnx counts its initialisations. */
            Result rc = Open(TimeServiceType_User);
            Result rc2 = Open(TimeServiceType_User);
            Check("a second timeInitialize succeeds (reference count)", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2), rc2);
            ::timeExit();
            s64 now = 0;
            Check("the first timeExit leaves the session open", R_SUCCEEDED(IpcGetTime(User, std::addressof(now))));
            ::timeExit();

            rc = Open(TimeServiceType_System);
            Check("timeInitialize works again after the last timeExit (time:s)", R_SUCCEEDED(rc), rc);
            ::timeExit();
        }

        void TestSteadyClock() {
            Session session(TimeServiceType_SystemUser);
            if (!session.IsOpen()) {
                return;
            }

            timesrv::SteadyClockTimePoint a = {};
            Result rc = IpcGetSteady(std::addressof(a));
            Check("steady clock: GetCurrentTimePoint (IPC)", R_SUCCEEDED(rc) && !IsZeroUuid(a.source_id), rc);

            ::TimeSteadyClockTimePoint shm = {};
            rc = ::timeGetStandardSteadyClockTimePoint(std::addressof(shm));
            Check("steady clock: the shared memory answer has the same source ID and time", R_SUCCEEDED(rc) && SameUuid(shm.source_id.uuid, a.source_id.data) && Near(shm.time_point, a.time_point, 1), rc);

            s64 value = 0;
            rc = serviceDispatchOut(Steady(), 100, value);
            Check("steady clock: GetRtcValue", R_SUCCEEDED(rc), rc);

            u8 flag = 0;
            rc = serviceDispatchOut(Steady(), 101, flag);
            Check("steady clock: IsRtcResetDetected is false", R_SUCCEEDED(rc) && flag == 0, rc);

            u32 setup = 1;
            rc = serviceDispatchOut(Steady(), 102, setup);
            Check("steady clock: GetSetupResultValue is 0", R_SUCCEEDED(rc) && setup == 0, rc);

            s64 offset = 1;
            rc = ::timeGetStandardSteadyClockInternalOffset(std::addressof(offset));
            Check("steady clock: GetInternalOffset (libnx wrapper) is 0", R_SUCCEEDED(rc) && offset == 0, rc);

            s64 test_offset = 1;
            rc = serviceDispatchOut(Steady(), 2, test_offset);
            Check("steady clock: GetTestOffset is 0", R_SUCCEEDED(rc) && test_offset == 0, rc);

            /* Real rate: across a 2 s sleep the steady clock and the tick counter (svcGetSystemTick, 19.2 MHz) advance together. */
            const u64 tick_before = ::svcGetSystemTick();
            timesrv::SteadyClockTimePoint before = {};
            IpcGetSteady(std::addressof(before));
            svcSleepThread(INT64_C(1000000000) * 2);
            timesrv::SteadyClockTimePoint after = {};
            rc = IpcGetSteady(std::addressof(after));
            const u64 tick_after = ::svcGetSystemTick();

            const s64 tick_ms = static_cast<s64>(::armTicksToNs(tick_after - tick_before) / 1000000);
            Check("steady clock: a 2 s sleep is 1900 to 2600 ms of ticks", tick_ms >= 1900 && tick_ms <= 2600, tick_ms);
            const s64 steady_seconds = after.time_point - before.time_point;
            Check("steady clock: it advanced as the ticks did (within a second)", R_SUCCEEDED(rc) && Near(steady_seconds * 1000, tick_ms, 1000) && steady_seconds >= 1 && steady_seconds <= 3, steady_seconds);
            Check("steady clock: the source ID did not change", SameUuid(after.source_id, before.source_id) && SameUuid(after.source_id, a.source_id));
        }

        /* ===== System clocks ===== */

        void TestClocksRead() {
            Session session(TimeServiceType_User);
            if (!session.IsOpen()) {
                return;
            }

            timesrv::SteadyClockTimePoint steady = {};
            IpcGetSteady(std::addressof(steady));

            for (::TimeType type : { User, Local }) {
                const char *name = (type == User) ? "user" : "local";
                char what[0x80];

                s64 ipc = 0, shm = 0;
                Result rc = IpcGetTime(type, std::addressof(ipc));
                std::snprintf(what, sizeof(what), "%s clock: GetCurrentTime (IPC) is a time after 2025", name);
                Check(what, R_SUCCEEDED(rc) && ipc > 1735689600, rc);

                u64 value = 0;
                rc = ::timeGetCurrentTime(type, std::addressof(value));
                shm = static_cast<s64>(value);
                std::snprintf(what, sizeof(what), "%s clock: timeGetCurrentTime (shared memory) agrees with IPC", name);
                Check(what, R_SUCCEEDED(rc) && Near(shm, ipc, 1), rc);

                timesrv::SystemClockContext context = {};
                rc = IpcGetContext(type, std::addressof(context));
                std::snprintf(what, sizeof(what), "%s clock: the context is of the steady clock, and offset + steady is the time", name);
                s64 now = 0;
                IpcGetTime(type, std::addressof(now));
                IpcGetSteady(std::addressof(steady));
                Check(what, R_SUCCEEDED(rc) && SameUuid(context.timestamp.source_id, steady.source_id) && Near(context.offset + steady.time_point, now, 1), rc);
            }

            s64 user = 0, local = 0;
            IpcGetTime(User, std::addressof(user));
            IpcGetTime(Local, std::addressof(local));
            Check("the user and local clocks read the same (they share one context here)", Near(user, local, 1));

            /* The ephemeral network clock, through libnx's own session object for it: command 5 of the static service. */
            ::Service ephemeral = {};
            Result rc = serviceDispatch(Static(), 5, .out_num_objects = 1, .out_objects = std::addressof(ephemeral));
            Check("GetEphemeralNetworkSystemClock returns an object", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                s64 value = 0;
                rc = serviceDispatchOut(std::addressof(ephemeral), 0, value);
                Check("the ephemeral network clock is not set (116-102)", rc == ResultClockMismatch, rc);
                ::serviceClose(std::addressof(ephemeral));
            }

            bool enabled = false;
            rc = IpcGetBool(100, std::addressof(enabled));
            Check("automatic correction is on by default", R_SUCCEEDED(rc) && enabled, rc);

            s32 year = 0;
            rc = serviceDispatchOut(Static(), 102, year);
            Check("GetStandardUserSystemClockInitialYear is 2000", R_SUCCEEDED(rc) && year == 2000, rc);
        }

        /* The user clock and what can set it. */
        void TestSet() {
            constexpr s64 FirstTime  = 1700000000;     /* 2023-11-14T22:13:20Z */
            constexpr s64 SecondTime = 1800000000;     /* 2027-01-15T08:00:00Z */

            /* Automatic correction would let a late SNTP answer move the user clock under the checks below. */
            {
                Session session(TimeServiceType_System);
                if (!session.IsOpen()) {
                    return;
                }
                Check("time:s turns automatic correction off", R_SUCCEEDED(IpcSetAutomaticCorrection(false)));
            }

            for (const auto &n : Names) {
                Session session(n.type);
                if (!session.IsOpen()) {
                    continue;
                }

                Result rc = ::timeSetCurrentTime(User, static_cast<u64>(FirstTime));
                char what[0x80];
                if (n.can_set_user) {
                    s64 ipc = 0;
                    u64 shm = 0;
                    IpcGetTime(User, std::addressof(ipc));
                    ::timeGetCurrentTime(User, std::addressof(shm));
                    std::snprintf(what, sizeof(what), "  %s: SetCurrentTime on the user clock, then GetCurrentTime (IPC and shared memory)", n.name);
                    Check(what, R_SUCCEEDED(rc) && Near(ipc, FirstTime, 1) && Near(static_cast<s64>(shm), FirstTime, 1), rc);
                } else {
                    std::snprintf(what, sizeof(what), "  %s: SetCurrentTime on the user clock is refused (116-1001)", n.name);
                    Check(what, rc == ResultPermissionDenied, rc);
                }

                rc = ::timeSetCurrentTime(Local, static_cast<u64>(SecondTime));
                if (n.can_set_local) {
                    s64 ipc = 0;
                    IpcGetTime(Local, std::addressof(ipc));
                    s64 user = 0;
                    IpcGetTime(User, std::addressof(user));
                    std::snprintf(what, sizeof(what), "  %s: SetCurrentTime on the local clock (and the user clock reads the same)", n.name);
                    Check(what, R_SUCCEEDED(rc) && Near(ipc, SecondTime, 1) && Near(user, SecondTime, 1), rc);
                } else {
                    std::snprintf(what, sizeof(what), "  %s: SetCurrentTime on the local clock is refused", n.name);
                    Check(what, rc == ResultPermissionDenied, rc);
                }

                rc = ::timeSetCurrentTime(Network, static_cast<u64>(FirstTime));
                if (!n.can_set_network) {
                    std::snprintf(what, sizeof(what), "  %s: SetCurrentTime on the network clock is refused", n.name);
                    Check(what, rc == ResultPermissionDenied, rc);
                }
            }

            /* time:s: the network clock, and automatic correction. */
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }

            Result rc = ::timeSetCurrentTime(User, static_cast<u64>(FirstTime));
            Check("time:s: sets the user clock to a known time", R_SUCCEEDED(rc), rc);

            rc = ::timeSetCurrentTime(Network, static_cast<u64>(SecondTime));
            s64 network = 0, user = 0;
            IpcGetTime(Network, std::addressof(network));
            IpcGetTime(User, std::addressof(user));
            Check("time:s: sets the network clock; with automatic correction off the user clock is unchanged", R_SUCCEEDED(rc) && Near(network, SecondTime, 1) && Near(user, FirstTime, 1), rc);

            u64 shm = 0;
            rc = ::timeGetCurrentTime(Network, std::addressof(shm));
            Check("the network clock through the shared memory agrees", R_SUCCEEDED(rc) && Near(static_cast<s64>(shm), SecondTime, 1), rc);

            Check("time:s: turns automatic correction on; the user clock follows the network clock at once", R_SUCCEEDED(IpcSetAutomaticCorrection(true)));
            IpcGetTime(User, std::addressof(user));
            Check("  the user clock is the network time", Near(user, SecondTime, 1), user);

            bool enabled = false;
            IpcGetBool(100, std::addressof(enabled));
            Check("  automatic correction reads back as on", enabled);

            timesrv::SteadyClockTimePoint updated = {}, steady = {};
            rc = serviceDispatchOut(Static(), 201, updated);
            IpcGetSteady(std::addressof(steady));
            Check("  the automatic correction updated time is a point on the steady clock, a moment ago", R_SUCCEEDED(rc) && SameUuid(updated.source_id, steady.source_id) && updated.time_point <= steady.time_point && steady.time_point - updated.time_point <= 5, rc);

            bool sufficient = false;
            rc = IpcGetBool(200, std::addressof(sufficient));
            Check("IsStandardNetworkSystemClockAccuracySufficient is true once the network clock is set", R_SUCCEEDED(rc) && sufficient, rc);

            /* The context commands. */
            timesrv::SystemClockContext context = {};
            IpcGetContext(Network, std::addressof(context));
            context.offset += 1000;
            rc = serviceDispatchIn(Clock(Network), 3, context);
            IpcGetTime(Network, std::addressof(network));
            Check("time:s: SetSystemClockContext on the network clock moves it by the offset", R_SUCCEEDED(rc) && Near(network, SecondTime + 1000, 2), rc);
            IpcSetTime(Network, SecondTime);

            /* Back to the default: user and local the same, correction on, which is what the checks above assumed. */
        }

        /* The operation event of a clock is signalled when the clock is set. */
        void TestEvents() {
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }

            ::Handle handle = INVALID_HANDLE;
            Result rc = serviceDispatch(Clock(User), 4, .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = std::addressof(handle));
            Check("ISystemClock GetOperationEventReadableHandle returns a handle", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            ::svcResetSignal(handle);
            s32 index;
            rc = ::svcWaitSynchronization(std::addressof(index), std::addressof(handle), 1, 0);
            Check("  the event is not signalled", R_FAILED(rc), rc);

            s64 now = 0;
            IpcGetTime(User, std::addressof(now));
            IpcSetTime(User, now);
            rc = ::svcWaitSynchronization(std::addressof(index), std::addressof(handle), 1, 1 * NanoSecondsPerSecond);
            Check("  setting the clock signals it", R_SUCCEEDED(rc), rc);
            ::svcCloseHandle(handle);
        }

        /* ===== Time zone ===== */

        struct Instant {
            s64 posix;
            u16 year;
            u8 month, day, hour, minute, second;
            u32 wday, yday;
        };

        constexpr Instant Instants[] = {
            { 0,           1970, 1,  1,  0,  0,  0,  4,   0 },
            { 951782400,   2000, 2,  29, 0,  0,  0,  2,   59 },
            { 1700000000,  2023, 11, 14, 22, 13, 20, 2,   317 },
            { 4102444800,  2100, 1,  1,  0,  0,  0,  5,   0 },
            { -1,          1969, 12, 31, 23, 59, 59, 3,   364 },
        };

        alignas(8) constinit ::TimeZoneRule g_rule = {};
        /* For loads that are meant to fail: a failed call need not leave its output buffer as it was */
        /* (the kernel refills partial pages of a mapped buffer when it unmaps it), so never g_rule. */
        alignas(8) constinit ::TimeZoneRule g_scratch_rule = {};

        void TestTimeZone() {
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }

            ::TimeLocationName name = {};
            Result rc = ::timeGetDeviceLocationName(std::addressof(name));
            Check("GetDeviceLocationName is \"UTC\"", R_SUCCEEDED(rc) && std::strcmp(name.name, "UTC") == 0, rc);

            s32 count = 0;
            rc = ::timeGetTotalLocationNameCount(std::addressof(count));
            Check("GetTotalLocationNameCount is 1", R_SUCCEEDED(rc) && count == 1, rc);

            ::TimeLocationName list[4] = {};
            s32 listed = -1;
            rc = ::timeLoadLocationNameList(0, list, 4, std::addressof(listed));
            Check("LoadLocationNameList(0) lists UTC", R_SUCCEEDED(rc) && listed == 1 && std::strcmp(list[0].name, "UTC") == 0, rc);
            rc = ::timeLoadLocationNameList(1, list, 4, std::addressof(listed));
            Check("LoadLocationNameList(1) lists nothing", R_SUCCEEDED(rc) && listed == 0, rc);

            rc = ::timeLoadTimeZoneRule(std::addressof(name), std::addressof(g_rule));
            Check("LoadTimeZoneRule(UTC) fills a rule", R_SUCCEEDED(rc), rc);
            timesrv::TimeZoneRuleLayout rule;
            std::memcpy(std::addressof(rule), std::addressof(g_rule), sizeof(rule));
            Check("  the rule has one type, no transitions, offset 0, named UTC", rule.time_count == 0 && rule.type_count == 1 && rule.types[0].gmt_offset == 0 && std::strcmp(rule.chars, "UTC") == 0);

            ::TimeLocationName other = {};
            std::strcpy(other.name, "Europe/London");
            rc = ::timeLoadTimeZoneRule(std::addressof(other), std::addressof(g_scratch_rule));
            Check("LoadTimeZoneRule(Europe/London) fails: there is no TimeZoneBinary archive", R_FAILED(rc), rc);
            rc = ::timeSetDeviceLocationName(std::addressof(other));
            Check("SetDeviceLocationName(Europe/London) fails", R_FAILED(rc), rc);
            rc = ::timeSetDeviceLocationName(std::addressof(name));
            Check("SetDeviceLocationName(UTC) works on time:s", R_SUCCEEDED(rc), rc);

            ::TimeLocationName version_check = {};
            timesrv::TimeZoneRuleVersion version = {};
            rc = serviceDispatchOut(Zone(), 5, version);
            Check("GetTimeZoneRuleVersion", R_SUCCEEDED(rc), rc);
            (void)version_check;

            for (const auto &i : Instants) {
                char what[0x80];
                ::TimeCalendarTime calendar = {};
                ::TimeCalendarAdditionalInfo info = {};
                rc = ::timeToCalendarTimeWithMyRule(static_cast<u64>(i.posix), std::addressof(calendar), std::addressof(info));
                std::snprintf(what, sizeof(what), "ToCalendarTimeWithMyRule(%lld) is %04u-%02u-%02u %02u:%02u:%02u, wday %u, yday %u, UTC +0", static_cast<long long>(i.posix), i.year, i.month, i.day, i.hour, i.minute, i.second, i.wday, i.yday);
                Check(what, R_SUCCEEDED(rc) && calendar.year == i.year && calendar.month == i.month && calendar.day == i.day && calendar.hour == i.hour && calendar.minute == i.minute && calendar.second == i.second
                         && info.wday == i.wday && info.yday == i.yday && std::strcmp(info.timezoneName, "UTC") == 0 && info.DST == 0 && info.offset == 0, rc);

                u64 list_out[2] = {};
                s32 posix_count = 0;
                rc = ::timeToPosixTimeWithMyRule(std::addressof(calendar), list_out, 2, std::addressof(posix_count));
                std::snprintf(what, sizeof(what), "  ToPosixTimeWithMyRule gives %lld back, once", static_cast<long long>(i.posix));
                Check(what, R_SUCCEEDED(rc) && posix_count == 1 && static_cast<s64>(list_out[0]) == i.posix, rc);

                /* With the rule loaded from the service. */
                ::TimeCalendarTime calendar2 = {};
                ::TimeCalendarAdditionalInfo info2 = {};
                rc = ::timeToCalendarTime(std::addressof(g_rule), static_cast<u64>(i.posix), std::addressof(calendar2), std::addressof(info2));
                u64 list_out2[2] = {};
                s32 posix_count2 = 0;
                const Result rc2 = ::timeToPosixTime(std::addressof(g_rule), std::addressof(calendar2), list_out2, 2, std::addressof(posix_count2));
                std::snprintf(what, sizeof(what), "  the same with the loaded rule (ToCalendarTime, ToPosixTime) for %lld", static_cast<long long>(i.posix));
                Check(what, R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && std::memcmp(std::addressof(calendar), std::addressof(calendar2), sizeof(calendar)) == 0 && posix_count2 == 1 && static_cast<s64>(list_out2[0]) == i.posix, rc2);
            }

            /* Invalid calendar times. */
            struct { const char *what; ::TimeCalendarTime time; } invalid[] = {
                { "month 13",                  { 2024, 13, 1,  0,  0,  0, 0 } },
                { "month 0",                   { 2024, 0,  1,  0,  0,  0, 0 } },
                { "day 30 of February",        { 2024, 2,  30, 0,  0,  0, 0 } },
                { "29 February 2023 (not a leap year)", { 2023, 2, 29, 0, 0, 0, 0 } },
                { "hour 24",                   { 2024, 1,  1,  24, 0,  0, 0 } },
                { "second 60",                 { 2024, 1,  1,  0,  0,  60, 0 } },
            };
            for (const auto &entry : invalid) {
                u64 out[2] = {};
                s32 out_count = 0;
                rc = ::timeToPosixTimeWithMyRule(std::addressof(entry.time), out, 2, std::addressof(out_count));
                char what[0x80];
                std::snprintf(what, sizeof(what), "ToPosixTime refuses %s", entry.what);
                Check(what, R_FAILED(rc), rc);
            }

            /* A rule the caller makes: the conversions use the rule they are given, not UTC. Fixed offset +01:00, as a zone with no changes. */
            timesrv::TimeZoneRuleLayout *plus_one = reinterpret_cast<timesrv::TimeZoneRuleLayout *>(std::addressof(g_rule));
            plus_one->type_count = 1;
            plus_one->time_count = 0;
            plus_one->types[0].gmt_offset = 3600;
            std::memcpy(plus_one->chars, "CET", 4);
            ::TimeCalendarTime calendar = {};
            ::TimeCalendarAdditionalInfo info = {};
            rc = ::timeToCalendarTime(std::addressof(g_rule), 1700000000, std::addressof(calendar), std::addressof(info));
            Check("ToCalendarTime with a caller's +01:00 rule: 23:13:20, offset 3600, name CET", R_SUCCEEDED(rc) && calendar.hour == 23 && calendar.minute == 13 && info.offset == 3600 && std::strcmp(info.timezoneName, "CET") == 0, rc);
            u64 out[2] = {};
            s32 out_count = 0;
            rc = ::timeToPosixTime(std::addressof(g_rule), std::addressof(calendar), out, 2, std::addressof(out_count));
            Check("  ToPosixTime with it gives the instant back", R_SUCCEEDED(rc) && out_count == 1 && out[0] == 1700000000, rc);

            std::memset(std::addressof(g_rule), 0xFF, sizeof(g_rule));
            rc = ::timeToCalendarTime(std::addressof(g_rule), 0, std::addressof(calendar), std::addressof(info));
            Check("ToCalendarTime refuses a rule that is not one (counts out of range)", R_FAILED(rc), rc);
        }

        /* ===== newlib ===== */

        void TestNewlib() {
            Session session(TimeServiceType_User);
            if (!session.IsOpen()) {
                return;
            }

            /* This test controls startup explicitly, so initialize newlib's clock here. */
            __libnx_init_time();

            s64 ipc = 0;
            IpcGetTime(User, std::addressof(ipc));

            const time_t t = ::time(nullptr);
            Check("time() agrees with the user clock (within 2 s)", t != static_cast<time_t>(-1) && Near(t, ipc, 2), static_cast<long>(t));

            struct timespec ts = {};
            int ret = ::clock_gettime(CLOCK_REALTIME, std::addressof(ts));
            Check("clock_gettime(CLOCK_REALTIME) agrees", ret == 0 && Near(ts.tv_sec, ipc, 2), ret);

            struct timeval tv = {};
            ret = ::gettimeofday(std::addressof(tv), nullptr);
            Check("gettimeofday() agrees", ret == 0 && Near(tv.tv_sec, ipc, 2), ret);

            const char *tz = ::getenv("TZ");
            Check("TZ was set from the time zone to <UTC>-00:00:00", tz != nullptr && std::strcmp(tz, "<UTC>-00:00:00") == 0);

            struct timespec before = {}, after = {};
            ::clock_gettime(CLOCK_MONOTONIC, std::addressof(before));
            svcSleepThread(INT64_C(1000000000) * 2);
            ::clock_gettime(CLOCK_MONOTONIC, std::addressof(after));
            const s64 elapsed_ms = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000;
            Check("clock_gettime advances 2 s across a 2 s sleep (1900 to 2600 ms)", elapsed_ms >= 1900 && elapsed_ms <= 2600, elapsed_ms);

            /* libnx reads the clock once, at __libnx_init_time: SetCurrentTime does not move time() afterwards. That is libnx, not the service. */
        }

        /* ===== Commands that are declared but not supported ===== */

        void TestRefusals() {
            Session session(TimeServiceType_SystemUser);
            if (!session.IsOpen()) {
                return;
            }

            struct Command {
                const char *what;
                ::Service *service;
                u32 id;
            };
            const Command commands[] = {
                { "IStaticService 600 GetInitialLaunchEndTime",                   Static(), 600 },
                { "IStaticService 601 NotifyInitialLaunchSettingsDone",           Static(), 601 },
                { "ITimeZoneService 6 GetDeviceLocationNameAndUpdatedTime",       Zone(),   6 },
                { "ITimeZoneService 7 SetDeviceLocationNameWithTimeZoneRule",     Zone(),   7 },
                { "ISteadyClock 201 SetInternalOffset (3.0.0 to 3.0.2 only)",     Steady(), 201 },
            };

            for (const auto &c : commands) {
                /* 201 takes an s64 in; the others take nothing. */
                const s64 zero = 0;
                const Result rc = (c.id == 201) ? serviceDispatchIn(c.service, c.id, zero) : serviceDispatch(c.service, c.id);
                char what[0x100];
                std::snprintf(what, sizeof(what), "%s answers 116-990", c.what);
                Check(what, rc == ResultNotSupported, rc);

                /* The session that got the command, and the other two the client holds, still answer. */
                bool enabled = false;
                s32 location_count = 0;
                timesrv::SteadyClockTimePoint probe = {};
                const Result rc_static = IpcGetBool(100, std::addressof(enabled));
                const Result rc_zone   = serviceDispatchOut(Zone(), 2, location_count);
                const Result rc_steady = IpcGetSteady(std::addressof(probe));
                Check("  and the sessions still work (static, time zone, steady clock)", R_SUCCEEDED(rc_static) && R_SUCCEEDED(rc_zone) && R_SUCCEEDED(rc_steady) && location_count == 1, rc_static | rc_zone | rc_steady);
            }

            /* A command ID the tables do not have is answered, as it is for every service built on this framework (an error, and the session may be closed): not tested. */
        }

        /* ===== Snapshots and differences ===== */

        /* Exercise glue's pointer-buffer forwarding, not just psc's direct ports. */
        void TestGlueBuffers() {
            Session session(TimeServiceType_User);
            if (!session.IsOpen()) return;
            timesrv::ClockSnapshot a = {}, b = {};
            Result rc = IpcGetSnapshot(&a, TimeType_UserSystemClock);
            Check("time:u snapshot 400", R_SUCCEEDED(rc), rc);
            struct { u8 type; u8 pad[7]; timesrv::SystemClockContext user, network; } input = {};
            input.type = TimeType_UserSystemClock;
            input.user = a.user_context;
            input.network = a.network_context;
            rc = serviceDispatchIn(Static(), 401, input,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize },
                .buffers = { { &b, sizeof(b) } });
            Check("time:u snapshot 401", R_SUCCEEDED(rc) && Near(a.user_time, b.user_time, 1), rc);
            for (u32 command : {500u, 501u}) {
                s64 value = -1;
                rc = serviceDispatchOut(Static(), command, value,
                    .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize, SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize },
                    .buffers = { { &a, sizeof(a) }, { &a, sizeof(a) } });
                Check(command == 500 ? "time:u snapshot difference 500" : "time:u snapshot span 501", R_SUCCEEDED(rc) && value == 0, rc);
            }
            ::TimeCalendarTime calendar = {};
            ::TimeCalendarAdditionalInfo info = {};
            rc = ::timeToCalendarTimeWithMyRule(0, &calendar, &info);
            u64 values[4] = {}; s32 count = 0;
            if (R_SUCCEEDED(rc)) rc = ::timeToPosixTimeWithMyRule(&calendar, values, 4, &count);
            Check("time:u pointer output 202", R_SUCCEEDED(rc) && count == 1 && values[0] == 0, rc);
            ::TimeLocationName name = {}; std::strcpy(name.name, "UTC");
            rc = ::timeLoadTimeZoneRule(&name, &g_scratch_rule);
            count = 0;
            if (R_SUCCEEDED(rc)) rc = ::timeToPosixTime(&g_scratch_rule, &calendar, values, 4, &count);
            Check("time:u mapped input and pointer output 201", R_SUCCEEDED(rc) && count == 1 && values[0] == 0, rc);
        }

        void TestSnapshots() {
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }

            timesrv::ClockSnapshot a = {};
            Result rc = IpcGetSnapshot(std::addressof(a), TimeType_NetworkSystemClock);
            s64 user = 0;
            IpcGetTime(User, std::addressof(user));
            timesrv::SteadyClockTimePoint steady = {};
            IpcGetSteady(std::addressof(steady));
            Check("GetClockSnapshot (400)", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            Check("  the user time and context are the user clock's", Near(a.user_time, user, 1) && Near(a.user_context.offset + steady.time_point, user, 1));
            Check("  the steady clock time point has this boot's source ID", SameUuid(a.steady_clock_time_point.source_id, steady.source_id) && Near(a.steady_clock_time_point.time_point, steady.time_point, 1));
            Check("  the location is UTC, the type is echoed, the automatic correction flag is as set", std::strcmp(a.location_name.name, "UTC") == 0 && a.type == TimeType_NetworkSystemClock && a.is_automatic_correction_enabled);

            ::TimeCalendarTime calendar = {};
            ::TimeCalendarAdditionalInfo info = {};
            ::timeToCalendarTimeWithMyRule(static_cast<u64>(a.user_time), std::addressof(calendar), std::addressof(info));
            Check("  the user calendar time is the calendar time of the user time", a.user_calendar_time.year == calendar.year && a.user_calendar_time.month == calendar.month && a.user_calendar_time.day == calendar.day && a.user_calendar_time.hour == calendar.hour && a.user_calendar_additional_info.wday == info.wday);

            /* 401: from the contexts of the snapshot. */
            struct {
                u8 type;
                u8 pad[7];
                timesrv::SystemClockContext user_context;
                timesrv::SystemClockContext network_context;
            } input = {};
            input.type            = TimeType_UserSystemClock;
            input.user_context    = a.user_context;
            input.network_context = a.network_context;
            timesrv::ClockSnapshot b = {};
            rc = serviceDispatchIn(Static(), 401, input,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize },
                .buffers      = { { std::addressof(b), sizeof(b) } },
            );
            Check("GetClockSnapshotFromSystemClockContext (401) gives the same time", R_SUCCEEDED(rc) && Near(b.user_time, a.user_time, 1) && b.type == TimeType_UserSystemClock, rc);

            input.user_context.timestamp.source_id = {};
            rc = serviceDispatchIn(Static(), 401, input,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out | SfBufferAttr_FixedSize },
                .buffers      = { { std::addressof(b), sizeof(b) } },
            );
            Check("  a user context of another steady clock is refused (116-102)", rc == ResultClockMismatch, rc);

            /* 501: the span between two snapshots 2 s apart. */
            svcSleepThread(INT64_C(1000000000) * 2);
            timesrv::ClockSnapshot c = {};
            rc = IpcGetSnapshot(std::addressof(c), TimeType_UserSystemClock);
            s64 span = 0;
            rc = serviceDispatchOut(Static(), 501, span,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize, SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize },
                .buffers      = { { std::addressof(a), sizeof(a) }, { std::addressof(c), sizeof(c) } },
            );
            Check("CalculateSpanBetween (501) of snapshots 2 s apart is 1 to 3 s (nanoseconds)", R_SUCCEEDED(rc) && span >= 1 * NanoSecondsPerSecond && span <= 3 * NanoSecondsPerSecond, static_cast<long>(span / 1000000));

            /* 500: the user sets the clock 100 s ahead between two snapshots. */
            s64 now = 0;
            IpcGetTime(User, std::addressof(now));
            timesrv::ClockSnapshot d = {};
            IpcGetSnapshot(std::addressof(d), TimeType_UserSystemClock);
            IpcSetTime(User, now + 100);
            timesrv::ClockSnapshot e = {};
            IpcGetSnapshot(std::addressof(e), TimeType_UserSystemClock);
            s64 difference = 0;
            rc = serviceDispatchOut(Static(), 500, difference,
                .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize, SfBufferAttr_HipcPointer | SfBufferAttr_In | SfBufferAttr_FixedSize },
                .buffers      = { { std::addressof(d), sizeof(d) }, { std::addressof(e), sizeof(e) } },
            );
            Check("CalculateStandardUserSystemClockDifferenceByUser (500) after setting the clock 100 s ahead is 99 to 101 s", R_SUCCEEDED(rc) && difference >= 99 * NanoSecondsPerSecond && difference <= 101 * NanoSecondsPerSecond, static_cast<long>(difference / 1000000));
            IpcSetTime(User, now);

            /* 300. */
            timesrv::SystemClockContext context = {};
            IpcGetContext(User, std::addressof(context));
            s64 base = 0;
            rc = serviceDispatchInOut(Static(), 300, context, base);
            Check("CalculateMonotonicSystemClockBaseTimePoint (300) is the context's offset (the steady clock starts at the counter's zero)", R_SUCCEEDED(rc) && Near(base, context.offset, 1), rc);
        }

        /* ===== The network clock ===== */

        s64 g_network_time_at_check = 0;
        s64 g_network_steady_at_check = 0;

        void TestNetwork() {
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }

            /* SNTP starts when bsd:u is there and the interface has an address; give it NetworkWaitSeconds. */
            bool sufficient = false;
            for (int i = 0; i < NetworkWaitSeconds && !sufficient; ++i) {
                IpcGetBool(200, std::addressof(sufficient));
                if (!sufficient) {
                    svcSleepThread(INT64_C(1000000000) * 1);
                }
            }
            if (!sufficient) {
                Skip("the network clock was set by SNTP", "no SNTP server answered (no network, or no server reachable; see docs/time.md#sntp)");
                return;
            }

            s64 network = 0;
            Result rc = IpcGetTime(Network, std::addressof(network));
            Check("SNTP: the network clock is set, to a time after 2024", R_SUCCEEDED(rc) && network > PlausibleNetworkTime, network);

            u64 shm = 0;
            rc = ::timeGetCurrentTime(Network, std::addressof(shm));
            Check("  the shared memory path agrees", R_SUCCEEDED(rc) && Near(static_cast<s64>(shm), network, 1), rc);

            s64 user = 0;
            IpcGetTime(User, std::addressof(user));
            bool automatic = false;
            IpcGetBool(100, std::addressof(automatic));
            Check("  with automatic correction on, the user clock is the network time too", automatic && Near(user, network, 1), user - network);

            timesrv::SteadyClockTimePoint steady = {};
            IpcGetSteady(std::addressof(steady));
            g_network_time_at_check   = network;
            g_network_steady_at_check = steady.time_point;
            Log("[time_test] info: network time %lld at steady %lld\n", static_cast<long long>(network), static_cast<long long>(steady.time_point));
        }

        /* What the scripts compare with the host's clock: the network time the checks above saw, advanced by the steady clock. */
        void ReportNetworkTime() {
            if (g_network_time_at_check == 0) {
                return;
            }
            Session session(TimeServiceType_System);
            if (!session.IsOpen()) {
                return;
            }
            timesrv::SteadyClockTimePoint steady = {};
            IpcGetSteady(std::addressof(steady));
            const s64 now = g_network_time_at_check + (steady.time_point - g_network_steady_at_check);

            /* The checks that set clocks have left them at made-up times: put both back. */
            IpcSetTime(Network, now);
            IpcSetTime(User, now);
            IpcSetAutomaticCorrection(true);
            Log("[time_test] final network time %lld\n", static_cast<long long>(now));
        }

    }

    void RunTests() {
        Log("[time_test] starting\n");

        /* Everything that does not set a clock comes first, then the network (SNTP needs time to answer), then the checks that set clocks. */
        TestInitialize();
        TestSteadyClock();
        TestClocksRead();
        TestNewlib();
        TestTimeZone();
        TestRefusals();
        TestNetwork();
        TestSet();
        TestEvents();
        TestSnapshots();
        TestGlueBuffers();
        ReportNetworkTime();

        Log("[time_test] done: %d checks, %d failure(s), %d skipped\n", g_checks, g_failures, g_skipped);

        /* Idle forever; this KIP is immortal. */
        while (true) {
            svcSleepThread(INT64_C(1000000000) * 60);
        }
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        fake_heap_start = g_newlib_heap;
        fake_heap_end = g_newlib_heap + sizeof(g_newlib_heap);
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

int main(int, char **) { RunTests(); return 0; }
