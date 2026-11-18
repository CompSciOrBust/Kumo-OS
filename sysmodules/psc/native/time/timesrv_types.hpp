/*
 * Types of Horizon's time services (nn::time), shared by psc (which hosts the
 * time core: time:m, time:s, time:su, time:al, time:p) and glue (time:u,
 * time:a, time:r). See docs/time.md.
 *
 * Layouts are from https://switchbrew.org/wiki/Glue_services and
 * https://switchbrew.org/wiki/PSC_services (read 2026-11-13) and from libnx's
 * time.h. ClockSnapshot is not on the wiki: its layout (0xD0 bytes) is from
 * memory of emulator implementations, and nothing on this OS reads it but the
 * test. Everything marked [?] in docs/time.md is unverified.
 */
#pragma once
#include <kumo/prelude.hpp>

namespace kumo::horizon::timesrv {

    struct Uuid {
        u8 data[16];
        bool operator==(const Uuid &other) const { return std::memcmp(data, other.data, sizeof(data)) == 0; }
    };

    /* libnx's TimeType, which is also what GetClockSnapshot's type argument is. */
    enum TimeType : u8 {
        TimeType_UserSystemClock    = 0,
        TimeType_NetworkSystemClock = 1,
        TimeType_LocalSystemClock   = 2,
    };

    struct SteadyClockTimePoint {
        s64 time_point;         /* seconds */
        Uuid source_id;
    };
    static_assert(sizeof(SteadyClockTimePoint) == 0x18);

    struct SystemClockContext {
        s64 offset;             /* seconds; the clock reads offset + steady clock */
        SteadyClockTimePoint timestamp;
    };
    static_assert(sizeof(SystemClockContext) == 0x20);

    struct CalendarTime {
        u16 year;
        u8 month;
        u8 day;
        u8 hour;
        u8 minute;
        u8 second;
        u8 pad;
    };
    static_assert(sizeof(CalendarTime) == 8);

    struct CalendarAdditionalInfo {
        u32 wday;               /* 0 = Sunday */
        u32 yday;               /* 0-based */
        char timezone_name[8];
        u32 dst;
        s32 offset;             /* seconds east of UTC */
    };
    static_assert(sizeof(CalendarAdditionalInfo) == 0x18);

    struct LocationName {
        char name[0x24];
    };
    static_assert(sizeof(LocationName) == 0x24);

    struct TimeZoneRuleVersion {
        u8 data[0x10];
    };
    static_assert(sizeof(TimeZoneRuleVersion) == 0x10);

    /* Large structures travel as fixed-size pointer buffers (types 0x19 and 0x1A). */
    struct ClockSnapshot {
        SystemClockContext user_context;                        /* 0x00 */
        SystemClockContext network_context;                     /* 0x20 */
        s64 user_time;                                          /* 0x40 */
        s64 network_time;                                       /* 0x48 */
        CalendarTime user_calendar_time;                        /* 0x50 */
        CalendarTime network_calendar_time;                     /* 0x58 */
        CalendarAdditionalInfo user_calendar_additional_info;   /* 0x60 */
        CalendarAdditionalInfo network_calendar_additional_info;/* 0x78 */
        SteadyClockTimePoint steady_clock_time_point;           /* 0x90 */
        LocationName location_name;                             /* 0xA8 */
        bool is_automatic_correction_enabled;                   /* 0xCC */
        u8 type;                                                /* 0xCD */
        u8 pad[2];                                              /* 0xCE */
    };
    static_assert(sizeof(ClockSnapshot) == 0xD0);

    /* The 0x4000-byte TimeZoneRule, the zoneinfo "state" of the tz library. Offsets are the wiki's. */
    constexpr size_t TimeZoneRuleSize = 0x4000;
    constexpr int MaxTimes = 1000;
    constexpr int MaxTypes = 128;
    constexpr int MaxChars = 512;

    struct TimeTypeInfo {
        s32 gmt_offset;
        u8 is_dst;
        u8 pad0[3];
        s32 abbreviation_index;
        u8 is_std;
        u8 is_gmt;
        u8 pad1[2];
    };
    static_assert(sizeof(TimeTypeInfo) == 0x10);

    struct TimeZoneRuleLayout {
        s32 time_count;
        s32 type_count;
        s32 char_count;
        u8 go_back;
        u8 go_ahead;
        u8 pad0[2];
        s64 transition_times[MaxTimes];
        u8 transition_types[MaxTimes];
        TimeTypeInfo types[MaxTypes];
        char chars[MaxChars];
        s32 default_type;
        u8 reserved[TimeZoneRuleSize - 0x2D3C];
    };
    static_assert(sizeof(TimeZoneRuleLayout) == TimeZoneRuleSize);
    static_assert(offsetof(TimeZoneRuleLayout, transition_times) == 0x10);
    static_assert(offsetof(TimeZoneRuleLayout, transition_types) == 0x1F50);
    static_assert(offsetof(TimeZoneRuleLayout, types) == 0x2338);
    static_assert(offsetof(TimeZoneRuleLayout, chars) == 0x2B38);
    static_assert(offsetof(TimeZoneRuleLayout, default_type) == 0x2D38);

    /* The shared memory page libnx maps (command 20 of IStaticService). Reader protocol: libnx time.c. */
    constexpr size_t SharedMemorySize = 0x1000;

    struct SteadyClockSharedRecord {
        s64 base_time;          /* nanoseconds: base_time + armTicksToNs(tick) is the steady clock */
        Uuid source_id;
    };
    static_assert(sizeof(SteadyClockSharedRecord) == 0x18);

    template<typename T>
    struct SharedLockFree {
        u32 counter;            /* the slot to read is counter & 1 */
        u32 pad;
        T slots[2];
    };

    struct SharedMemoryLayout {
        SharedLockFree<SteadyClockSharedRecord> steady;     /* 0x00 .. 0x38 */
        SharedLockFree<SystemClockContext> local;           /* 0x38 .. 0x80 (libnx reads the user and local clocks here) */
        SharedLockFree<SystemClockContext> network;         /* 0x80 .. 0xC8 */
        u8 automatic_correction_enabled;                    /* 0xC8 (not read by libnx; placement [?]) */
    };
    static_assert(offsetof(SharedMemoryLayout, steady)  == 0x00);
    static_assert(offsetof(SharedMemoryLayout, local)   == 0x38);
    static_assert(offsetof(SharedMemoryLayout, network) == 0x80);
    static_assert(offsetof(SharedMemoryLayout, automatic_correction_enabled) == 0xC8);
    static_assert(sizeof(SharedMemoryLayout) <= SharedMemorySize);

}

namespace kumo::horizon::timesrv {
    constexpr Result ResultClockMismatch() { return MAKERESULT(116, 102); }
    constexpr Result ResultNotSupported() { return MAKERESULT(116, 990); }
    constexpr Result ResultPermissionDenied() { return MAKERESULT(116, 1001); }
    constexpr Result ResultInvalidCalendarTime() { return MAKERESULT(116, 1002); }
    constexpr Result ResultInvalidTimeZoneRule() { return MAKERESULT(116, 1003); }
    constexpr Result ResultTimeZoneNotFound() { return MAKERESULT(116, 1004); }
    constexpr Result ResultNotConnected() { return MAKERESULT(116, 1005); }
}
