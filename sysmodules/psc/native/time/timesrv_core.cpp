#include <kumo/prelude.hpp>
#include <kumo/mutex.hpp>
#include "timesrv_core.hpp"
#include "timesrv_utc.hpp"
#include "../../source/psc_log.hpp"

namespace kumo::horizon::timesrv::srv {

    namespace {

        /* Contexts: 0 is the local and user clocks, 1 the network clock, 2 the ephemeral network clock. */
        constexpr int SlotLocalUser = 0;
        constexpr int SlotNetwork   = 1;
        constexpr int SlotEphemeral = 2;
        constexpr int SlotCount     = 3;

        enum EventIndex {
            EventIndex_LocalUser,
            EventIndex_Network,
            EventIndex_Ephemeral,
            EventIndex_AutomaticCorrection,
            EventIndex_Alarm,
            EventIndex_Location,
            EventIndex_PowerState,
            EventIndex_Count,
        };

        constinit kumo::Mutex g_mutex;
        constinit bool g_initialized = false;

        constinit Uuid g_steady_source_id = {};
        constinit s64 g_internal_offset   = 0;
        constinit s64 g_test_offset       = 0;
        constinit s64 g_rtc_offset        = 0;
        constinit bool g_rtc_reset        = false;

        constinit SystemClockContext g_context[SlotCount] = {};
        constinit bool g_automatic_correction   = DefaultAutomaticCorrection;
        constinit u64 g_accuracy_minutes        = DefaultSufficientAccuracyMinutes;
        constinit SteadyClockTimePoint g_user_updated_time = {};

        constinit LocationName g_location = {};
        constinit TimeZoneRuleLayout g_utc_rule = {};

        Event g_events[EventIndex_Count];

        SharedMemory g_shared_memory;
        constinit SharedMemoryLayout *g_shared = nullptr;

        int SlotOf(ClockId clock) {
            switch (clock) {
                case ClockId_User:
                case ClockId_Local:     return SlotLocalUser;
                case ClockId_Network:   return SlotNetwork;
                case ClockId_Ephemeral: return SlotEphemeral;
                default: KUMO_ABORT("invalid clock");
            }
        }

        /* The steady clock is the tick counter, which the kernel runs at the Switch's 19.2 MHz everywhere, as libnx assumes. */
        s64 SteadyNanoSeconds() {
            return static_cast<s64>(::armTicksToNs(::armGetSystemTick()));
        }

        SteadyClockTimePoint SteadyTimePoint() {
            return SteadyClockTimePoint{ SteadyNanoSeconds() / INT64_C(1000000000), g_steady_source_id };
        }

        /* The writer of libnx's sequence protocol: fill the slot the readers are not using, then make it the current one. */
        template<typename T>
        void Publish(SharedLockFree<T> *dst, const T &value) {
            if (dst == nullptr) {
                return;
            }
            const u32 next = __atomic_load_n(std::addressof(dst->counter), __ATOMIC_RELAXED) + 1;
            std::memcpy(std::addressof(dst->slots[next & 1]), std::addressof(value), sizeof(T));
            __atomic_store_n(std::addressof(dst->counter), next, __ATOMIC_RELEASE);
        }

        bool Matches(const SystemClockContext &context) {
            return context.timestamp.source_id == g_steady_source_id;
        }

        /* The caller holds g_mutex. */
        void SetContextLocked(int slot, const SystemClockContext &context) {
            g_context[slot] = context;
            if (slot == SlotLocalUser) {
                Publish(g_shared != nullptr ? std::addressof(g_shared->local) : nullptr, context);
            } else if (slot == SlotNetwork) {
                Publish(g_shared != nullptr ? std::addressof(g_shared->network) : nullptr, context);
            }
            KUMO_ABORT_RESULT(eventFire(&g_events[slot]));
        }

        void PublishAutomaticCorrection() {
            if (g_shared != nullptr) {
                __atomic_store_n(std::addressof(g_shared->automatic_correction_enabled), static_cast<u8>(g_automatic_correction ? 1 : 0), __ATOMIC_RELEASE);
            }
        }

