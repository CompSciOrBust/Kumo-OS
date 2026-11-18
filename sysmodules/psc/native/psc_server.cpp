#include "psc_pm_manager.hpp"
#include <kumo/ipc/dispatch.hpp>
#include <kumo/ipc/object_server.hpp>

namespace kumo::horizon::psc::srv {
namespace {
using namespace kumo::ipc;
struct Request { u32 state; u32 flags; static constexpr size_t WireBytes = 8; };
struct DispatchArgs { u32 state; u32 order; u32 flags; static constexpr size_t WireBytes = 12; };
struct Information { PmModuleTransitionInformation info; u32 before; u32 after; static constexpr size_t WireBytes = 40; };
class Module final : public ServiceObject {
public:
    ModuleEntry *entry = nullptr;
    ~Module() override { if (entry) UnregisterModule(entry, true); }
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class ModuleService final : public ServiceObject {
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class Control final : public ServiceObject {
    bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
#define CMD(S,I,O,NI,NO,H,ID,...) MakeCommand<S,I,O,NI,NO,H>(ID __VA_OPT__(,) __VA_ARGS__)
bool ModuleService::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(ModuleService, NoData, NoData, 0, 0,
            ([](ModuleService &, const NoData &, NoData &, Arguments<0,0> &a) -> Result {
                auto obj = MakeObject<Module>();
                if (!obj) return ResultTooManyModules();
                a.ReturnObject(std::move(obj)); return 0;
            }), 0)
    });
    return commands.Dispatch(this, r, reply);
}
bool Module::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Module, u32, NoData, 1, 0,
            ([](Module &s, const u32 &id, NoData &, Arguments<1,0> &a) -> Result {
                if (s.entry) return MAKERESULT(138, 2);
                auto b = a.In(0);
                if (b.GetSize() % sizeof(u32) || b.GetSize() / sizeof(u32) > MaxDependencies) return ResultInvalidArgument();
                u32 deps[MaxDependencies]{};
                if (b.GetSize()) std::memcpy(deps, b.GetPointer(), b.GetSize());
                Handle h = INVALID_HANDLE;
                Result rc = RegisterModule(&s.entry, &h, id, deps, b.GetSize() / sizeof(u32));
                if (!rc) a.ReturnCopy({h});
                return rc;
            }), 0, CommandOptions{false, 0, 0, BufferMode::Mapped}),
        CMD(Module, NoData, Request, 0, 0,
            ([](Module &s, const NoData &, Request &out, Arguments<0,0> &) -> Result {
                if (!s.entry) return MAKERESULT(138, 3);
                PmState state; GetModuleRequest(s.entry, &state, &out.flags); out.state = state; return 0;
            }), 1),
        CMD(Module, NoData, NoData, 0, 0,
            ([](Module &s, const NoData &, NoData &, Arguments<0,0> &) -> Result {
                if (!s.entry) return MAKERESULT(138, 3);
                AcknowledgeModule(s.entry, false, PmState_FullAwake); return 0;
            }), 2),
        CMD(Module, NoData, NoData, 0, 0,
            ([](Module &s, const NoData &, NoData &, Arguments<0,0> &) -> Result {
                if (!s.entry) return MAKERESULT(138, 3);
                UnregisterModule(s.entry, false); s.entry = nullptr; return 0;
            }), 3),
        CMD(Module, u32, NoData, 0, 0,
            ([](Module &s, const u32 &state, NoData &, Arguments<0,0> &) -> Result {
                if (!s.entry) return MAKERESULT(138, 3);
                AcknowledgeModule(s.entry, true, static_cast<PmState>(state)); return 0;
            }), 4)
    });
    return commands.Dispatch(this, r, reply);
}
bool Control::Dispatch(ConcurrentRequest &r, ConcurrentReply &reply) {
    static constexpr CommandTable commands({
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetControlEventHandle()}); return 0; }), 0),
        CMD(Control, DispatchArgs, NoData, 0, 0, ([](Control &, const DispatchArgs &in, NoData &, Arguments<0,0> &) -> Result { return DispatchRequest(in.state, in.order, in.flags); }), 1),
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return GetResult(); }), 2),
        CMD(Control, NoData, u32, 0, 0, ([](Control &, const NoData &, u32 &out, Arguments<0,0> &) -> Result { out = GetState(); return 0; }), 3),
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return Cancel(); }), 4),
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &) -> Result { PrintModuleInformation(); return 0; }), 5),
        CMD(Control, NoData, Information, 0, 2, ([](Control &, const NoData &, Information &, Arguments<0,2> &) -> Result { return ResultNotSupported(); }), 6, CommandOptions{false,0,0,BufferMode::Mapped}),
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &) -> Result { return ResultNotSupported(); }), 7),
        CMD(Control, NoData, NoData, 0, 0, ([](Control &, const NoData &, NoData &, Arguments<0,0> &a) -> Result { a.ReturnCopy({GetStateLockEventHandle()}); return 0; }), 10),
        CMD(Control, u32, u8, 0, 0, ([](Control &, const u32 &, u8 &out, Arguments<0,0> &) -> Result { out = 0; return 0; }), 11)
    });
    return commands.Dispatch(this, r, reply);
}
ObjectServer server({60, 60, 128});
ObjectRef OpenModule(void *) { return MakeObject<ModuleService>(); }
ObjectRef OpenControl(void *) { return MakeObject<Control>(); }
}
void InitializeServer() {
    KUMO_ABORT_RESULT(server.Register({"psc:m", 32, nullptr, OpenModule}));
    KUMO_ABORT_RESULT(server.Register({"psc:c", 4, nullptr, OpenControl}));
}
[[noreturn]] void LoopServer() { KUMO_ABORT_RESULT(server.Run()); KUMO_ABORT("psc server returned"); }
}
