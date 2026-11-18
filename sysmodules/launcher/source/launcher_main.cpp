/*
 * Starts the programs that should run at boot.
 *
 * On a Switch this is boot2's job. Atmosphere's boot2 cannot run here: it
 * needs the settings and GPIO services and ams.mitm, and its launch list is
 * the Switch's own system programs (docs/boot2_assessment.md). This is the
 * small part of it this OS needs: read a list of program IDs and ask pm to
 * launch each one from the system's content storage.
 *
 * The list is SYSTEM:/boot.lst, one program ID in hexadecimal per line, in
 * launch order; '#' starts a comment. scripts/build-os.py writes it from the
 * modules marked "boot" in the OS configuration. A missing list is not an
 * error: there is then nothing to start.
 *
 * A program that fails to launch is reported and skipped, so one bad entry
 * does not stop the rest of the system coming up.
 */
#include <switch.h>
#include <kumo/prelude.hpp>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

namespace {

    constexpr size_t ListSizeMax = 16 * 1024;

    /* libnx sends FS_MAX_PATH bytes for every path argument. */
    char g_list_path[FS_MAX_PATH] = "/boot.lst";
    char g_list[ListSizeMax + 1];

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

    void Log(const char *fmt, ...) {
        char buf[0x100];
        std::va_list vl;
        va_start(vl, fmt);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
        va_end(vl);
        if (len > 0) {
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }
    }

    /* Read the whole list into g_list. A missing file reads as an empty list. */
    ::Result ReadList(size_t *out_size) {
        *out_size = 0;

        KUMO_TRY(::fsInitialize());

        ::FsFileSystem system;
        KUMO_TRY(::fsOpenBisFileSystem(&system, FsBisPartitionId_System, ""));
        auto close_fs = [&] { ::fsFsClose(&system); };
        struct ScopeExit { decltype(close_fs) f; ~ScopeExit() { f(); } } fs_guard{close_fs};

        ::FsFile file;
        const ::Result open_rc = ::fsFsOpenFile(&system, g_list_path, FsOpenMode_Read, &file);
        if (open_rc == 0x202) { /* fs::ResultPathNotFound */
            KUMO_SUCCEED();
        }
        KUMO_TRY(open_rc);
        auto close_file = [&] { ::fsFileClose(&file); };
        struct ScopeExitFile { decltype(close_file) f; ~ScopeExitFile() { f(); } } file_guard{close_file};

        s64 size;
        KUMO_TRY(::fsFileGetSize(&file, &size));
        KUMO_UNLESS(static_cast<size_t>(size) <= ListSizeMax, 0x19202); /* fs::ResultInvalidSize */

        u64 read = 0;
        KUMO_TRY(::fsFileRead(&file, 0, g_list, static_cast<u64>(size), FsReadOption_None, &read));

        *out_size = static_cast<size_t>(read);
        KUMO_SUCCEED();
    }

    /* Parse one line as a hexadecimal program ID, with or without "0x". False for blank and comment lines. */
    bool ParseLine(u64 *out, bool *out_valid, const char *line, size_t length) {
        *out_valid = true;

        size_t i = 0;
        while (i < length && (line[i] == ' ' || line[i] == '\t')) { ++i; }
        if (i == length || line[i] == '#' || line[i] == '\r') {
            return false;
        }
        if (i + 1 < length && line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
            i += 2;
        }

        u64 value = 0;
        size_t digits = 0;
        for (; i < length; ++i, ++digits) {
            const char c = line[i];
            u64 digit;
            if      ('0' <= c && c <= '9') { digit = c - '0'; }
            else if ('a' <= c && c <= 'f') { digit = c - 'a' + 10; }
            else if ('A' <= c && c <= 'F') { digit = c - 'A' + 10; }
            else { break; }
            value = (value << 4) | digit;
        }

        /* Anything after the number must be white space or a comment. */
        while (i < length && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) { ++i; }
        if (digits == 0 || digits > 16 || (i < length && line[i] != '#')) {
            *out_valid = false;
        }

        *out = value;
        return true;
    }

}

extern "C" {
    u32 __nx_applet_type = AppletType_None;

    // libnx and newlib allocate small thread and runtime records from here.
    void __libnx_initheap() {
        alignas(16) static unsigned char heap[256 * 1024];
        extern char *fake_heap_start;
        extern char *fake_heap_end;
        fake_heap_start = reinterpret_cast<char *>(heap);
        fake_heap_end = reinterpret_cast<char *>(heap + sizeof(heap));
    }

    void __appInit() {
        const ::Result rc = ::smInitialize();
        if (rc != 0) ::diagAbortWithResult(rc);
    }

    void __appExit() { ::smExit(); }

    void __libnx_exit(int) {
        // A KIP is an immortal process; reaching exit signals a fatal error.
        ::diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
        __builtin_unreachable();
    }
}

int main(int, char **) {
    size_t size = 0;
    if (const ::Result rc = ReadList(&size); rc != 0) {
        Log("[launcher] cannot read SYSTEM:/boot.lst (rc=0x%x); nothing started\n", rc);
    } else if (size == 0) {
        Log("[launcher] no SYSTEM:/boot.lst; nothing to start\n");
    } else if (const ::Result rc = ::pmshellInitialize(); rc != 0) {
        Log("[launcher] cannot reach pm:shell (rc=0x%x); nothing started\n", rc);
    } else {
        int started = 0, failed = 0;

        for (size_t start = 0; start < size; /* ... */) {
            size_t end = start;
            while (end < size && g_list[end] != '\n') { ++end; }

            u64 program_id;
            bool valid;
            if (ParseLine(&program_id, &valid, g_list + start, end - start)) {
                if (!valid) {
                    Log("[launcher] boot.lst: cannot understand \"%.*s\"; skipped\n", static_cast<int>(end - start), g_list + start);
                    ++failed;
                } else {
                    const ::NcmProgramLocation location = { .program_id = program_id, .storageID = NcmStorageId_BuiltInSystem };
                    u64 process_id = 0;
                    const ::Result rc = ::pmshellLaunchProgram(0, &location, &process_id);
                    if (rc == 0) {
                        Log("[launcher] started %016lx as process %lu\n", program_id, process_id);
                        ++started;
                    } else {
                        Log("[launcher] could not start %016lx (rc=0x%x)\n", program_id, rc);
                        ++failed;
                    }
                }
            }

            start = end + 1;
        }

        Log("[launcher] done: %d started, %d failed\n", started, failed);
    }

    /* Nothing left to do. This KIP is immortal, so it idles. */
    while (true) {
        ::svcSleepThread(3600'000'000'000ULL); /* 1 hour in nanoseconds */
    }
}
