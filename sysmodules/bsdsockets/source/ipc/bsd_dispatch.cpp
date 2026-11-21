#include <kumo/ipc/dispatch.hpp>
#include "bsd_dispatch.hpp"
namespace kumo::horizon::hznet::ipc {
namespace {
using namespace kumo::ipc;
struct I1 { static constexpr size_t WireBytes = 4; s32 a; };
struct I2 { static constexpr size_t WireBytes = 8; s32 a, b; };
struct I3 { static constexpr size_t WireBytes = 12; s32 a, b, c; };
struct Registration { static constexpr size_t WireBytes = 48; LibraryConfigData config; u64 pid; u64 size; };
struct Duplicate { static constexpr size_t WireBytes = 16; s32 fd; u32 padding; u64 source_pid; };
struct Version { static constexpr size_t WireBytes = 16; u64 pid; u32 version; u32 padding; };
struct Pair { static constexpr size_t WireBytes = 8; s32 value, error; };
struct PairExtra { static constexpr size_t WireBytes = 12; s32 value, error; u32 extra; };
struct PairExtra64 { static constexpr size_t WireBytes = 16; s32 value, error; u64 extra; };
static_assert(sizeof(Registration) == 48 && sizeof(Duplicate) == 16 && sizeof(Version) == 16);
static_assert(offsetof(Duplicate, source_pid) == 8);
static_assert(sizeof(Pair) == 8 && sizeof(PairExtra) == 12 && sizeof(PairExtra64) == 16);
static_assert(offsetof(Registration, pid) == 32 && offsetof(Registration, size) == 40);
static_assert(offsetof(PairExtra64, extra) == 8 && offsetof(Version, version) == 8);
constexpr CommandTable Commands({
    MakeCommand<BsdService, Registration, s32, 0, 0,
        [](BsdService &s, const Registration &x, s32 &o, Arguments<0, 0> &a) -> Result {
            return s.RegisterClient(kumo::ipc::Out<s32>(&o), x.config, a.Pid(), x.size, a.TakeCopy(0));
        }>(0, {true, 1}),
    MakeCommand<BsdService, u64, NoData, 0, 0,
        [](BsdService &s, const u64 &, NoData &, Arguments<0, 0> &a) -> Result {
            return s.StartMonitoring(a.Pid());
        }>(1, {true}),
    MakeCommand<BsdService, I3, Pair, 0, 0,
        [](BsdService &s, const I3 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.Socket(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, x.c);
        }>(2, {}),
    MakeCommand<BsdService, I3, Pair, 0, 0,
        [](BsdService &s, const I3 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.SocketExempt(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, x.c);
        }>(3, {}),
    MakeCommand<BsdService, I1, Pair, 1, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.Open(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), a.In(0), x.a);
        }>(4, {}),
    MakeCommand<BsdService, SelectInput, Pair, 3, 3,
        [](BsdService &s, const SelectInput &x, Pair &o, Arguments<3, 3> &a) -> Result {
            return s.Select(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x, a.In(0), a.In(1), a.In(2), a.Out(0), a.Out(1), a.Out(2));
        }>(5, {}),
    MakeCommand<BsdService, I2, Pair, 1, 1,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<1, 1> &a) -> Result {
            return s.Poll(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, a.In(0), a.Out(0));
        }>(6, {}),
    MakeCommand<BsdService, NoData, PairExtra64, 2, 1,
        [](BsdService &s, const NoData &, PairExtra64 &o, Arguments<2, 1> &a) -> Result {
            return s.Sysctl(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u64>(&o.extra), a.In(0), a.In(1), a.Out(0));
        }>(7, {}),
    MakeCommand<BsdService, I2, Pair, 0, 1,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<0, 1> &a) -> Result {
            return s.Recv(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, a.Out(0));
        }>(8, {}),
    MakeCommand<BsdService, I2, PairExtra, 0, 2,
        [](BsdService &s, const I2 &x, PairExtra &o, Arguments<0, 2> &a) -> Result {
            return s.RecvFrom(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u32>(&o.extra), x.a, x.b, a.Out(0), a.Out(1));
        }>(9, {}),
    MakeCommand<BsdService, I2, Pair, 1, 0,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.Send(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, a.In(0));
        }>(10, {}),
    MakeCommand<BsdService, I2, Pair, 2, 0,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<2, 0> &a) -> Result {
            return s.SendTo(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, a.In(0), a.In(1));
        }>(11, {}),
    MakeCommand<BsdService, I1, PairExtra, 0, 1,
        [](BsdService &s, const I1 &x, PairExtra &o, Arguments<0, 1> &a) -> Result {
            return s.Accept(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u32>(&o.extra), x.a, a.Out(0));
        }>(12, {}),
    MakeCommand<BsdService, I1, Pair, 1, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.Bind(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, a.In(0));
        }>(13, {}),
    MakeCommand<BsdService, I1, Pair, 1, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.Connect(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, a.In(0));
        }>(14, {}),
    MakeCommand<BsdService, I1, PairExtra, 0, 1,
        [](BsdService &s, const I1 &x, PairExtra &o, Arguments<0, 1> &a) -> Result {
            return s.GetPeerName(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u32>(&o.extra), x.a, a.Out(0));
        }>(15, {}),
    MakeCommand<BsdService, I1, PairExtra, 0, 1,
        [](BsdService &s, const I1 &x, PairExtra &o, Arguments<0, 1> &a) -> Result {
            return s.GetSockName(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u32>(&o.extra), x.a, a.Out(0));
        }>(16, {}),
    MakeCommand<BsdService, I3, PairExtra, 0, 1,
        [](BsdService &s, const I3 &x, PairExtra &o, Arguments<0, 1> &a) -> Result {
            return s.GetSockOpt(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), kumo::ipc::Out<u32>(&o.extra), x.a, x.b, x.c, a.Out(0));
        }>(17, {}),
    MakeCommand<BsdService, I2, Pair, 0, 0,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.Listen(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b);
        }>(18, {}),
    MakeCommand<BsdService, I3, Pair, 4, 4,
        [](BsdService &s, const I3 &x, Pair &o, Arguments<4, 4> &a) -> Result {
            return s.Ioctl(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, x.c, a.In(0), a.In(1), a.In(2), a.In(3), a.Out(0), a.Out(1), a.Out(2), a.Out(3));
        }>(19, {}),
    MakeCommand<BsdService, I3, Pair, 0, 0,
        [](BsdService &s, const I3 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.Fcntl(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, x.c);
        }>(20, {}),
    MakeCommand<BsdService, I3, Pair, 1, 0,
        [](BsdService &s, const I3 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.SetSockOpt(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b, x.c, a.In(0));
        }>(21, {}),
    MakeCommand<BsdService, I2, Pair, 0, 0,
        [](BsdService &s, const I2 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.Shutdown(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, x.b);
        }>(22, {}),
    MakeCommand<BsdService, I1, Pair, 0, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.ShutdownAllSockets(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a);
        }>(23, {}),
    MakeCommand<BsdService, I1, Pair, 1, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<1, 0> &a) -> Result {
            return s.Write(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, a.In(0));
        }>(24, {}),
    MakeCommand<BsdService, I1, Pair, 0, 1,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<0, 1> &a) -> Result {
            return s.Read(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a, a.Out(0));
        }>(25, {}),
    MakeCommand<BsdService, I1, Pair, 0, 0,
        [](BsdService &s, const I1 &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.Close(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.a);
        }>(26, {}),
    MakeCommand<BsdService, Duplicate, Pair, 0, 0,
        [](BsdService &s, const Duplicate &x, Pair &o, Arguments<0, 0> &) -> Result {
            return s.DuplicateSocket(kumo::ipc::Out<s32>(&o.value), kumo::ipc::Out<s32>(&o.error), x.fd, x.source_pid);
        }>(27, {}),
    MakeCommand<BsdService, Duplicate, Pair, 1, 1,
        [](BsdService &, const Duplicate &, Pair &o, Arguments<1, 1> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(28, {true}),
    MakeCommand<BsdService, NoData, Pair, 0, 1,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 1> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(29, {false, 0, 0, BufferMode::Mapped}),
    MakeCommand<BsdService, NoData, Pair, 0, 1,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 1> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(30, {false, 0, 0, BufferMode::Mapped}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(31, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(32, {}),
    MakeCommand<BsdService, Registration, s32, 0, 0,
        [](BsdService &s, const Registration &x, s32 &o, Arguments<0, 0> &a) -> Result {
            return s.RegisterClientShared(kumo::ipc::Out<s32>(&o), x.config, a.Pid(), x.size);
        }>(33, {true, 0}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(34, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(35, {}),
    MakeCommand<BsdService, Version, s32, 0, 0,
        [](BsdService &s, const Version &x, s32 &o, Arguments<0, 0> &a) -> Result {
            return s.Cmd36(kumo::ipc::Out<s32>(&o), x.version, a.Pid());
        }>(36, {true}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(37, {}),
    MakeCommand<BsdService, NoData, Pair, 1, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<1, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(38, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(39, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(40, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(41, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(42, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(43, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0,
        [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result {
            o.value = -1; o.error = wire::Errno_EOPNOTSUPP; return 0;
        }>(44, {}),
    MakeCommand<BsdService, NoData, Pair, 0, 0, [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result { o.value=-1; o.error=wire::Errno_EOPNOTSUPP; return 0; }>(200),
    MakeCommand<BsdService, NoData, Pair, 0, 0, [](BsdService &, const NoData &, Pair &o, Arguments<0, 0> &) -> Result { o.value=-1; o.error=wire::Errno_EOPNOTSUPP; return 0; }>(201),
});
}
bool DispatchBsd(BsdService &service, kumo::ipc::ConcurrentRequest &request, kumo::ipc::ConcurrentReply &reply) {
    return kumo::ipc::Dispatch(&service, Commands, request, reply);
}
}