        s64 ClockTime(const SystemClockContext &context, const SteadyClockTimePoint &now) {
            return context.offset + now.time_point;
        }

        void FillCalendar(CalendarTime *out_time, CalendarAdditionalInfo *out_info, s64 posix_time) {
            std::memset(out_time, 0, sizeof(*out_time));
            std::memset(out_info, 0, sizeof(*out_info));
            /* The UTC rule is valid, so the only failure is a year outside 1 to 65535; the calendar is then left zero. */
            if (R_FAILED(ToCalendarTime(out_time, out_info, posix_time, g_utc_rule))) {
                std::memset(out_time, 0, sizeof(*out_time));
                std::memset(out_info, 0, sizeof(*out_info));
            }
        }

    }

    void InitializeCore() {
        std::scoped_lock lk(g_mutex);
        if (g_initialized) KUMO_ABORT("time core already initialized");

        /* A source ID for this boot only: nothing is saved, so a context from an earlier boot can never be presented. A version 4 UUID, so it is never the invalid (all zero) ID. */
        randomGet(g_steady_source_id.data, sizeof(g_steady_source_id.data));
        g_steady_source_id.data[6] = (g_steady_source_id.data[6] & 0x0F) | 0x40;
        g_steady_source_id.data[8] = (g_steady_source_id.data[8] & 0x3F) | 0x80;

        for (auto &event : g_events) {
            KUMO_ABORT_RESULT(eventCreate(&event, false));
        }

        BuildUtcRule(std::addressof(g_utc_rule));
        std::memcpy(g_location.name, "UTC", 4);

        /* The shared memory page, read-only to clients. */
        if (R_SUCCEEDED(shmemCreate(&g_shared_memory, SharedMemorySize, Perm_Rw, Perm_R))) {
            if (R_SUCCEEDED(shmemMap(&g_shared_memory))) {
                g_shared = static_cast<SharedMemoryLayout *>(shmemGetAddr(&g_shared_memory));
            } else {
                shmemClose(&g_shared_memory);
            }
        }
        if (g_shared == nullptr) {
            psc::srv::Log("[psc] time: could not map the shared memory page; libnx 6.0.0+ clients will fail\n");
        }

        /* The steady clock: base_time 0, so libnx's (base_time + armTicksToNs(tick)) is exactly what GetCurrentTimePoint answers. */
        const SteadyClockSharedRecord steady = { 0, g_steady_source_id };
        Publish(g_shared != nullptr ? std::addressof(g_shared->steady) : nullptr, steady);

        /* The local and user clocks start at the default time; the network clocks start uninitialized (an invalid source ID never matches). */
        const SteadyClockTimePoint now = SteadyTimePoint();
        g_context[SlotLocalUser] = SystemClockContext{ DefaultWallClockSeconds - now.time_point, now };
        g_context[SlotNetwork]   = SystemClockContext{};
        g_context[SlotEphemeral] = SystemClockContext{};
        g_user_updated_time      = SteadyClockTimePoint{};

        if (g_shared != nullptr) {
            Publish(std::addressof(g_shared->local),   g_context[SlotLocalUser]);
            Publish(std::addressof(g_shared->network), g_context[SlotNetwork]);
            PublishAutomaticCorrection();
        }

        g_initialized = true;

        psc::srv::Log("[psc] time: clocks start at %lld (default)\n", static_cast<long long>(DefaultWallClockSeconds));
    }

    /* ===== Steady clock ===== */

    s64 GetSteadyNanoSeconds() {
        return SteadyNanoSeconds();
    }

    SteadyClockTimePoint GetSteadyTimePoint() {
        return SteadyTimePoint();
    }

    const Uuid &GetSteadySourceId() {
        return g_steady_source_id;
    }

    s64 GetSteadyClockInternalOffset() { std::scoped_lock lk(g_mutex); return g_internal_offset; }
    s64 GetSteadyClockTestOffset()     { std::scoped_lock lk(g_mutex); return g_test_offset; }
    s64 GetSteadyClockRtcOffset()      { std::scoped_lock lk(g_mutex); return g_rtc_offset; }
    bool IsRtcResetDetected()          { std::scoped_lock lk(g_mutex); return g_rtc_reset; }

