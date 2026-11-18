/* PSC and its hosted time services, using libnx and the project IPC runtime. */
#include "psc_pm_manager.hpp"
#include "../source/psc_log.hpp"
#include "time/timesrv_core.hpp"
#include "time/timesrv_ipc.hpp"
#include "time/timesrv_sntp.hpp"

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
void __libnx_exit(int) { KUMO_ABORT("psc exited"); }
}
namespace kumo::horizon::psc::srv { void InitializeServer(); [[noreturn]] void LoopServer(); }
int main(int, char **) {
    kumo::horizon::psc::srv::Log("[psc] starting\n");
    kumo::horizon::psc::srv::InitializeManager();
    kumo::horizon::psc::srv::InitializeServer();
    kumo::horizon::timesrv::srv::InitializeCore();
    kumo::horizon::timesrv::srv::InitializeTimeServer();
    kumo::horizon::timesrv::srv::StartSntpClient();
    kumo::horizon::psc::srv::Log("[psc] serving psc:m, psc:c and time services\n");
    kumo::horizon::psc::srv::LoopServer();
}
