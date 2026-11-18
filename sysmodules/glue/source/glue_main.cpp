#include "glue_inner.hpp"
#include "glue_log.hpp"
#include "glue_server.hpp"
#include <kumo/prelude.hpp>
#include <switch.h>
extern "C" {
u32 __nx_applet_type = AppletType_None;
void __libnx_initheap() {
  alignas(16) static unsigned char heap[1024 * 1024];
  extern char *fake_heap_start, *fake_heap_end;
  fake_heap_start = reinterpret_cast<char *>(heap);
  fake_heap_end = reinterpret_cast<char *>(heap + sizeof(heap));
}
void __appInit() { KUMO_ABORT_RESULT(smInitialize()); }
void __appExit() { smExit(); }
void __libnx_exit(int) { KUMO_ABORT("glue exited"); }
}
int main(int, char **) {
  kumo::horizon::glue::InitializeServer();
  kumo::horizon::glue::StartConnectThread();
  kumo::horizon::glue::Log("[glue] serving time:u, time:a, time:r\n");
  kumo::horizon::glue::LoopServer();
}
