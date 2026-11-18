/* Temporary power-service harness; the production KIP must also serve time. */
#include "psc_pm_manager.hpp"
#include "../source/psc_log.hpp"
#include "time/timesrv_core.hpp"
#include "time/timesrv_utc.hpp"
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
    using namespace kumo::horizon::timesrv;
    using namespace kumo::horizon::timesrv::srv;
    InitializeCore();
    s64 current = 0;
    if (GetCurrentTime(ClockId_User, &current) || current < DefaultWallClockSeconds || !IsSharedMemoryAvailable()) KUMO_ABORT("native time core startup");
    if (!GetCurrentTime(ClockId_Network, &current)) KUMO_ABORT("native network clock should start unset");
    const Handle clock_event = GetClockEventHandle(ClockId_User);
    s32 event_index = -1;
    if (!svcWaitSynchronization(&event_index, &clock_event, 1, 0)) KUMO_ABORT("native clock event initially signaled");
    if (SetCurrentTime(ClockId_User, 1234567890) || svcWaitSynchronization(&event_index, &clock_event, 1, 0)) KUMO_ABORT("native clock event");
    if (GetCurrentTime(ClockId_User, &current) || current < 1234567890 || current > 1234567891) KUMO_ABORT("native clock set");
    CalendarTime calendar{};
    CalendarAdditionalInfo info{};
    if (ToCalendarTime(&calendar, &info, 0, GetUtcRule()) || calendar.year != 1970 || calendar.month != 1 || calendar.day != 1) KUMO_ABORT("native UTC conversion");
    s64 instants[2]{};
    s32 count = 0;
    if (ToPosixTimes(&count, instants, 2, calendar, GetUtcRule()) || count != 1 || instants[0] != 0) KUMO_ABORT("native UTC roundtrip");
    if (ToCalendarTime(&calendar, &info, -1, GetUtcRule()) || calendar.year != 1969 || calendar.month != 12 || calendar.day != 31 || calendar.second != 59) KUMO_ABORT("native UTC before epoch");
    calendar.month = 13;
    if (!ToPosixTimes(&count, instants, 2, calendar, GetUtcRule())) KUMO_ABORT("native invalid calendar");
    if (SetCurrentTime(ClockId_User, DefaultWallClockSeconds)) KUMO_ABORT("native clock reset");
    KUMO_ABORT_RESULT(svcResetSignal(clock_event));
    kumo::horizon::psc::srv::Log("[psc] native time core smoke checks passed\n");
    kumo::horizon::psc::srv::InitializeManager();
    kumo::horizon::psc::srv::InitializeServer();
    InitializeTimeServer();
    StartSntpClient();
    kumo::horizon::psc::srv::Log("[psc] native power-service test harness\n");
    kumo::horizon::psc::srv::LoopServer();
}
