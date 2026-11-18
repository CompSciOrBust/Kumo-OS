#pragma once
#include <kumo/ipc/dispatch.hpp>
#include <cassert>
namespace kumo::ipc::test {
// A NIFM-shaped service without any networking implementation: returned child,
// two borrowed events, fixed profile/client-ID pointers, and mapped arrays.
class Fixture final : public ServiceObject {
  public:
    inline static std::atomic<int> live{0};
    inline static Handle events[2] = {70, 71};
    Fixture() { ++live; }
    ~Fixture() { --live; }
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        constexpr auto snapshot_difference = [](Fixture &, const NoData &, s64 &out, Arguments<2, 0> &a) -> Result {
            out = s64(a.In(0).GetPointer()[0]) - s64(a.In(1).GetPointer()[0]);
            return 0;
        };
        constexpr auto to_posix = [](Fixture &, const u64 &calendar, s32 &out, auto &a) -> Result {
            if (calendar == 0) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
            auto buffer = a.Out(0);
            auto value = calendar;
            if constexpr (std::tuple_size_v<decltype(a.inputs)> != 0) {
                assert(a.In(0).GetSize() == 0x4000);
                value += a.In(0).GetPointer()[0];
            }
            out = buffer.GetSize() >= sizeof(value);
            if (out) std::memcpy(buffer.GetPointer(), &value, sizeof(value));
            return 0;
        };
        static constexpr CommandTable commands(
            {MakeCommand<Fixture, NoData, NoData, 0, 0,
                         [](Fixture &, const NoData &, NoData &, Arguments<0, 0> &a) -> Result {
                             auto object = MakeObject<Fixture>();
                             if (!object)
                                 return MAKERESULT(Module_Kernel, KernelError_OutOfMemory);
                             a.ReturnObject(std::move(object));
                             return 0;
                         }>(0),
             MakeCommand<Fixture, NoData, NoData, 0, 0,
                         [](Fixture &, const NoData &, NoData &, Arguments<0, 0> &a) -> Result {
                             a.ReturnCopy(BorrowedHandle{events[0]}, 0);
                             a.ReturnCopy(BorrowedHandle{events[1]}, 1);
                             return 0;
                         }>(1),
             MakeCommand<Fixture, NoData, NoData, 0, 1,
                         [](Fixture &, const NoData &, NoData &, Arguments<0, 1> &a) -> Result {
                             auto buffer = a.Out(0);
                             for (size_t i = 0; i < buffer.GetSize(); ++i)
                                 buffer.GetPointer()[i] = static_cast<u8>(i);
                             return 0;
                         }>(2, {false, 0, 0, BufferMode::FixedPointer, 0, 0x17c}),
             MakeCommand<Fixture, NoData, u32, 1, 0,
                         [](Fixture &, const NoData &, u32 &out, Arguments<1, 0> &a) -> Result {
                             std::memcpy(&out, a.In(0).GetPointer(), 4);
                             return 0;
                         }>(3, {false, 0, 0, BufferMode::FixedPointer, 4, 0}),
             MakeCommand<Fixture, u8, u32, 0, 1,
                         [](Fixture &, const u8 &in, u32 &out, Arguments<0, 1> &a) -> Result {
                             out = a.Out(0).GetSize() / 0x75;
                             std::memset(a.Out(0).GetPointer(), in, a.Out(0).GetSize());
                             return 0;
                         }>(4, {false, 0, 0, BufferMode::Mapped}),
             MakeCommand<Fixture, u64, u64, 0, 0,
                         [](Fixture &, const u64 &, u64 &out, Arguments<0, 0> &a) -> Result {
                             out = a.Pid().GetValue();
                             return 0;
                         }>(5, {true}),
             MakeCommand<Fixture, s32, NoData, 2, 1,
                         [](Fixture &, const s32 &id, NoData &, Arguments<2, 1> &a) -> Result {
                             if (id != 7 || a.In(0).GetPointer()[0] != '/' ||
                                 a.In(1).GetSize() != 4 || a.Out(0).GetSize() != 4)
                                 return MAKERESULT(Module_Libnx, LibnxError_BadInput);
                             std::memcpy(a.Out(0).GetPointer(), a.In(1).GetPointer(), 4);
                             return 0;
                         }>(15, {.buffers = BufferMode::FixedPointerAndNonSecureMapped, .input_pointer_size = 0x301}),
             MakeCommand<Fixture, NoData, s64, 2, 0, snapshot_difference>(
                 500, {.buffers = BufferMode::FixedPointer, .input_pointer_size = 0xD0}),
             MakeCommand<Fixture, NoData, s64, 2, 0, snapshot_difference>(
                 501, {.buffers = BufferMode::FixedPointer, .input_pointer_size = 0xD0}),
             MakeCommand<Fixture, u64, s32, 1, 1, to_posix>(201, {.buffers = BufferMode::PointerOutput}),
             MakeCommand<Fixture, u64, s32, 0, 1, to_posix>(202, {.buffers = BufferMode::PointerOutput})});
        return kumo::ipc::Dispatch(this, commands, request, reply);
    }
};
} // namespace kumo::ipc::test
