#include "glue_server.hpp"
#include "glue_inner.hpp"
#include "glue_services.hpp"
#include <kumo/ipc/object_server.hpp>
#include <kumo/prelude.hpp>
namespace kumo::horizon::glue {
namespace {
kumo::ipc::ObjectServer server({56, 56, 128});
kumo::ipc::ObjectRef Open(void *context) {
  return kumo::ipc::MakeObject<GlueStaticService>(
      static_cast<InnerStatic *>(context));
}
} // namespace
void InitializeServer() {
  KUMO_ABORT_RESULT(server.Register({"time:u", 8, &GetUserStatic(), Open}));
  KUMO_ABORT_RESULT(server.Register({"time:a", 2, &GetAdminStatic(), Open}));
  KUMO_ABORT_RESULT(server.Register({"time:r", 1, &GetRepairStatic(), Open}));
}
[[noreturn]] void LoopServer() {
  KUMO_ABORT_RESULT(server.Run());
  KUMO_ABORT("glue server returned");
}
} // namespace kumo::horizon::glue
