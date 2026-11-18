/* A program installed as NCAs and started by pm and the loader. */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/result.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    void Log(const char *fmt, ...) {
        char buf[0x100];
        std::va_list args;
        va_start(args, fmt);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (len > 0) svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
    }

    char g_path[FS_MAX_PATH] = "/hello_program.txt";

    Result UseSdCard() {
        constexpr char Text[] = "written by a program the loader started\n";
        KUMO_TRY(fsInitialize());
        kumo::ScopeExit close_fs([] { fsExit(); });
        FsFileSystem sd;
        KUMO_TRY(fsOpenSdCardFileSystem(&sd));
        kumo::ScopeExit close_sd([&] { fsFsClose(&sd); });
        const Result deleted = fsFsDeleteFile(&sd, g_path);
        if (R_FAILED(deleted) && !kumo::IsResult(deleted, MAKERESULT(2, 1))) return deleted;
        KUMO_TRY(fsFsCreateFile(&sd, g_path, sizeof(Text) - 1, 0));
        FsFile file;
        KUMO_TRY(fsFsOpenFile(&sd, g_path, FsOpenMode_Read | FsOpenMode_Write, &file));
        kumo::ScopeExit close_file([&] { fsFileClose(&file); });
        KUMO_TRY(fsFileWrite(&file, 0, Text, sizeof(Text) - 1, FsWriteOption_Flush));
        char read_back[sizeof(Text)] = {};
        u64 read = 0;
        KUMO_TRY(fsFileRead(&file, 0, read_back, sizeof(Text) - 1, FsReadOption_None, &read));
        KUMO_UNLESS(read == sizeof(Text) - 1 && std::memcmp(read_back, Text, read) == 0,
                    MAKERESULT(2, 4000));
        return 0;
    }
}

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    void __libnx_initheap() {
        alignas(16) static char heap[1024 * 1024];
        extern char *fake_heap_start, *fake_heap_end;
        fake_heap_start = heap;
        fake_heap_end = heap + sizeof(heap);
    }
    void __appInit() {
        SecmonArgs config{{0xC3000002, 65011}};
        svcCallSecureMonitor(&config);
        KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0);
        hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
        KUMO_ABORT_RESULT(smInitialize());
    }
    void __appExit() { smExit(); }
    void __libnx_exit(int) { svcExitProcess(); }
}

int main(int, char **) {
    u64 program_id = 0, process_id = 0;
    KUMO_ABORT_RESULT(svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0));
    KUMO_ABORT_RESULT(svcGetProcessId(&process_id, CUR_PROCESS_HANDLE));
    Log("[hello_program] running: program %016lx, process %lu\n", program_id, process_id);
    const Result rc = UseSdCard();
    Log("[hello_program] wrote and read back a file on the SD card: %s (rc=0x%x)\n",
        R_SUCCEEDED(rc) ? "PASS" : "FAIL", rc);
    return R_SUCCEEDED(rc) ? 0 : 1;
}
