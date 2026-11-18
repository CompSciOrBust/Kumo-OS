#include "timesrv_ipc.hpp"
#include "timesrv_core.hpp"
#include "timesrv_utc.hpp"
#include <kumo/ipc/dispatch.hpp>
#include <kumo/ipc/object_server.hpp>

namespace kumo::horizon::timesrv::srv {
namespace {
using namespace kumo::ipc;
template<size_t N, size_t A = 1> struct alignas(A) Wire {
    static constexpr size_t WireBytes = N;
    u8 bytes[N];
};
using Context = Wire<0x20, 8>;
using Point = Wire<0x18, 8>;
using Location = Wire<0x24>;
using Version = Wire<0x10>;
using Calendar = Wire<8, 2>;
using CalendarOutput = Wire<0x20, 4>;
using SnapshotInput = Wire<0x48, 8>;
using SetupSteady = Wire<0x30, 8>;
using SetupClock = Wire<0x28, 8>;
using SetupUser = Wire<0x20, 8>;
using SetupZone = Wire<0x50, 8>;
struct CalendarResult { CalendarTime time; CalendarAdditionalInfo info; };
static_assert(sizeof(CalendarResult) == 0x20);
template<class D, class S> D WireCopy(const S &s) {
    static_assert(sizeof(D) == sizeof(S));
    D d{}; std::memcpy(&d, &s, sizeof(d)); return d;
}
Result CheckPermission(u32 have, u32 need) {
    return (have & need) == need ? 0 : ResultPermissionDenied();
}
class Clock final : public ServiceObject {
public:
    ClockId id;
    u32 permission;
    Clock(ClockId id_, u32 permission_) : id(id_), permission(permission_) {}
    Result Writable() const {
        const u32 need = id == ClockId_User ? Permission_UserClock : id == ClockId_Local ? Permission_LocalClock : Permission_NetworkClock;
        return CheckPermission(permission, need);
    }
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Steady final : public ServiceObject {
public:
    u32 permission;
    explicit Steady(u32 p) : permission(p) {}
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Manager final : public ServiceObject {
public:
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Alarm final : public ServiceObject {
public:
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Power final : public ServiceObject {
public:
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Zone final : public ServiceObject {
public:
    u32 permission;
    explicit Zone(u32 p) : permission(p) {}
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Static final : public ServiceObject {
public:
    u32 permission;
    explicit Static(u32 p) : permission(p) {}
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
#define CMD(S,I,O,NI,NO,H,ID,...) MakeCommand<S,I,O,NI,NO,H>(ID __VA_OPT__(,) __VA_ARGS__)
bool Clock::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Clock, NoData, s64, 0, 0, ([](Clock &s, const NoData &, s64 &out, Arguments<0,0> &) -> Result { return GetCurrentTime(s.id, &out); }), 0),
        CMD(Clock, s64, NoData, 0, 0, ([](Clock &s, const s64 &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(s.Writable()); return SetCurrentTime(s.id, in); }), 1),
        CMD(Clock, NoData, Context, 0, 0, ([](Clock &s, const NoData &, Context &out, Arguments<0,0> &) -> Result { out = WireCopy<Context>(GetClockContext(s.id)); return 0; }), 2),
        CMD(Clock, Context, NoData, 0, 0, ([](Clock &s, const Context &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(s.Writable()); SetClockContext(s.id, WireCopy<SystemClockContext>(in)); return 0; }), 3),
        CMD(Clock, NoData, NoData, 0, 0, ([](Clock &s, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(s.id)}); return 0; }), 4)
    });
    return commands.Dispatch(this, r, reply);
}
bool Steady::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Steady, NoData, Point, 0, 0, ([](Steady &, const NoData &, Point &out, Arguments<0,0> &) -> Result { out = WireCopy<Point>(GetSteadyTimePoint()); return 0; }), 0),
        CMD(Steady, NoData, s64, 0, 0, ([](Steady &, const NoData &, s64 &out, Arguments<0,0> &) -> Result { out = GetSteadyClockTestOffset(); return 0; }), 2),
        CMD(Steady, s64, NoData, 0, 0, ([](Steady &s, const s64 &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(CheckPermission(s.permission, Permission_SteadyClock)); SetSteadyClockTestOffset(in); return 0; }), 3),
        CMD(Steady, NoData, s64, 0, 0, ([](Steady &, const NoData &, s64 &out, Arguments<0,0> &) -> Result { out = GetSteadyClockRtcOffset() + GetSteadyTimePoint().time_point; return 0; }), 100),
        CMD(Steady, NoData, u8, 0, 0, ([](Steady &, const NoData &, u8 &out, Arguments<0,0> &) -> Result { out = IsRtcResetDetected(); return 0; }), 101),
        CMD(Steady, NoData, u32, 0, 0, ([](Steady &, const NoData &, u32 &out, Arguments<0,0> &) -> Result { out = 0; return 0; }), 102),
        CMD(Steady, NoData, s64, 0, 0, ([](Steady &, const NoData &, s64 &out, Arguments<0,0> &) -> Result { out = GetSteadyClockInternalOffset(); return 0; }), 200),
        CMD(Steady, s64, NoData, 0, 0, ([](Steady &, const s64 &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 201)
    });
    return commands.Dispatch(this, r, reply);
}
template<u32 P> Result OpenStatic(Manager &, const NoData &, NoData &, Arguments<0,0> &a) {
    auto obj = MakeObject<Static>(P); if (!obj) return ResultNotConnected(); a.ReturnObject(std::move(obj)); return 0;
}
bool Manager::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Manager, NoData, NoData, 0, 0, (OpenStatic<Permission_User>), 0),
        CMD(Manager, NoData, NoData, 0, 0, (OpenStatic<Permission_Admin>), 5),
        CMD(Manager, NoData, NoData, 0, 0, (OpenStatic<Permission_Repair>), 6),
        CMD(Manager, NoData, NoData, 0, 0, (OpenStatic<Permission_Manager>), 9),
        CMD(Manager, SetupSteady, NoData, 0, 0, ([](Manager &, const SetupSteady &in, NoData &, Arguments<0,0> &) -> Result { s64 offsets[3]; std::memcpy(offsets, in.bytes + 24, sizeof(offsets)); SetupSteadyClock(offsets[0], offsets[1], offsets[2], in.bytes[16] != 0); return 0; }), 10),
        CMD(Manager, SetupClock, NoData, 0, 0, ([](Manager &, const SetupClock &in, NoData &, Arguments<0,0> &) -> Result { SystemClockContext c{}; std::memcpy(&c, in.bytes, sizeof(c)); if (c.timestamp.source_id == GetSteadySourceId()) SetClockContext(ClockId_Local, c); return 0; }), 11),
        CMD(Manager, SetupClock, NoData, 0, 0, ([](Manager &, const SetupClock &in, NoData &, Arguments<0,0> &) -> Result { SystemClockContext c{}; u64 accuracy; std::memcpy(&c, in.bytes, sizeof(c)); std::memcpy(&accuracy, in.bytes + 32, sizeof(accuracy)); SetNetworkAccuracyMinutes(accuracy); if (c.timestamp.source_id == GetSteadySourceId()) SetClockContext(ClockId_Network, c); return 0; }), 12),
        CMD(Manager, SetupUser, NoData, 0, 0, ([](Manager &, const SetupUser &in, NoData &, Arguments<0,0> &) -> Result { SetAutomaticCorrectionEnabled(in.bytes[0] != 0); return 0; }), 13),
        CMD(Manager, SetupZone, NoData, 1, 0, ([](Manager &, const SetupZone &in, NoData &, Arguments<1,0> &) -> Result { LocationName name{}; std::memcpy(&name, in.bytes, sizeof(name)); return SetLocationName(name); }), 14),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return 0; }), 15),
        CMD(Manager, NoData, Point, 0, 0, ([](Manager &, const NoData &, Point &out, Arguments<0,0> &) -> Result { out = WireCopy<Point>(GetSteadyTimePoint()); return 0; }), 20),
        CMD(Manager, NoData, s64, 0, 0, ([](Manager &, const NoData &, s64 &out, Arguments<0,0> &) -> Result { return GetCurrentTime(ClockId_Network, &out); }), 21),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(ClockId_Local)}); return 0; }), 50),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(ClockId_Network)}); return 0; }), 51),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(ClockId_Ephemeral)}); return 0; }), 52),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetAutomaticCorrectionEventHandle()}); return 0; }), 60),
        CMD(Manager, s64, NoData, 0, 0, ([](Manager &, const s64 &in, NoData &, Arguments<0,0> &) -> Result { SetSteadyClockBaseTime(in); return 0; }), 100),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetAlarmEventHandle()}); return 0; }), 200),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return 0; }), 201),
        CMD(Manager, NoData, NoData, 0, 0, ([](Manager &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 202)
    }); return commands.Dispatch(this, r, reply);
}
bool Alarm::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Alarm, NoData, NoData, 0, 0, ([](Alarm &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 0),
        CMD(Alarm, NoData, NoData, 0, 0, ([](Alarm &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 1)
    }); return commands.Dispatch(this, r, reply);
}
bool Power::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Power, NoData, NoData, 0, 0, ([](Power &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetPowerStateEventHandle()}); return 0; }), 0),
        CMD(Power, NoData, NoData, 0, 0, ([](Power &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 1)
    }); return commands.Dispatch(this, r, reply);
}
bool Zone::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Zone, NoData, Location, 0, 0, ([](Zone &, const NoData &, Location &out, Arguments<0,0> &) -> Result { out = WireCopy<Location>(GetLocationName()); return 0; }), 0),
        CMD(Zone, Location, NoData, 0, 0, ([](Zone &s, const Location &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(CheckPermission(s.permission, Permission_TimeZone)); return SetLocationName(WireCopy<LocationName>(in)); }), 1),
        CMD(Zone, NoData, s32, 0, 0, ([](Zone &, const NoData &, s32 &out, Arguments<0,0> &) -> Result { out = 1; return 0; }), 2),
        CMD(Zone, s32, s32, 0, 1, ([](Zone &, const s32 &index, s32 &out, Arguments<0,1> &a) -> Result { if (index < 0) return ResultTimeZoneNotFound(); out = 0; if (index == 0 && a.Out(0).GetSize() >= sizeof(LocationName)) { const auto name = GetLocationName(); std::memcpy(a.Out(0).GetPointer(), &name, sizeof(name)); out = 1; } return 0; }), 3, CommandOptions{false,0,0,BufferMode::Mapped}),
        CMD(Zone, Location, NoData, 0, 1, ([](Zone &, const Location &in, NoData &, Arguments<0,1> &a) -> Result { const auto name = WireCopy<LocationName>(in); if (a.Out(0).GetSize() < TimeZoneRuleSize) return ResultInvalidTimeZoneRule(); if (strnlen(name.name, sizeof(name.name)) != 3 || std::memcmp(name.name, "UTC", 3)) return ResultTimeZoneNotFound(); std::memcpy(a.Out(0).GetPointer(), &GetUtcRule(), TimeZoneRuleSize); return 0; }), 4, CommandOptions{false,0,0,BufferMode::Mapped}),
        CMD(Zone, NoData, Version, 0, 0, ([](Zone &, const NoData &, Version &out, Arguments<0,0> &) -> Result { out = WireCopy<Version>(GetTimeZoneRuleVersion()); return 0; }), 5),
        CMD(Zone, NoData, NoData, 0, 0, ([](Zone &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 6),
        CMD(Zone, NoData, NoData, 0, 0, ([](Zone &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 7),
        CMD(Zone, NoData, NoData, 1, 1, ([](Zone &, const NoData &, NoData &, Arguments<1,1> &) -> Result { return ResultNotSupported(); }), 8, CommandOptions{false,0,0,BufferMode::Mapped}),
        CMD(Zone, NoData, NoData, 0, 0, ([](Zone &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetLocationEventHandle()}); return 0; }), 20),
        CMD(Zone, s64, CalendarOutput, 1, 0, ([](Zone &, const s64 &in, CalendarOutput &out, Arguments<1,0> &a) -> Result { if (a.In(0).GetSize() < sizeof(TimeZoneRuleLayout)) return ResultInvalidTimeZoneRule(); TimeZoneRuleLayout rule; std::memcpy(&rule, a.In(0).GetPointer(), sizeof(rule)); CalendarResult value{}; KUMO_TRY(ToCalendarTime(&value.time, &value.info, in, rule)); out = WireCopy<CalendarOutput>(value); return 0; }), 100, CommandOptions{false,0,0,BufferMode::Mapped}),
        CMD(Zone, s64, CalendarOutput, 0, 0, ([](Zone &, const s64 &in, CalendarOutput &out, Arguments<0,0> &) -> Result { CalendarResult value{}; KUMO_TRY(ToCalendarTime(&value.time, &value.info, in, GetUtcRule())); out = WireCopy<CalendarOutput>(value); return 0; }), 101),
        CMD(Zone, Calendar, s32, 1, 1, ([](Zone &, const Calendar &in, s32 &out, Arguments<1,1> &a) -> Result { if (a.In(0).GetSize() < sizeof(TimeZoneRuleLayout)) return ResultInvalidTimeZoneRule(); TimeZoneRuleLayout rule; std::memcpy(&rule, a.In(0).GetPointer(), sizeof(rule)); return ToPosixTimes(&out, reinterpret_cast<s64 *>(a.Out(0).GetPointer()), a.Out(0).GetSize() / sizeof(s64), WireCopy<CalendarTime>(in), rule); }), 201, CommandOptions{false,0,0,BufferMode::PointerOutput}),
        CMD(Zone, Calendar, s32, 0, 1, ([](Zone &, const Calendar &in, s32 &out, Arguments<0,1> &a) -> Result { return ToPosixTimes(&out, reinterpret_cast<s64 *>(a.Out(0).GetPointer()), a.Out(0).GetSize() / sizeof(s64), WireCopy<CalendarTime>(in), GetUtcRule()); }), 202, CommandOptions{false,0,0,BufferMode::PointerOutput})
    });
    return commands.Dispatch(this, r, reply);
}
Result OpenZone(Static &s, const NoData &, NoData &, Arguments<0,0> &a) {
    auto obj = MakeObject<Zone>(s.permission); if (!obj) return ResultNotConnected(); a.ReturnObject(std::move(obj)); return 0;
}
template<ClockId Id> Result OpenClock(Static &s, const NoData &, NoData &, Arguments<0,0> &a) {
    auto obj = MakeObject<Clock>(Id, s.permission); if (!obj) return ResultNotConnected(); a.ReturnObject(std::move(obj)); return 0;
}
Result OpenSteady(Static &s, const NoData &, NoData &, Arguments<0,0> &a) {
    auto obj = MakeObject<Steady>(s.permission); if (!obj) return ResultNotConnected(); a.ReturnObject(std::move(obj)); return 0;
}
bool Static::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Static, NoData, NoData, 0, 0, (OpenClock<ClockId_User>), 0),
        CMD(Static, NoData, NoData, 0, 0, (OpenClock<ClockId_Network>), 1),
        CMD(Static, NoData, NoData, 0, 0, (OpenSteady), 2),
        CMD(Static, NoData, NoData, 0, 0, (OpenZone), 3),
        CMD(Static, NoData, NoData, 0, 0, (OpenClock<ClockId_Local>), 4),
        CMD(Static, NoData, NoData, 0, 0, (OpenClock<ClockId_Ephemeral>), 5),
        CMD(Static, NoData, NoData, 0, 0, ([](Static &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { if (!IsSharedMemoryAvailable()) return ResultNotSupported(); a.ReturnCopy({GetSharedMemoryHandle()}); return 0; }), 20),
        CMD(Static, NoData, NoData, 0, 0, ([](Static &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(ClockId_Network)}); return 0; }), 30),
        CMD(Static, NoData, NoData, 0, 0, ([](Static &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetClockEventHandle(ClockId_Ephemeral)}); return 0; }), 31),
        CMD(Static, s64, NoData, 0, 0, ([](Static &s, const s64 &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(CheckPermission(s.permission, Permission_SteadyClock)); SetSteadyClockInternalOffset(in); return 0; }), 50),
        CMD(Static, NoData, s64, 0, 0, ([](Static &, const NoData &, s64 &out, Arguments<0,0> &) -> Result { out = GetSteadyClockRtcOffset() + GetSteadyTimePoint().time_point; return 0; }), 51),
        CMD(Static, NoData, u8, 0, 0, ([](Static &, const NoData &, u8 &out, Arguments<0,0> &) -> Result { out = IsAutomaticCorrectionEnabled(); return 0; }), 100),
        CMD(Static, u8, NoData, 0, 0, ([](Static &s, const u8 &in, NoData &, Arguments<0,0> &) -> Result { KUMO_TRY(CheckPermission(s.permission, Permission_UserClock)); SetAutomaticCorrectionEnabled(in != 0); return 0; }), 101),
        CMD(Static, NoData, s32, 0, 0, ([](Static &, const NoData &, s32 &out, Arguments<0,0> &) -> Result { out = DefaultUserClockInitialYear; return 0; }), 102),
        CMD(Static, NoData, u8, 0, 0, ([](Static &, const NoData &, u8 &out, Arguments<0,0> &) -> Result { out = IsNetworkClockAccuracySufficient(); return 0; }), 200),
        CMD(Static, NoData, Point, 0, 0, ([](Static &, const NoData &, Point &out, Arguments<0,0> &) -> Result { out = WireCopy<Point>(GetAutomaticCorrectionUpdatedTime()); return 0; }), 201),
        CMD(Static, Context, s64, 0, 0, ([](Static &, const Context &in, s64 &out, Arguments<0,0> &) -> Result { const auto context = WireCopy<SystemClockContext>(in); const auto now = GetSteadyTimePoint(); if (!(context.timestamp.source_id == now.source_id)) return ResultClockMismatch(); out = context.offset + now.time_point - GetSteadyNanoSeconds() / 1000000000LL; return 0; }), 300),
        CMD(Static, u8, NoData, 0, 1, ([](Static &, const u8 &in, NoData &, Arguments<0,1> &a) -> Result { ClockSnapshot snapshot{}; KUMO_TRY(GetClockSnapshot(&snapshot, in)); std::memcpy(a.Out(0).GetPointer(), &snapshot, sizeof(snapshot)); return 0; }), 400, CommandOptions{false,0,0,BufferMode::FixedPointer,0,0xD0}),
        CMD(Static, SnapshotInput, NoData, 0, 1, ([](Static &, const SnapshotInput &in, NoData &, Arguments<0,1> &a) -> Result { SystemClockContext user{}, network{}; std::memcpy(&user, in.bytes + 8, sizeof(user)); std::memcpy(&network, in.bytes + 40, sizeof(network)); ClockSnapshot snapshot{}; KUMO_TRY(GetClockSnapshotFromContexts(&snapshot, in.bytes[0], user, network)); std::memcpy(a.Out(0).GetPointer(), &snapshot, sizeof(snapshot)); return 0; }), 401, CommandOptions{false,0,0,BufferMode::FixedPointer,0,0xD0}),
        CMD(Static, NoData, s64, 2, 0, ([](Static &, const NoData &, s64 &out, Arguments<2,0> &args) -> Result { ClockSnapshot a{}, b{}; std::memcpy(&a, args.In(0).GetPointer(), sizeof(a)); std::memcpy(&b, args.In(1).GetPointer(), sizeof(b)); out = a.user_context.timestamp.source_id == b.user_context.timestamp.source_id ? (b.user_context.offset - a.user_context.offset) * 1000000000LL : 0; return 0; }), 500, CommandOptions{false,0,0,BufferMode::FixedPointer,0xD0,0}),
        CMD(Static, NoData, s64, 2, 0, ([](Static &, const NoData &, s64 &out, Arguments<2,0> &args) -> Result { ClockSnapshot a{}, b{}; std::memcpy(&a, args.In(0).GetPointer(), sizeof(a)); std::memcpy(&b, args.In(1).GetPointer(), sizeof(b)); if (a.steady_clock_time_point.source_id == b.steady_clock_time_point.source_id) out = (b.steady_clock_time_point.time_point - a.steady_clock_time_point.time_point) * 1000000000LL; else { if (!a.network_time || !b.network_time) return MAKERESULT(116,200); out = (b.network_time - a.network_time) * 1000000000LL; } return 0; }), 501, CommandOptions{false,0,0,BufferMode::FixedPointer,0xD0,0}),
        CMD(Static, NoData, NoData, 0, 0, ([](Static &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 600),
        CMD(Static, NoData, NoData, 0, 0, ([](Static &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 601)
    });
    return commands.Dispatch(this, r, reply);
}
}
namespace {
ObjectServer time_server({60, 60, 128});
ObjectServer static_server({60, 60, 128});
Thread time_thread{}, static_thread{};
alignas(4096) u8 time_stack[32 * 1024], static_stack[32 * 1024];
ObjectRef OpenManager(void *) { return MakeObject<Manager>(); }
ObjectRef OpenAlarm(void *) { return MakeObject<Alarm>(); }
ObjectRef OpenPower(void *) { return MakeObject<Power>(); }
void TimeThread(void *) { KUMO_ABORT_RESULT(time_server.Run()); KUMO_ABORT("time server returned"); }
void StaticThread(void *) { KUMO_ABORT_RESULT(static_server.Run()); KUMO_ABORT("time static server returned"); }
}
void InitializeTimeServer() {
    KUMO_ABORT_RESULT(time_server.Register({"time:m", 1, nullptr, OpenManager}));
    KUMO_ABORT_RESULT(time_server.Register({"time:al", 2, nullptr, OpenAlarm}));
    KUMO_ABORT_RESULT(time_server.Register({"time:p", 1, nullptr, OpenPower}));
    KUMO_ABORT_RESULT(static_server.Register({"time:s", 2, nullptr, OpenSystemStatic}));
    KUMO_ABORT_RESULT(static_server.Register({"time:su", 3, nullptr, OpenSystemUserStatic}));
    s32 priority{}; KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
    KUMO_ABORT_RESULT(threadCreate(&time_thread, TimeThread, nullptr, time_stack, sizeof(time_stack), priority, -2));
    KUMO_ABORT_RESULT(threadStart(&time_thread));
    KUMO_ABORT_RESULT(threadCreate(&static_thread, StaticThread, nullptr, static_stack, sizeof(static_stack), priority, -2));
    KUMO_ABORT_RESULT(threadStart(&static_thread));
}
ObjectRef OpenSystemStatic(void *) { return MakeObject<Static>(Permission_System); }
ObjectRef OpenSystemUserStatic(void *) { return MakeObject<Static>(Permission_SystemUser); }
}
