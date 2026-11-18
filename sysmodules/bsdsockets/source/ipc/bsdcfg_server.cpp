#include <kumo/prelude.hpp>
#include <kumo/ipc/server.hpp>
#include <kumo/ipc/dispatch.hpp>
#include "bsdcfg_server.hpp"
#include "bsdcfg_service.hpp"
namespace kumo::horizon::hznet::ipc {
    namespace {
        using namespace kumo::ipc;
        BsdConfigService g_service;
        kumo::ipc::Server g_server;
        template<bool WithEvent>
        Result SetUp(BsdConfigService &service, const IfUpInput &input, NoData &, Arguments<1, 0> &args) {
            const auto buffer = args.In(0);
            if constexpr (WithEvent) {
                Handle event = INVALID_HANDLE;
                Result rc = service.SetIfUpWithEvent(input, buffer.GetPointer(), buffer.GetSize(), &event);
                if (R_SUCCEEDED(rc)) args.ReturnCopy(BorrowedHandle{event});
                return rc;
            } else {
                return service.SetIfUp(input, buffer.GetPointer(), buffer.GetSize());
            }
        }
        template<auto Method>
        Result Index(BsdConfigService &service, const u32 &index, NoData &, Arguments<0, 0> &) {
            return (service.*Method)(index);
        }
        template<size_t Inputs = 0, bool Success = false>
        consteval auto Stub(u32 id) {
            return MakeCommand<BsdConfigService, NoData, NoData, Inputs, 0,
                [](BsdConfigService &, const NoData &, NoData &, Arguments<Inputs, 0> &) -> Result {
                    return Success ? 0 : kumo::KernelError(127);
                }>(id, {false, 0, 0, BufferMode::Mapped});
        }
        constexpr CommandTable Commands({
            MakeCommand<BsdConfigService, IfUpInput, NoData, 1, 0, SetUp<false>>(0, {false, 0, 0, BufferMode::Mapped}),
            MakeCommand<BsdConfigService, IfUpInput, NoData, 1, 0, SetUp<true>>(1, {false, 0, 0, BufferMode::Mapped}),
            MakeCommand<BsdConfigService, u32, NoData, 0, 0, Index<&BsdConfigService::CancelIf>>(2),
            MakeCommand<BsdConfigService, u32, NoData, 0, 0, Index<&BsdConfigService::SetIfDown>>(3),
            MakeCommand<BsdConfigService, u32, IfState, 0, 0,
                [](BsdConfigService &service, const u32 &index, IfState &state, Arguments<0, 0> &) -> Result {
                    return service.GetIfState(&state, index);
                }>(4),
            MakeCommand<BsdConfigService, u32, NoData, 0, 0, Index<&BsdConfigService::DhcpRenew>>(5),
            Stub(6), Stub(7), Stub(8), Stub(9), Stub(10), Stub(11), Stub(12),
            Stub<1, true>(13), Stub<1, true>(14), Stub<1, true>(15),
            Stub(16), Stub(17), Stub(18), Stub(19), Stub(20), Stub(21), Stub(22), Stub(23), Stub(24),
            Stub(50), Stub(51), Stub(52), Stub(53), Stub(54), Stub(55), Stub(56), Stub(57), Stub(58), Stub(100),
        });
        bool DispatchConfig(void *service, ConcurrentRequest &request, ConcurrentReply &reply) {
            return Dispatch(service, Commands, request, reply);
        }
        constexpr size_t ThreadStackSize = (16 * 1024);
        alignas(0x1000) constinit u8 g_thread_stack[ThreadStackSize];
        constinit ::Thread g_thread;

        void ThreadFunction(void *) { KUMO_ABORT_RESULT(g_server.Run()); }
    }

    void StartConfigServer() {
        const kumo::ipc::Service service{"bsdcfg", DispatchConfig, &g_service};
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(g_server.Register(service, 4)));
        s32 priority = 0;
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)));
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadCreate(&g_thread, ThreadFunction, nullptr, g_thread_stack, ThreadStackSize, priority, -2)));
        KUMO_ABORT_UNLESS(::kumo::IsSuccess(::threadStart(&g_thread)));
    }
}
