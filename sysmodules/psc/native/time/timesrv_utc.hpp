/*
 * Calendar arithmetic and time zone rules. There is no TimeZoneBinary archive
 * on this OS, so the only zone is UTC, and the rule for it is built here. The
 * conversions still read a rule the caller supplies (ToCalendarTime and
 * ToPosixTime take one), but only its transitions and types: there is no POSIX
 * TZ footer, and no leap seconds, as on the Switch.
 */
#pragma once
#include "timesrv_types.hpp"

namespace kumo::horizon::timesrv::srv {

    /* Days since 1970-01-01 of a proleptic Gregorian date, and the reverse. */
    s64 DaysFromCivil(s64 year, int month, int day);
    void CivilFromDays(s64 days, s64 *out_year, int *out_month, int *out_day);

    bool IsValidCalendarTime(const CalendarTime &calendar);

    /* Fills a 0x4000-byte buffer with the rule for UTC. */
    void BuildUtcRule(TimeZoneRuleLayout *rule);

    /* A rule a caller passed in must be self-consistent before it is used. */
    Result ValidateRule(const TimeZoneRuleLayout &rule);

    Result ToCalendarTime(CalendarTime *out_time, CalendarAdditionalInfo *out_info, s64 posix_time, const TimeZoneRuleLayout &rule);

    /* The instants (up to `capacity`, in increasing order) at which the local time is `calendar`: none or two around a change of offset, one otherwise. */
    Result ToPosixTimes(s32 *out_count, s64 *out, size_t capacity, const CalendarTime &calendar, const TimeZoneRuleLayout &rule);

}
