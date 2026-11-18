#include "nifm_server.hpp"
#include "nifm_general_service.hpp"
#include "nifm_request.hpp"
#include "nifm_wire.hpp"
#include <atomic>
#include <cstring>
#include <kumo/ipc/dispatch.hpp>
#include <kumo/ipc/object_server.hpp>
#include <kumo/result.hpp>

namespace kumo::horizon::nifm {
namespace {
using namespace kumo::ipc;
constexpr CommandOptions fixed_out(size_t n) {
  return {false, 0, 0, BufferMode::FixedPointer, 0, n};
}
constexpr CommandOptions fixed_in(size_t n) {
  return {false, 0, 0, BufferMode::FixedPointer, n, 0};
}
constexpr CommandOptions mapped{false, 0, 0, BufferMode::Mapped};
template <class To, class From> To wire_copy(const From &from) {
  static_assert(sizeof(To) == sizeof(From));
  To to{};
  std::memcpy(&to, &from, sizeof(to));
  return to;
}
class RequestObject final : public ServiceObject {
public:
  using ServiceType = Request;
  Request service;
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class GeneralObject final : public ServiceObject {
public:
  using ServiceType = GeneralService;
  GeneralService service;
  GeneralObject(u32 type, u32 id) : service(type, id) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
class StaticObject final : public ServiceObject {
public:
  u32 type;
  explicit StaticObject(u32 t) : type(t) {}
  bool Dispatch(ConcurrentRequest &, ConcurrentReply &) override;
};
std::atomic<u32> next_client{1};
template <class S, class I, class O, size_t NI, size_t NO>
::Result unsupported(S &, const I &, O &, Arguments<NI, NO> &) {
  return kumo::KernelError(127);
}
template <auto Method, class S, class I, class O, size_t NI, size_t NO>
::Result noarg(S &s, const I &, O &, Arguments<NI, NO> &) {
  return (s.service.*Method)();
}
template <auto Method, class S, class I, class O, size_t NI, size_t NO>
::Result inarg(S &s, const I &in, O &, Arguments<NI, NO> &) {
  return (s.service.*Method)(in);
}
template <auto Method, class S, class I, class O, size_t NI, size_t NO>
::Result boolin(S &s, const I &in, O &, Arguments<NI, NO> &) {
  return (s.service.*Method)(in.value != 0);
}
template <auto Method, class S, class I, class O, size_t NI, size_t NO>
::Result boolout(S &s, const I &, O &out, Arguments<NI, NO> &) {
  bool value{};
  auto rc = (s.service.*Method)(value);
  out.value = value;
  return rc;
}
template <auto Method, class S, class I, class O, size_t NI, size_t NO>
::Result outarg(S &s, const I &, O &out, Arguments<NI, NO> &) {
  return (s.service.*Method)(out);
}
#define CMD(S, I, O, NI, NO, H, ID, ...)                                       \
  MakeCommand<S, I, O, NI, NO, H>(ID __VA_OPT__(, ) __VA_ARGS__)
#define UNSUP(S, ID)                                                           \
  CMD(S, NoData, NoData, 0, 0, (unsupported<S, NoData, NoData, 0, 0>), ID)
#define SIMPLE(S, M, ID)                                                       \
  CMD(S, NoData, NoData, 0, 0,                                                 \
      (noarg<&S::ServiceType::M, S, NoData, NoData, 0, 0>), ID)
#define INPUT(S, T, M, ID)                                                     \
  CMD(S, T, NoData, 0, 0, (inarg<&S::ServiceType::M, S, T, NoData, 0, 0>), ID)
#define BOOLIN(S, M, ID)                                                       \
  CMD(S, wire::Boolean, NoData, 0, 0,                                          \
      (boolin<&S::ServiceType::M, S, wire::Boolean, NoData, 0, 0>), ID)
#define BOOLOUT(S, M, ID)                                                      \
  CMD(S, NoData, wire::Boolean, 0, 0,                                          \
      (boolout<&S::ServiceType::M, S, NoData, wire::Boolean, 0, 0>), ID)
bool StaticObject::Dispatch(ConcurrentRequest &request,
                            ConcurrentReply &reply) {
  static constexpr CommandTable commands(
      {CMD(StaticObject, NoData, NoData, 0, 0,
           ([](StaticObject &s, const NoData &, NoData &,
               Arguments<0, 0> &a) -> Result {
             auto obj = MakeObject<GeneralObject>(s.type, next_client++);
             if (!obj)
               return MAKERESULT(Module_Kernel, KernelError_OutOfMemory);
             a.ReturnObject(std::move(obj));
             return 0;
           }),
           4),
       CMD(StaticObject, u64, NoData, 0, 0,
           ([](StaticObject &s, const u64 &, NoData &,
               Arguments<0, 0> &a) -> Result {
             (void)a.Pid();
             auto obj = MakeObject<GeneralObject>(s.type, next_client++);
             if (!obj)
               return MAKERESULT(Module_Kernel, KernelError_OutOfMemory);
             a.ReturnObject(std::move(obj));
             return 0;
           }),
           5, CommandOptions{true})});
  const u32 id = request.input.header.command_id;
  if (id != 4 && id != 5)
    return false;
  return commands.Dispatch(this, request, reply);
}
bool GeneralObject::Dispatch(ConcurrentRequest &request,
                             ConcurrentReply &reply) {
  static constexpr CommandTable commands(
      {CMD(GeneralObject, NoData, NoData, 0, 1,
           ([](GeneralObject &s, const NoData &, NoData &,
               Arguments<0, 1> &a) -> Result {
             ClientId v{};
             auto rc = s.service.GetClientId(v);
             if (!rc)
               std::memcpy(a.Out(0).GetPointer(), &v, sizeof(v));
             return rc;
           }),
           1, fixed_out(sizeof(ClientId))),
       UNSUP(GeneralObject, 2),
       CMD(GeneralObject, s32, NoData, 0, 0,
           ([](GeneralObject &, const s32 &, NoData &,
               Arguments<0, 0> &a) -> Result {
             if (GetRequestCount() >= MaxRequests)
               return MAKERESULT(Module_Kernel, KernelError_ResourceExhausted);
             auto obj = MakeObject<RequestObject>();
             if (!obj)
               return MAKERESULT(Module_Kernel, KernelError_OutOfMemory);
             a.ReturnObject(std::move(obj));
             return 0;
           }),
           4),
       CMD(GeneralObject, NoData, NoData, 0, 1,
           ([](GeneralObject &s, const NoData &, NoData &,
               Arguments<0, 1> &a) -> Result {
             SfNetworkProfileData v{};
             auto rc = s.service.GetCurrentNetworkProfile(v);
             if (!rc)
               std::memcpy(a.Out(0).GetPointer(), &v, sizeof(v));
             return rc;
           }),
           5, fixed_out(sizeof(SfNetworkProfileData))),
       UNSUP(GeneralObject, 6),
       CMD(GeneralObject, u8, s32, 0, 1,
           ([](GeneralObject &s, const u8 &in, s32 &out,
               Arguments<0, 1> &a) -> Result {
             SfNetworkProfileBasicInfo value{};
             const auto b = a.Out(0);
             auto rc = s.service.EnumerateNetworkProfiles(
                 out, &value, b.GetSize() / sizeof(value), in);
             if (!rc && out > 0)
               std::memcpy(b.GetPointer(), &value, sizeof(value));
             return rc;
           }),
           7, mapped),
       CMD(GeneralObject, wire::Uuid, NoData, 0, 1,
           ([](GeneralObject &s, const wire::Uuid &in, NoData &,
               Arguments<0, 1> &a) -> Result {
             SfNetworkProfileData v{};
             auto rc = s.service.GetNetworkProfile(v, wire_copy<Uuid>(in));
             if (!rc)
               std::memcpy(a.Out(0).GetPointer(), &v, sizeof(v));
             return rc;
           }),
           8, fixed_out(sizeof(SfNetworkProfileData))),
       CMD(GeneralObject, NoData, wire::Uuid, 1, 0,
           (unsupported<GeneralObject, NoData, wire::Uuid, 1, 0>), 9,
           fixed_in(sizeof(SfNetworkProfileData))),
       UNSUP(GeneralObject, 10),
       UNSUP(GeneralObject, 11),
       CMD(GeneralObject, NoData, wire::Ipv4Address, 0, 0,
           ([](GeneralObject &s, const NoData &, wire::Ipv4Address &out,
               Arguments<0, 0> &) -> Result {
             Ipv4Address v{};
             auto rc = s.service.GetCurrentIpAddress(v);
             out = wire_copy<wire::Ipv4Address>(v);
             return rc;
           }),
           12),
       UNSUP(GeneralObject, 13),
       UNSUP(GeneralObject, 14),
       CMD(GeneralObject, NoData, wire::IpConfigInfo, 0, 0,
           ([](GeneralObject &s, const NoData &, wire::IpConfigInfo &out,
               Arguments<0, 0> &) -> Result {
             IpConfigInfo v{};
             auto rc = s.service.GetCurrentIpConfigInfo(v);
             out = wire_copy<wire::IpConfigInfo>(v);
             return rc;
           }),
           15),
       BOOLIN(GeneralObject, SetWirelessCommunicationEnabled, 16),
       BOOLOUT(GeneralObject, IsWirelessCommunicationEnabled, 17),
       CMD(GeneralObject, NoData, wire::ConnectionStatus, 0, 0,
           ([](GeneralObject &s, const NoData &, wire::ConnectionStatus &out,
               Arguments<0, 0> &) -> Result {
             InternetConnectionStatusData v{};
             auto rc = s.service.GetInternetConnectionStatus(v);
             out = wire_copy<wire::ConnectionStatus>(v);
             return rc;
           }),
           18),
       UNSUP(GeneralObject, 19),
       BOOLOUT(GeneralObject, IsEthernetCommunicationEnabled, 20),
       CMD(GeneralObject, NoData, wire::Boolean, 1, 0,
           ([](GeneralObject &s, const NoData &, wire::Boolean &out,
               Arguments<1, 0> &a) -> Result {
             ClientId id{};
             std::memcpy(&id, a.In(0).GetPointer(), sizeof(id));
             bool value{};
             auto rc = s.service.IsAnyInternetRequestAccepted(value, id);
             out.value = value;
             return rc;
           }),
           21, fixed_in(sizeof(ClientId))),
       BOOLOUT(GeneralObject, IsAnyForegroundRequestAccepted, 22),
       SIMPLE(GeneralObject, PutToSleep, 23),
       SIMPLE(GeneralObject, WakeUp, 24),
       UNSUP(GeneralObject, 25),
       UNSUP(GeneralObject, 26),
       UNSUP(GeneralObject, 27),
       UNSUP(GeneralObject, 28),
       UNSUP(GeneralObject, 29),
       UNSUP(GeneralObject, 30),
       UNSUP(GeneralObject, 31),
       UNSUP(GeneralObject, 32),
       UNSUP(GeneralObject, 33),
       UNSUP(GeneralObject, 34),
       UNSUP(GeneralObject, 35),
       UNSUP(GeneralObject, 36),
       UNSUP(GeneralObject, 37),
       UNSUP(GeneralObject, 38),
       UNSUP(GeneralObject, 39),
       UNSUP(GeneralObject, 40),
       UNSUP(GeneralObject, 41),
       UNSUP(GeneralObject, 42),
       INPUT(GeneralObject, s32, SetWowlDelayedWakeTime, 43),
       UNSUP(GeneralObject, 44),
       UNSUP(GeneralObject, 45),
       UNSUP(GeneralObject, 46),
       UNSUP(GeneralObject, 47),
       UNSUP(GeneralObject, 48),
       UNSUP(GeneralObject, 49),
       UNSUP(GeneralObject, 50),
       UNSUP(GeneralObject, 51),
       UNSUP(GeneralObject, 52),
       UNSUP(GeneralObject, 53),
       UNSUP(GeneralObject, 54),
       UNSUP(GeneralObject, 55),
       UNSUP(GeneralObject, 56),
       UNSUP(GeneralObject, 57)});
  const u32 id = request.input.header.command_id;
  if (id < 1 || id == 3 || id > 57)
    return false;
  return commands.Dispatch(this, request, reply);
}
bool RequestObject::Dispatch(ConcurrentRequest &request,
                             ConcurrentReply &reply) {
  static constexpr CommandTable commands(
      {CMD(RequestObject, NoData, u32, 0, 0,
           (outarg<&Request::GetRequestState, RequestObject, NoData, u32, 0,
                   0>),
           0),
       SIMPLE(RequestObject, GetResult, 1),
       CMD(RequestObject, NoData, NoData, 0, 0,
           ([](RequestObject &s, const NoData &, NoData &,
               Arguments<0, 0> &a) -> Result {
             Handle state{}, network{};
             auto rc = s.service.GetSystemEventReadableHandles(state, network);
             if (!rc) {
               a.ReturnCopy({state}, 0);
               a.ReturnCopy({network}, 1);
             }
             return rc;
           }),
           2),
       SIMPLE(RequestObject, Cancel, 3),
       SIMPLE(RequestObject, Submit, 4),
       CMD(RequestObject, wire::Requirement, NoData, 0, 0,
           ([](RequestObject &s, const wire::Requirement &in, NoData &,
               Arguments<0, 0> &) -> Result {
             return s.service.SetRequirement(wire_copy<Requirement>(in));
           }),
           5),
       INPUT(RequestObject, u32, SetRequirementPreset, 6),
       INPUT(RequestObject, u8, SetPriority, 8),
       CMD(RequestObject, wire::Uuid, NoData, 0, 0,
           ([](RequestObject &s, const wire::Uuid &in, NoData &,
               Arguments<0, 0> &) -> Result {
             return s.service.SetNetworkProfileId(wire_copy<Uuid>(in));
           }),
           9),
       BOOLIN(RequestObject, SetRejectable, 10),
       INPUT(RequestObject, u8, SetConnectionConfirmationOption, 11),
       BOOLIN(RequestObject, SetPersistent, 12),
       BOOLIN(RequestObject, SetInstant, 13),
       BOOLIN(RequestObject, SetSustainable, 14),
       INPUT(RequestObject, u8, SetRawPriority, 15),
       BOOLIN(RequestObject, SetGreedy, 16),
       BOOLIN(RequestObject, SetSharable, 17),
       INPUT(RequestObject, u32, SetRequirementByRevision, 18),
       UNSUP(RequestObject, 19),
       UNSUP(RequestObject, 20),
       CMD(RequestObject, u32, wire::AppletInfo, 0, 1,
           (unsupported<RequestObject, u32, wire::AppletInfo, 0, 1>), 21,
           mapped),
       UNSUP(RequestObject, 22),
       BOOLIN(RequestObject, SetKeptInSleep, 23),
       INPUT(RequestObject, s32, RegisterSocketDescriptor, 24),
       INPUT(RequestObject, s32, UnregisterSocketDescriptor, 25),
       UNSUP(RequestObject, 26)});
  const u32 id = request.input.header.command_id;
  if (id == 7 || id > 26)
    return false;
  return commands.Dispatch(this, request, reply);
}
#undef CMD
#undef UNSUP
#undef SIMPLE
#undef INPUT
#undef BOOLIN
#undef BOOLOUT
ObjectRef OpenPort(void *context) {
  return MakeObject<StaticObject>(*static_cast<const u32 *>(context));
}
u32 port_types[] = {ServiceTypeUser, ServiceTypeSystem, ServiceTypeAdmin};
ObjectServer server({26, 32, 128});
} // namespace
::Result InitializeServer() {
  KUMO_TRY(server.Register({"nifm:u", 16, &port_types[0], OpenPort}));
  KUMO_TRY(server.Register({"nifm:s", 8, &port_types[1], OpenPort}));
  KUMO_TRY(server.Register({"nifm:a", 2, &port_types[2], OpenPort}));
  return 0;
}
void LoopServer() {
  const auto rc = server.Run();
  diagAbortWithResult(rc);
}
} // namespace kumo::horizon::nifm
