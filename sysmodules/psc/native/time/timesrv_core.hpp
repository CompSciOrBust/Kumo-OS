/*
 * The time core: the standard steady clock, the system clocks (local, user,
 * network, ephemeral network), the settings stand-ins, and the shared memory
 * page libnx reads. Nothing in here speaks IPC; the services in
 * timesrv_services.cpp and timesrv_manager.cpp call it, and so does the SNTP
 * client (timesrv_sntp.cpp). See docs/time.md for what each piece stands in for.
 *
 * Every function is safe to call from any thread.
 */
#pragma once
#include "timesrv_types.hpp"

namespace kumo::horizon::timesrv::srv {

    enum ClockId {
        ClockId_User,
        ClockId_Network,
        ClockId_Local,
        ClockId_Ephemeral,
    };

    /* What a static service's session may change: time:u and time:r nothing, time:su the user clock, time:a the user and local clocks and the zone, time:s everything. */
    enum Permission : u32 {
        Permission_None         = 0,
        Permission_LocalClock   = (1u << 0),
        Permission_UserClock    = (1u << 1),
        Permission_NetworkClock = (1u << 2),    /* the network and ephemeral network clocks */
        Permission_TimeZone     = (1u << 3),
        Permission_SteadyClock  = (1u << 4),

        Permission_User         = Permission_None,
        Permission_Repair       = Permission_None,
        Permission_SystemUser   = Permission_UserClock,
        Permission_Admin        = Permission_UserClock | Permission_LocalClock | Permission_TimeZone,
        Permission_System       = Permission_UserClock | Permission_LocalClock | Permission_NetworkClock | Permission_TimeZone | Permission_SteadyClock,
        Permission_Manager      = Permission_NetworkClock,
    };

    /* The settings stand-ins (there is no settings service). */
    constexpr s64 DefaultWallClockSeconds        = 1767225600;     /* 2026-01-01T00:00:00Z: what the clocks read at boot until something sets them */
    constexpr u64 DefaultSufficientAccuracyMinutes = 43200;        /* 30 days [?] */
    constexpr s32 DefaultUserClockInitialYear    = 2000;
    constexpr bool DefaultAutomaticCorrection    = true;

    /* Creates the shared memory page, the steady clock's source ID and the events, and starts the clocks. */
    void InitializeCore();

    /* ===== Steady clock ===== */
    s64 GetSteadyNanoSeconds();
    SteadyClockTimePoint GetSteadyTimePoint();
    const Uuid &GetSteadySourceId();
    s64 GetSteadyClockInternalOffset();
    s64 GetSteadyClockTestOffset();
    s64 GetSteadyClockRtcOffset();
    bool IsRtcResetDetected();
    void SetSteadyClockInternalOffset(s64 offset);
    void SetSteadyClockTestOffset(s64 offset);
    void SetSteadyClockBaseTime(s64 rtc_offset);
    void SetupSteadyClock(s64 rtc_offset, s64 internal_offset, s64 test_offset, bool is_rtc_reset_detected);

    /* ===== System clocks ===== */
    /* The user and local clocks are one context here (libnx reads them from the same slot). */
    Result GetCurrentTime(ClockId clock, s64 *out);
    Result SetCurrentTime(ClockId clock, s64 posix_time);
    SystemClockContext GetClockContext(ClockId clock);
    void SetClockContext(ClockId clock, const SystemClockContext &context);
    /* Whether a clock's context belongs to the steady clock that is running. */
    bool IsClockInitialized(ClockId clock);
    Handle GetClockEventHandle(ClockId clock);

    /* Applies a time from the network (the SNTP client): sets the network clock, and the user and local clocks if automatic correction is on. */
    void UpdateNetworkClock(s64 posix_time, const SteadyClockTimePoint &steady_time_point);

    bool IsAutomaticCorrectionEnabled();
    void SetAutomaticCorrectionEnabled(bool enabled);
    SteadyClockTimePoint GetAutomaticCorrectionUpdatedTime();
    bool IsNetworkClockAccuracySufficient();
    void SetNetworkAccuracyMinutes(u64 minutes);
    Handle GetAutomaticCorrectionEventHandle();

    /* Events that are never signalled (alarms, time zone changes, power state requests). */
    Handle GetAlarmEventHandle();
    Handle GetLocationEventHandle();
    Handle GetPowerStateEventHandle();

    /* ===== Shared memory ===== */
    bool IsSharedMemoryAvailable();
    Handle GetSharedMemoryHandle();

    /* ===== Time zone ===== */
    const TimeZoneRuleLayout &GetUtcRule();
    LocationName GetLocationName();
    Result SetLocationName(const LocationName &name);
    TimeZoneRuleVersion GetTimeZoneRuleVersion();

    /* ===== Snapshots ===== */
    Result GetClockSnapshot(ClockSnapshot *out, u8 type);
    Result GetClockSnapshotFromContexts(ClockSnapshot *out, u8 type, const SystemClockContext &user_context, const SystemClockContext &network_context);

}
