#include <kumo/prelude.hpp>
#include "timesrv_utc.hpp"

namespace kumo::horizon::timesrv::srv {

    namespace {

        constexpr s64 SecondsPerDay = 86400;

        s64 FloorDiv(s64 a, s64 b) {
            s64 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) {
                --q;
            }
            return q;
        }

        bool IsLeap(s64 year) {
            return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
        }

        int DaysInMonth(s64 year, int month) {
            static constexpr int Days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            return (month == 2 && IsLeap(year)) ? 29 : Days[month - 1];
        }

        /* The ttinfo in force at `posix_time`. */
        const TimeTypeInfo &FindType(const TimeZoneRuleLayout &rule, s64 posix_time) {
            int type = rule.default_type;
            if (rule.time_count > 0 && posix_time >= rule.transition_times[0]) {
                /* The last transition at or before the time. */
                int lo = 0, hi = rule.time_count - 1;
                while (lo < hi) {
                    const int mid = (lo + hi + 1) / 2;
                    if (rule.transition_times[mid] <= posix_time) {
                        lo = mid;
                    } else {
                        hi = mid - 1;
                    }
                }
                type = rule.transition_types[lo];
            }
            return rule.types[type];
        }

    }

    s64 DaysFromCivil(s64 year, int month, int day) {
        /* Howard Hinnant's days_from_civil (public domain). */
        year -= (month <= 2) ? 1 : 0;
        const s64 era = FloorDiv(year, 400);
        const s64 yoe = year - era * 400;
        const s64 doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    void CivilFromDays(s64 days, s64 *out_year, int *out_month, int *out_day) {
        /* Howard Hinnant's civil_from_days (public domain). */
        days += 719468;
        const s64 era = FloorDiv(days, 146097);
        const s64 doe = days - era * 146097;
        const s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const s64 mp  = (5 * doy + 2) / 153;
        const int day   = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
        *out_year  = yoe + era * 400 + (month <= 2 ? 1 : 0);
        *out_month = month;
        *out_day   = day;
    }

    bool IsValidCalendarTime(const CalendarTime &calendar) {
        if (calendar.year < 1 || calendar.month < 1 || calendar.month > 12) {
            return false;
        }
        if (calendar.day < 1 || calendar.day > DaysInMonth(calendar.year, calendar.month)) {
            return false;
        }
        return calendar.hour < 24 && calendar.minute < 60 && calendar.second < 60;
    }

    void BuildUtcRule(TimeZoneRuleLayout *rule) {
        std::memset(rule, 0, sizeof(*rule));
        rule->time_count   = 0;
        rule->type_count   = 1;
        rule->char_count   = 4;
        rule->default_type = 0;
        rule->types[0].gmt_offset         = 0;
        rule->types[0].is_dst             = 0;
        rule->types[0].abbreviation_index = 0;
        std::memcpy(rule->chars, "UTC", 4);
    }

    Result ValidateRule(const TimeZoneRuleLayout &rule) {
        KUMO_UNLESS(rule.time_count >= 0 && rule.time_count <= MaxTimes, ResultInvalidTimeZoneRule());
        KUMO_UNLESS(rule.type_count >= 1 && rule.type_count <= MaxTypes, ResultInvalidTimeZoneRule());
        KUMO_UNLESS(rule.char_count >= 1 && rule.char_count <= MaxChars, ResultInvalidTimeZoneRule());
        KUMO_UNLESS(rule.default_type >= 0 && rule.default_type < rule.type_count, ResultInvalidTimeZoneRule());

        for (int i = 0; i < rule.time_count; ++i) {
            KUMO_UNLESS(rule.transition_types[i] < rule.type_count, ResultInvalidTimeZoneRule());
            KUMO_UNLESS(i == 0 || rule.transition_times[i - 1] <= rule.transition_times[i], ResultInvalidTimeZoneRule());
        }
        for (int i = 0; i < rule.type_count; ++i) {
            KUMO_UNLESS(rule.types[i].abbreviation_index >= 0 && rule.types[i].abbreviation_index < rule.char_count, ResultInvalidTimeZoneRule());
        }
        KUMO_SUCCEED();
    }

    Result ToCalendarTime(CalendarTime *out_time, CalendarAdditionalInfo *out_info, s64 posix_time, const TimeZoneRuleLayout &rule) {
        KUMO_TRY(ValidateRule(rule));

        const TimeTypeInfo &type = FindType(rule, posix_time);
        const s64 local = posix_time + type.gmt_offset;

        const s64 days = FloorDiv(local, SecondsPerDay);
        const s64 secs = local - days * SecondsPerDay;

        s64 year;
        int month, day;
        CivilFromDays(days, std::addressof(year), std::addressof(month), std::addressof(day));
        KUMO_UNLESS(year >= 1 && year <= 0xFFFF, MAKERESULT(116, 201));

        out_time->year   = static_cast<u16>(year);
        out_time->month  = static_cast<u8>(month);
        out_time->day    = static_cast<u8>(day);
        out_time->hour   = static_cast<u8>(secs / 3600);
        out_time->minute = static_cast<u8>((secs / 60) % 60);
        out_time->second = static_cast<u8>(secs % 60);
        out_time->pad    = 0;

        std::memset(out_info, 0, sizeof(*out_info));
        out_info->wday = static_cast<u32>(((days % 7) + 7 + 4) % 7);   /* 1970-01-01 was a Thursday */
        out_info->yday = static_cast<u32>(days - DaysFromCivil(year, 1, 1));
        out_info->dst  = type.is_dst ? 1 : 0;
        out_info->offset = type.gmt_offset;

        /* The abbreviation, at most 7 characters and always terminated. */
        const char *abbreviation = rule.chars + type.abbreviation_index;
        const int available = rule.char_count - type.abbreviation_index;
        for (int i = 0; i < 7 && i < available && abbreviation[i] != '\0'; ++i) {
            out_info->timezone_name[i] = abbreviation[i];
        }
        KUMO_SUCCEED();
    }

    Result ToPosixTimes(s32 *out_count, s64 *out, size_t capacity, const CalendarTime &calendar, const TimeZoneRuleLayout &rule) {
        KUMO_TRY(ValidateRule(rule));
        KUMO_UNLESS(IsValidCalendarTime(calendar), ResultInvalidCalendarTime());

        const s64 local = DaysFromCivil(calendar.year, calendar.month, calendar.day) * SecondsPerDay + calendar.hour * 3600 + calendar.minute * 60 + calendar.second;

        /* Every offset the rule uses is a candidate; it holds if the rule agrees that it is in force at the instant it gives. */
        s64 found[2];
        s32 count = 0;
        for (int i = 0; i < rule.type_count && count < 2; ++i) {
            const s64 candidate = local - rule.types[i].gmt_offset;
            if (FindType(rule, candidate).gmt_offset != rule.types[i].gmt_offset) {
                continue;
            }
            bool seen = false;
            for (s32 j = 0; j < count; ++j) {
                seen = seen || found[j] == candidate;
            }
            if (!seen) {
                found[count++] = candidate;
            }
        }

        /* In a gap (the local time was skipped over), the instant the default offset gives. */
        if (count == 0) {
            found[count++] = local - rule.types[rule.default_type].gmt_offset;
        }
        if (count == 2 && found[1] < found[0]) {
            std::swap(found[0], found[1]);
        }

        s32 written = 0;
        for (s32 i = 0; i < count && static_cast<size_t>(i) < capacity; ++i) {
            out[written++] = found[i];
        }
        *out_count = written;
        KUMO_SUCCEED();
    }

}
