/* Native client for content storage, code and system save data on a scratch disk. */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/result.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
extern "C" {
#include <switch/services/fsldr.h>
}

namespace {
constexpr u64 Program = 0x0100000000001000, Damaged = 0x0100000000001001, Missing = 0x0100000000001fff;
constexpr u64 Save = 0x8000000000004242;
constexpr size_t MainSize = 300000;
constexpr FsSaveDataSpaceId Space = FsSaveDataSpaceId_System;
char buffer[MainSize + 16];
int checks = 0, failures = 0;
void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *fmt, ...) {
    char text[256]; va_list args; va_start(args, fmt);
    int n = std::vsnprintf(text, sizeof(text), fmt, args); va_end(args);
    if (n > 0) svcOutputDebugString(text, std::min<size_t>(n, sizeof(text) - 1));
}
void Check(const char *what, bool ok, Result rc = 0) {
    ++checks; if (!ok) ++failures;
    Log("[content_fs_test] %s: %s (rc=0x%x)\n", ok ? "PASS" : "FAIL", what, rc);
}
const char *Path(const char *path) {
    static char paths[2][FS_MAX_PATH]; static unsigned next = 0;
    char *out = paths[next++ % 2]; std::snprintf(out, FS_MAX_PATH, "%s", path); return out;
}
Result Read(FsFileSystem *fs, const char *path, size_t *size) {
    FsFile f; KUMO_TRY(fsFsOpenFile(fs, Path(path), FsOpenMode_Read, &f));
    kumo::ScopeExit close([&] { fsFileClose(&f); });
    s64 len = 0; KUMO_TRY(fsFileGetSize(&f, &len));
    if (len < 0 || static_cast<u64>(len) > sizeof(buffer)) return MAKERESULT(2, 6063);
    u64 read = 0; KUMO_TRY(fsFileRead(&f, 0, buffer, len, FsReadOption_None, &read));
    *size = read; return 0;
}
Result Write(FsFileSystem *fs, const char *path, const char *data, size_t size) {
    KUMO_TRY(fsFsCreateFile(fs, Path(path), size, 0));
    FsFile f; KUMO_TRY(fsFsOpenFile(fs, Path(path), FsOpenMode_Write, &f));
    kumo::ScopeExit close([&] { fsFileClose(&f); });
    return fsFileWrite(&f, 0, data, size, FsWriteOption_Flush);
}
Result OpenSave(FsFileSystem *out) { return fsOpen_SystemSaveData(out, Space, Save, {}); }
void TestStorage() {
    FsFileSystem fs{}; Result rc = fsOpenContentStorageFileSystem(&fs, FsContentStorageId_System);
    Check("open system content storage", R_SUCCEEDED(rc), rc);
    if (R_SUCCEEDED(rc)) {
        FsDir dir{}; rc = fsFsOpenDirectory(&fs, Path("/registered"), FsDirOpenMode_ReadFiles, &dir);
        s64 total = 0; if (R_SUCCEEDED(rc)) { FsDirectoryEntry entries[8]; rc = fsDirRead(&dir, &total, 8, entries); fsDirClose(&dir); }
        Check("list four installed NCAs", R_SUCCEEDED(rc) && total == 4, rc); fsFsClose(&fs);
    }
    rc = fsOpenContentStorageFileSystem(&fs, FsContentStorageId_User);
    Check("open user content storage", R_SUCCEEDED(rc), rc); if (R_SUCCEEDED(rc)) fsFsClose(&fs);
    rc = fsOpenContentStorageFileSystem(&fs, FsContentStorageId_System0);
    Check("System0 content storage is absent", R_FAILED(rc), rc); if (R_SUCCEEDED(rc)) fsFsClose(&fs);
}
void TestCode() {
    FsFileSystem code{}; FsCodeInfo info{};
    Result rc = fsldrOpenCodeFileSystem(&info, Program, NcmStorageId_BuiltInSystem, Path("/"), FsContentAttributes_None, &code);
    Check("open program code found by FS", R_SUCCEEDED(rc), rc);
    if (R_SUCCEEDED(rc)) {
        size_t size = 0; rc = Read(&code, "/main", &size);
        bool match = R_SUCCEEDED(rc) && size == MainSize;
        for (size_t i = 0; match && i < size; ++i) match = static_cast<u8>(buffer[i]) == static_cast<u8>(i * 7 + 3);
        Check("program main reads back intact", match, rc);
        rc = Read(&code, "/main.npdm", &size);
        Check("program NPDM reads back intact", R_SUCCEEDED(rc) && size == 9 && std::memcmp(buffer, "fake npdm", 9) == 0, rc);
        FsDir dir{}; rc = fsFsOpenDirectory(&code, Path("/"), FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir);
        s64 count = 0; if (R_SUCCEEDED(rc)) { FsDirectoryEntry entries[8]; rc = fsDirRead(&dir, &count, 8, entries); fsDirClose(&dir); }
        Check("code filesystem lists three files", R_SUCCEEDED(rc) && count == 3, rc);
        rc = fsFsCreateFile(&code, Path("/new"), 16, 0); Check("code filesystem is read-only", R_FAILED(rc), rc);
        fsFsClose(&code);
    }
    rc = fsldrOpenCodeFileSystem(&info, Missing, NcmStorageId_BuiltInSystem, Path("/"), FsContentAttributes_None, &code);
    Check("missing program is not found", R_FAILED(rc), rc); if (R_SUCCEEDED(rc)) fsFsClose(&code);
    rc = fsldrOpenCodeFileSystem(&info, Damaged, NcmStorageId_BuiltInSystem, Path("/"), FsContentAttributes_None, &code);
    if (R_SUCCEEDED(rc)) { size_t size = 0; rc = Read(&code, "/main", &size); fsFsClose(&code); }
    Check("damaged NCA fails hash verification", R_FAILED(rc), rc);
}
void TestSave() {
    constexpr char data[] = "saved"; FsFileSystem fs{}; Result rc = OpenSave(&fs);
    Check("missing save is not found", kumo::IsResult(rc, MAKERESULT(2, 1002)), rc);
    if (R_SUCCEEDED(rc)) fsFsClose(&fs);
    rc = fsCreate_SystemSaveData(Space, Save, 0x10000, 0x10000, 0); Check("create system save", R_SUCCEEDED(rc), rc);
    rc = fsCreate_SystemSaveData(Space, Save, 0x10000, 0x10000, 0); Check("duplicate save is refused", kumo::IsResult(rc, MAKERESULT(2, 2)), rc);
    rc = OpenSave(&fs); Check("open system save", R_SUCCEEDED(rc), rc); if (R_FAILED(rc)) return;
    FsFileSystem second{}; rc = OpenSave(&second); Check("second save mount is refused", R_FAILED(rc), rc); if (R_SUCCEEDED(rc)) fsFsClose(&second);
    rc = Write(&fs, "/lost.txt", data, sizeof(data)); Check("write without commit", R_SUCCEEDED(rc), rc); fsFsClose(&fs);
    rc = OpenSave(&fs); FsDirEntryType type{}; Result probe = R_SUCCEEDED(rc) ? fsFsGetEntryType(&fs, Path("/lost.txt"), &type) : rc;
    Check("uncommitted file is gone", R_SUCCEEDED(rc) && kumo::IsResult(probe, MAKERESULT(2, 1)), probe);
    rc = fsFsCreateDirectory(&fs, Path("/dir")); if (R_SUCCEEDED(rc)) rc = Write(&fs, "/dir/kept.txt", data, sizeof(data)); if (R_SUCCEEDED(rc)) rc = fsFsCommit(&fs);
    Check("write and commit", R_SUCCEEDED(rc), rc); fsFsClose(&fs);
    rc = OpenSave(&fs); size_t size = 0; if (R_SUCCEEDED(rc)) rc = Read(&fs, "/dir/kept.txt", &size);
    Check("committed file persists", R_SUCCEEDED(rc) && size == sizeof(data) && std::memcmp(buffer, data, size) == 0, rc);
    rc = fsDeleteSaveDataFileSystemBySaveDataSpaceId(Space, Save); Check("deleting mounted save is refused", R_FAILED(rc), rc); fsFsClose(&fs);
    rc = OpenSave(&fs); if (R_SUCCEEDED(rc)) { FsFile f{}; rc = fsFsOpenFile(&fs, Path("/dir/kept.txt"), FsOpenMode_Write, &f); if (R_SUCCEEDED(rc)) { rc = fsFsCommit(&fs); fsFileClose(&f); } fsFsClose(&fs); }
    Check("commit refuses an open writable file", kumo::IsResult(rc, MAKERESULT(2, 6457)), rc);
    FsFileSystem system{}; rc = fsOpenBisFileSystem(&system, FsBisPartitionId_System, Path(""));
    if (R_SUCCEEDED(rc)) { rc = fsFsRenameDirectory(&system, Path("/save/8000000000004242/0"), Path("/save/8000000000004242/_")); fsFsClose(&system); }
    Check("stage interrupted commit", R_SUCCEEDED(rc), rc);
    if (R_SUCCEEDED(rc)) { rc = OpenSave(&fs); if (R_SUCCEEDED(rc)) { rc = Read(&fs, "/dir/kept.txt", &size); fsFsClose(&fs); } Check("mount repairs interrupted commit", R_SUCCEEDED(rc) && size == sizeof(data) && std::memcmp(buffer, data, size) == 0, rc); }
    FsSaveDataExtraData extra{}; rc = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), Space, Save);
    Check("read save flags", R_SUCCEEDED(rc) && extra.flags == 0, rc);
    extra.flags = FsSaveDataFlags_KeepAfterResettingSystemSaveData; rc = fsWriteSaveDataFileSystemExtraData(&extra, sizeof(extra), Space, Save);
    FsSaveDataExtraData again{}; Result rc2 = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&again, sizeof(again), Space, Save);
    Check("change and read save flags", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && again.flags == extra.flags, rc);
    rc = fsDeleteSaveDataFileSystemBySaveDataSpaceId(Space, Save); Check("delete save", R_SUCCEEDED(rc), rc);
    rc = OpenSave(&fs); Check("deleted save is not found", kumo::IsResult(rc, MAKERESULT(2, 1002)), rc); if (R_SUCCEEDED(rc)) fsFsClose(&fs);
}
}
extern "C" {
u32 __nx_applet_type = AppletType_None;
void __libnx_initheap() { alignas(16) static char heap[1024 * 1024]; extern char *fake_heap_start, *fake_heap_end; fake_heap_start = heap; fake_heap_end = heap + sizeof(heap); }
void __appInit() { SecmonArgs config{{0xC3000002, 65011}}; svcCallSecureMonitor(&config); KUMO_ASSERT(config.X[0] == 0 && config.X[1] != 0); hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8)); KUMO_ABORT_RESULT(smInitialize()); }
void __appExit() { smExit(); }
void __libnx_exit(int) { for (;;) svcSleepThread(1000000000); }
}
int main(int, char **) {
    Log("[content_fs_test] starting\n");
    Result rc = fsInitialize(); Check("initialize FS", R_SUCCEEDED(rc), rc);
    if (R_SUCCEEDED(rc)) { TestStorage(); rc = fsldrInitialize(); Check("initialize loader FS", R_SUCCEEDED(rc), rc); if (R_SUCCEEDED(rc)) { TestCode(); fsldrExit(); } TestSave(); }
    Log("[content_fs_test] done: %d checks, %d failure(s)\n", checks, failures);
    for (;;) svcSleepThread(60000000000LL);
}
