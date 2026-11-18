#pragma once
#include <kumo/prelude.hpp>
#include "../source/psc_protocol.hpp"

namespace kumo::horizon::psc::srv {
enum PmState : u32 {
    PmState_FullAwake = 0, PmState_MinimumAwake = 1, PmState_SleepReady = 2,
    PmState_EssentialServicesSleepReady = 3, PmState_EssentialServicesAwake = 4,
    PmState_ShutdownReady = 5,
};
using PmModuleId = u32;
using PmFlagSet = u32;
struct ModuleEntry;
void InitializeManager();
Result RegisterModule(ModuleEntry **out, Handle *out_event, PmModuleId id, const u32 *dependencies, size_t count);
void UnregisterModule(ModuleEntry *entry, bool closed);
void GetModuleRequest(ModuleEntry *entry, PmState *out_state, PmFlagSet *out_flags);
void AcknowledgeModule(ModuleEntry *entry, bool has_state, PmState state);
Handle GetControlEventHandle();
Handle GetStateLockEventHandle();
Result DispatchRequest(u32 state, u32 order, u32 flags);
Result GetResult();
PmState GetState();
Result Cancel();
void PrintModuleInformation();
constexpr Result ResultStateNotSupported() { return MAKERESULT(138, 100); }
constexpr Result ResultInvalidArgument() { return MAKERESULT(138, 101); }
constexpr Result ResultBusy() { return MAKERESULT(138, 102); }
constexpr Result ResultInProgress() { return MAKERESULT(138, 103); }
constexpr Result ResultCancelled() { return MAKERESULT(138, 104); }
constexpr Result ResultTimedOut() { return MAKERESULT(138, 105); }
constexpr Result ResultTooManyModules() { return MAKERESULT(138, 106); }
constexpr Result ResultDependencyTooDeep() { return MAKERESULT(138, 107); }
constexpr Result ResultNotSupported() { return MAKERESULT(138, 108); }
}