    /* The offsets are kept and reported back; they do not change what the steady clock reads, which has no RTC behind it. */
    void SetSteadyClockInternalOffset(s64 offset) { std::scoped_lock lk(g_mutex); g_internal_offset = offset; }
    void SetSteadyClockTestOffset(s64 offset)     { std::scoped_lock lk(g_mutex); g_test_offset = offset; }
    void SetSteadyClockBaseTime(s64 rtc_offset)   { std::scoped_lock lk(g_mutex); g_rtc_offset = rtc_offset; }

    void SetupSteadyClock(s64 rtc_offset, s64 internal_offset, s64 test_offset, bool is_rtc_reset_detected) {
        std::scoped_lock lk(g_mutex);
        g_rtc_offset      = rtc_offset;
        g_internal_offset = internal_offset;
        g_test_offset     = test_offset;
        g_rtc_reset       = is_rtc_reset_detected;
    }

    /* ===== System clocks ===== */

    Result GetCurrentTime(ClockId clock, s64 *out) {
        const SteadyClockTimePoint now = SteadyTimePoint();

        std::scoped_lock lk(g_mutex);
        const SystemClockContext &context = g_context[SlotOf(clock)];
        KUMO_UNLESS(Matches(context), ResultClockMismatch());

        *out = ClockTime(context, now);
        KUMO_SUCCEED();
    }

    Result SetCurrentTime(ClockId clock, s64 posix_time) {
        std::scoped_lock lk(g_mutex);

        const SteadyClockTimePoint now = SteadyTimePoint();
        const SystemClockContext context = { posix_time - now.time_point, now };
        const int slot = SlotOf(clock);
        SetContextLocked(slot, context);

        /* A time set by hand on the network clock is as good as one from the network. */
        if (slot == SlotNetwork && g_automatic_correction) {
            SetContextLocked(SlotLocalUser, context);
            g_user_updated_time = now;
            KUMO_ABORT_RESULT(eventFire(&g_events[EventIndex_AutomaticCorrection]));
        }
        KUMO_SUCCEED();
    }

    SystemClockContext GetClockContext(ClockId clock) {
        std::scoped_lock lk(g_mutex);
        return g_context[SlotOf(clock)];
    }

    void SetClockContext(ClockId clock, const SystemClockContext &context) {
        std::scoped_lock lk(g_mutex);
        SetContextLocked(SlotOf(clock), context);
    }

    bool IsClockInitialized(ClockId clock) {
        std::scoped_lock lk(g_mutex);
        return Matches(g_context[SlotOf(clock)]);
    }

    Handle GetClockEventHandle(ClockId clock) {
        return g_events[SlotOf(clock)].revent;
    }

    void UpdateNetworkClock(s64 posix_time, const SteadyClockTimePoint &steady_time_point) {
        const SystemClockContext context = { posix_time - steady_time_point.time_point, steady_time_point };

        std::scoped_lock lk(g_mutex);
        SetContextLocked(SlotNetwork, context);
        if (g_automatic_correction) {
            SetContextLocked(SlotLocalUser, context);
            g_user_updated_time = steady_time_point;
            KUMO_ABORT_RESULT(eventFire(&g_events[EventIndex_AutomaticCorrection]));
        }
    }

    bool IsAutomaticCorrectionEnabled() {
        std::scoped_lock lk(g_mutex);
        return g_automatic_correction;
    }

    void SetAutomaticCorrectionEnabled(bool enabled) {
        std::scoped_lock lk(g_mutex);
        g_automatic_correction = enabled;
        PublishAutomaticCorrection();

        /* Turning it on makes the user clock follow the network clock at once, if that is known. */
        if (enabled && Matches(g_context[SlotNetwork])) {
            SetContextLocked(SlotLocalUser, g_context[SlotNetwork]);
            g_user_updated_time = SteadyTimePoint();
        }
        KUMO_ABORT_RESULT(eventFire(&g_events[EventIndex_AutomaticCorrection]));
    }

    SteadyClockTimePoint GetAutomaticCorrectionUpdatedTime() {
        std::scoped_lock lk(g_mutex);
        return g_user_updated_time;
    }

    bool IsNetworkClockAccuracySufficient() {
        const SteadyClockTimePoint now = SteadyTimePoint();

        std::scoped_lock lk(g_mutex);
        const SystemClockContext &network = g_context[SlotNetwork];
        if (!Matches(network)) {
            return false;
        }
        const s64 age = now.time_point - network.timestamp.time_point;
        return age >= 0 && static_cast<u64>(age) < g_accuracy_minutes * 60;
    }

    void SetNetworkAccuracyMinutes(u64 minutes) {
        std::scoped_lock lk(g_mutex);
        g_accuracy_minutes = minutes;
    }

    Handle GetAutomaticCorrectionEventHandle() {
        return g_events[EventIndex_AutomaticCorrection].revent;
    }

    Handle GetAlarmEventHandle()      { return g_events[EventIndex_Alarm].revent; }
    Handle GetLocationEventHandle()   { return g_events[EventIndex_Location].revent; }
    Handle GetPowerStateEventHandle() { return g_events[EventIndex_PowerState].revent; }

    /* ===== Shared memory ===== */

    bool IsSharedMemoryAvailable() {
        return g_shared != nullptr;
    }

    Handle GetSharedMemoryHandle() {
        return g_shared_memory.handle;
    }

    /* ===== Time zone ===== */

    const TimeZoneRuleLayout &GetUtcRule() {
        return g_utc_rule;
    }

    LocationName GetLocationName() {
        std::scoped_lock lk(g_mutex);
        return g_location;
    }

    Result SetLocationName(const LocationName &name) {
        /* Only UTC exists. */
        KUMO_UNLESS(::strnlen(name.name, sizeof(name.name)) == 3 && std::memcmp(name.name, "UTC", 3) == 0, ResultTimeZoneNotFound());

        std::scoped_lock lk(g_mutex);
        KUMO_ABORT_RESULT(eventFire(&g_events[EventIndex_Location]));
        KUMO_SUCCEED();
    }

    TimeZoneRuleVersion GetTimeZoneRuleVersion() {
        /* The rule never changes: a fixed string. */
        TimeZoneRuleVersion version = {};
        std::memcpy(version.data, "UTC-only-v1", 11);
        return version;
    }

    /* ===== Snapshots ===== */

    Result GetClockSnapshot(ClockSnapshot *out, u8 type) {
        SystemClockContext user, network;
        {
            std::scoped_lock lk(g_mutex);
            user    = g_context[SlotLocalUser];
            network = g_context[SlotNetwork];
        }
        return GetClockSnapshotFromContexts(out, type, user, network);
    }

    Result GetClockSnapshotFromContexts(ClockSnapshot *out, u8 type, const SystemClockContext &user_context, const SystemClockContext &network_context) {
        const SteadyClockTimePoint now = SteadyTimePoint();
        KUMO_UNLESS(Matches(user_context), ResultClockMismatch());

        std::memset(out, 0, sizeof(*out));
        out->user_context   = user_context;
        out->network_context = network_context;
        out->user_time      = ClockTime(user_context, now);
        FillCalendar(std::addressof(out->user_calendar_time), std::addressof(out->user_calendar_additional_info), out->user_time);

        /* The network clock may never have been set; then its time and calendar are zero. */
        if (Matches(network_context)) {
            out->network_time = ClockTime(network_context, now);
            FillCalendar(std::addressof(out->network_calendar_time), std::addressof(out->network_calendar_additional_info), out->network_time);
        }

        out->steady_clock_time_point = now;
        out->location_name = GetLocationName();
        out->is_automatic_correction_enabled = IsAutomaticCorrectionEnabled();
        out->type = type;
        KUMO_SUCCEED();
    }

}
