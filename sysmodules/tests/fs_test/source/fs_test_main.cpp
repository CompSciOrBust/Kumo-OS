/*
 * Test client for the FS system module. Runs as a KIP, exercises fsp-srv
 * through libnx's fs.h client, and reports each check on the kernel debug log.
 *
 * It writes to the system storage: to the PRODINFO partition (or, on a disk
 * with no partition table, to the raw disk) and to the SDCARD partition, which
 * it formats. Run it against scratch disks only.
 */
#include <switch.h>
#include <kumo/assert.hpp>
#include <kumo/result.hpp>
#include <kumo/scope_exit.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>

namespace {

    namespace {

        constexpr u32 FsResultModule = 2;

        constinit int g_failures = 0;
        constinit int g_checks   = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x100];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = std::vsnprintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        void Check(const char *what, bool ok, Result rc = 0) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[fs_test] %s: %s (rc=0x%x)\n", ok ? "PASS" : "FAIL", what, rc);
        }

        /* libnx sends FS_MAX_PATH bytes for every path argument, so paths must live in buffers that large. */
        const char *Path(const char *path) {
            static char buffers[2][FS_MAX_PATH];
            static int next = 0;

            char *buffer = buffers[next];
            next ^= 1;
            std::memset(buffer, 0, FS_MAX_PATH);
            std::snprintf(buffer, FS_MAX_PATH, "%s", path);
            return buffer;
        }

        void FillPattern(u8 *dst, size_t size, u32 seed) {
            for (size_t i = 0; i < size; ++i) {
                dst[i] = static_cast<u8>((i * 31 + seed) ^ (i >> 8));
            }
        }

        constinit u8 g_out[(1024 * 1024)] = {};
        constinit u8 g_in[(1024 * 1024)]  = {};

        /* ===== fsp-srv itself ===== */

        bool TestService() {
            Result rc = ::fsInitialize();
            Check("fsInitialize (connect + SetCurrentProcess)", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return false;
            }

            bool exfat = false;
            rc = ::fsIsExFatSupported(std::addressof(exfat));
            Check("IsExFatSupported returns true", R_SUCCEEDED(rc) && exfat, rc);

            /* A command FS does not implement must answer NotImplemented rather than crash it. */
            ::FsFileSystem fs;
            rc = ::fsOpenImageDirectoryFileSystem(std::addressof(fs), FsImageDirectoryId_Sd);
            Check("an unimplemented command answers fs::ResultNotImplemented", kumo::IsResult(rc, MAKERESULT(FsResultModule, 3001)), rc);

            ::Service separate;
            rc = ::smGetService(&separate, "fsp-srv");
            Check("open a second fsp-srv session", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                rc = ::serviceDispatch(&separate, 0x7fff);
                Check("an unknown command closes only its session", kumo::IsResult(rc, MAKERESULT(Module_Kernel, 123)), rc);
                ::serviceClose(&separate);
                rc = ::fsIsExFatSupported(&exfat);
                Check("the original fsp-srv session survives", R_SUCCEEDED(rc) && exfat, rc);
            }

            return true;
        }

        /* ===== Raw storage ===== */

        void TestStorage() {
            /* Use the PRODINFO partition as scratch space; fall back to the whole disk if there is no partition table. */
            ::FsStorage storage;
            Result rc = ::fsOpenBisStorage(std::addressof(storage), FsBisPartitionId_CalibrationBinary);
            const bool partitioned = R_SUCCEEDED(rc);
            if (!partitioned) {
                Log("[fs_test] no PRODINFO partition (rc=0x%x); using the whole disk\n", rc);
                rc = ::fsOpenBisStorage(std::addressof(storage), FsBisPartitionId_UserDataRoot);
            }
            Check("OpenBisStorage", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            s64 size = 0;
            rc = ::fsStorageGetSize(std::addressof(storage), std::addressof(size));
            Check("IStorage.GetSize is a positive multiple of 512", R_SUCCEEDED(rc) && size > 0 && (size % 512) == 0, rc);
            Log("[fs_test] storage size: %lld bytes\n", static_cast<long long>(size));

            /* Unaligned write spanning several blocks, then read back. */
            constexpr s64 Offset    = 0x1000 + 123;
            constexpr size_t Length = 3000;
            FillPattern(g_out, Length, 3);

            u8 before[2] = {};
            rc = ::fsStorageRead(std::addressof(storage), Offset - 1, before, 1);
            Result rc2 = ::fsStorageRead(std::addressof(storage), Offset + Length, before + 1, 1);
            Check("read neighbouring bytes", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2), rc);

            rc = ::fsStorageWrite(std::addressof(storage), Offset, g_out, Length);
            Check("IStorage.Write unaligned", R_SUCCEEDED(rc), rc);
            rc = ::fsStorageFlush(std::addressof(storage));
            Check("IStorage.Flush", R_SUCCEEDED(rc), rc);
            rc = ::fsStorageRead(std::addressof(storage), Offset, g_in, Length);
            Check("IStorage.Read returns what was written", R_SUCCEEDED(rc) && std::memcmp(g_in, g_out, Length) == 0, rc);

            u8 after[2] = {};
            rc  = ::fsStorageRead(std::addressof(storage), Offset - 1, after, 1);
            rc2 = ::fsStorageRead(std::addressof(storage), Offset + Length, after + 1, 1);
            Check("neighbouring bytes untouched", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && after[0] == before[0] && after[1] == before[1], rc);

            /* A transfer larger than one driver request (300 KiB), block-aligned. */
            constexpr size_t BigLength = 300 * 1024;
            FillPattern(g_out, BigLength, 7);
            rc = ::fsStorageWrite(std::addressof(storage), 0x100000, g_out, BigLength);
            Check("IStorage.Write 300 KiB", R_SUCCEEDED(rc), rc);
            rc = ::fsStorageRead(std::addressof(storage), 0x100000, g_in, BigLength);
            Check("IStorage.Read 300 KiB returns what was written", R_SUCCEEDED(rc) && std::memcmp(g_in, g_out, BigLength) == 0, rc);

            /* Out-of-range access must be rejected. */
            rc = ::fsStorageRead(std::addressof(storage), size - 4, g_in, 16);
            Check("read past end is rejected", R_FAILED(rc), rc);

            ::fsStorageClose(std::addressof(storage));

            if (partitioned) {
                /* The whole-disk view must be larger than one partition, and start with the protective MBR. */
                ::FsStorage disk;
                rc = ::fsOpenBisStorage(std::addressof(disk), FsBisPartitionId_UserDataRoot);
                s64 disk_size = 0;
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsStorageGetSize(std::addressof(disk), std::addressof(disk_size));
                }
                u8 sector[512] = {};
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsStorageRead(std::addressof(disk), 0, sector, sizeof(sector));
                }
                Check("UserDataRoot is the whole disk (protective MBR at sector 0)", R_SUCCEEDED(rc) && disk_size > size && sector[510] == 0x55 && sector[511] == 0xAA && sector[450] == 0xEE, rc);
                if (R_SUCCEEDED(rc)) {
                    ::fsStorageClose(std::addressof(disk));
                }

                /* A boot partition id has no partition here. */
                ::FsStorage boot;
                rc = ::fsOpenBisStorage(std::addressof(boot), FsBisPartitionId_BootPartition1Root);
                Check("OpenBisStorage(BootPartition1Root) answers PartitionNotFound", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1001)), rc);
            }
        }

        /* ===== A FAT filesystem ===== */

        Result WriteWholeFile(::FsFileSystem *fs, const char *path, const void *data, size_t size) {
            KUMO_TRY(::fsFsCreateFile(fs, Path(path), static_cast<s64>(size), 0));

            ::FsFile file;
            KUMO_TRY(::fsFsOpenFile(fs, Path(path), FsOpenMode_Write, std::addressof(file)));
            kumo::ScopeExit close_file([&] { ::fsFileClose(std::addressof(file)); });

            KUMO_RETURN(::fsFileWrite(std::addressof(file), 0, data, size, FsWriteOption_Flush));
        }

        Result ReadWholeFile(size_t *out_size, ::FsFileSystem *fs, const char *path, void *buffer, size_t buffer_size) {
            ::FsFile file;
            KUMO_TRY(::fsFsOpenFile(fs, Path(path), FsOpenMode_Read, std::addressof(file)));
            kumo::ScopeExit close_file([&] { ::fsFileClose(std::addressof(file)); });

            u64 read = 0;
            KUMO_TRY(::fsFileRead(std::addressof(file), 0, buffer, buffer_size, FsReadOption_None, std::addressof(read)));
            *out_size = read;
            KUMO_SUCCEED();
        }

        /* Count a directory's entries, and whether one called `name` of the given type is among them. */
        Result FindInDirectory(s64 *out_count, bool *out_found, ::FsFileSystem *fs, const char *path, const char *name, s8 type) {
            ::FsDir dir;
            KUMO_TRY(::fsFsOpenDirectory(fs, Path(path), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, std::addressof(dir)));
            kumo::ScopeExit close_dir([&] { ::fsDirClose(std::addressof(dir)); });

            static ::FsDirectoryEntry entries[8];
            *out_count = 0;
            *out_found = false;
            while (true) {
                s64 count = 0;
                KUMO_TRY(::fsDirRead(std::addressof(dir), std::addressof(count), std::size(entries), entries));
                if (count == 0) {
                    break;
                }
                for (s64 i = 0; i < count; ++i) {
                    if (std::strcmp(entries[i].name, name) == 0 && entries[i].type == type) {
                        *out_found = true;
                    }
                }
                *out_count += count;
            }
            KUMO_SUCCEED();
        }

        void TestFileSystem(::FsFileSystem *fs) {
            Result rc;

            ::FsDirEntryType type;
            rc = ::fsFsGetEntryType(fs, Path("/"), std::addressof(type));
            Check("GetEntryType(/) is a directory", R_SUCCEEDED(rc) && type == FsDirEntryType_Dir, rc);

            s64 total_space = 0, free_before = 0;
            rc = ::fsFsGetTotalSpace(fs, Path("/"), std::addressof(total_space));
            Result rc2 = ::fsFsGetFreeSpace(fs, Path("/"), std::addressof(free_before));
            Check("GetTotalSpaceSize and GetFreeSpaceSize", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && 0 < free_before && free_before <= total_space, rc);
            Log("[fs_test] volume: %lld bytes total, %lld free\n", static_cast<long long>(total_space), static_cast<long long>(free_before));

            /* Directories. */
            rc = ::fsFsCreateDirectory(fs, Path("/test"));
            Check("CreateDirectory /test", R_SUCCEEDED(rc), rc);
            rc = ::fsFsCreateDirectory(fs, Path("/test/sub"));
            Check("CreateDirectory /test/sub", R_SUCCEEDED(rc), rc);
            rc = ::fsFsCreateDirectory(fs, Path("/test"));
            Check("CreateDirectory on an existing path answers PathAlreadyExists", kumo::IsResult(rc, MAKERESULT(FsResultModule, 2)), rc);
            rc = ::fsFsCreateDirectory(fs, Path("/missing/sub"));
            Check("CreateDirectory under a missing parent answers PathNotFound", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1)), rc);

            /* A small file, grown by appending. */
            constexpr const char Greeting[] = "Hello from Horizon\n";
            constexpr size_t GreetingLength = sizeof(Greeting) - 1;

            rc = ::fsFsCreateFile(fs, Path("/test/hello.txt"), 0, 0);
            Check("CreateFile /test/hello.txt", R_SUCCEEDED(rc), rc);
            rc = ::fsFsCreateFile(fs, Path("/test/hello.txt"), 0, 0);
            Check("CreateFile on an existing path answers PathAlreadyExists", kumo::IsResult(rc, MAKERESULT(FsResultModule, 2)), rc);

            {
                ::FsFile file;
                rc = ::fsFsOpenFile(fs, Path("/test/hello.txt"), FsOpenMode_Write, std::addressof(file));
                Check("OpenFile for writing", R_SUCCEEDED(rc), rc);
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsFileWrite(std::addressof(file), 0, Greeting, GreetingLength, FsWriteOption_None);
                    Check("Write past the end without Append answers FileExtensionWithoutOpenModeAllowAppend", kumo::IsResult(rc, MAKERESULT(FsResultModule, 6201)), rc);
                    ::fsFileClose(std::addressof(file));
                }

                rc = ::fsFsOpenFile(fs, Path("/test/hello.txt"), FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append, std::addressof(file));
                Check("OpenFile for read, write and append", R_SUCCEEDED(rc), rc);
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsFileWrite(std::addressof(file), 0, Greeting, GreetingLength, FsWriteOption_Flush);
                    Check("IFile.Write with append", R_SUCCEEDED(rc), rc);

                    s64 size = -1;
                    rc = ::fsFileGetSize(std::addressof(file), std::addressof(size));
                    Check("IFile.GetSize matches what was written", R_SUCCEEDED(rc) && size == static_cast<s64>(GreetingLength), rc);

                    u64 read = 0;
                    std::memset(g_in, 0, 64);
                    rc = ::fsFileRead(std::addressof(file), 0, g_in, 64, FsReadOption_None, std::addressof(read));
                    Check("IFile.Read stops at the end of the file", R_SUCCEEDED(rc) && read == GreetingLength && std::memcmp(g_in, Greeting, GreetingLength) == 0, rc);

                    rc = ::fsFileRead(std::addressof(file), 6, g_in, 4, FsReadOption_None, std::addressof(read));
                    Check("IFile.Read at an offset", R_SUCCEEDED(rc) && read == 4 && std::memcmp(g_in, "from", 4) == 0, rc);

                    rc = ::fsFileSetSize(std::addressof(file), 5);
                    rc2 = ::fsFileGetSize(std::addressof(file), std::addressof(size));
                    Check("IFile.SetSize shrinks the file", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && size == 5, rc);

                    rc = ::fsFileSetSize(std::addressof(file), 4096);
                    rc2 = ::fsFileGetSize(std::addressof(file), std::addressof(size));
                    Check("IFile.SetSize grows the file", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && size == 4096, rc);

                    /* Leave the greeting in place for the host to read back. */
                    rc  = ::fsFileSetSize(std::addressof(file), 0);
                    rc2 = ::fsFileWrite(std::addressof(file), 0, Greeting, GreetingLength, FsWriteOption_Flush);
                    Check("rewrite the file", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2), R_FAILED(rc) ? rc : rc2);

                    /* The file is open, so it cannot be deleted. */
                    rc = ::fsFsDeleteFile(fs, Path("/test/hello.txt"));
                    Check("DeleteFile on an open file answers TargetLocked", kumo::IsResult(rc, MAKERESULT(FsResultModule, 7)), rc);

                    ::fsFileClose(std::addressof(file));
                }

                rc = ::fsFsOpenFile(fs, Path("/test/hello.txt"), FsOpenMode_Read, std::addressof(file));
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsFileWrite(std::addressof(file), 0, Greeting, 1, FsWriteOption_None);
                    Check("Write to a file opened read-only answers WriteNotPermitted", kumo::IsResult(rc, MAKERESULT(FsResultModule, 6203)), rc);
                    ::fsFileClose(std::addressof(file));
                } else {
                    Check("OpenFile read-only", false, rc);
                }
            }

            /* A 1 MiB file, created at full size. */
            FillPattern(g_out, sizeof(g_out), 11);
            rc = WriteWholeFile(fs, "/test/sub/data.bin", g_out, sizeof(g_out));
            Check("create and write a 1 MiB file", R_SUCCEEDED(rc), rc);

            size_t read_size = 0;
            std::memset(g_in, 0, sizeof(g_in));
            rc = ReadWholeFile(std::addressof(read_size), fs, "/test/sub/data.bin", g_in, sizeof(g_in));
            Check("read the 1 MiB file back", R_SUCCEEDED(rc) && read_size == sizeof(g_out) && std::memcmp(g_in, g_out, sizeof(g_out)) == 0, rc);

            s64 free_after = 0;
            rc = ::fsFsGetFreeSpace(fs, Path("/"), std::addressof(free_after));
            Check("free space fell by at least 1 MiB", R_SUCCEEDED(rc) && free_after <= free_before - static_cast<s64>(sizeof(g_out)), rc);

            /* A long name with non-ASCII characters. */
            constexpr const char LongName[] = "/test/A long file name with spaces, UTF-8 (\xC3\xA9\xC3\xB1\xE6\x97\xA5\xE6\x9C\xAC) and more than eight characters.txt";
            rc = WriteWholeFile(fs, LongName, "x", 1);
            Check("create a file with a long UTF-8 name", R_SUCCEEDED(rc), rc);

            s64 count = 0;
            bool found = false;
            rc = FindInDirectory(std::addressof(count), std::addressof(found), fs, "/test", LongName + 6, FsDirEntryType_File);
            Check("IDirectory.Read lists /test (3 entries, long name intact)", R_SUCCEEDED(rc) && count == 3 && found, rc);

            {
                ::FsDir dir;
                rc = ::fsFsOpenDirectory(fs, Path("/test"), FsDirOpenMode_ReadDirs, std::addressof(dir));
                s64 dir_count = -1;
                if (R_SUCCEEDED(rc)) {
                    rc = ::fsDirGetEntryCount(std::addressof(dir), std::addressof(dir_count));
                    ::fsDirClose(std::addressof(dir));
                }
                Check("IDirectory.GetEntryCount counts only directories when asked", R_SUCCEEDED(rc) && dir_count == 1, rc);
            }

            /* Renames. */
            rc = ::fsFsRenameFile(fs, Path("/test/hello.txt"), Path("/test/greeting.txt"));
            Check("RenameFile", R_SUCCEEDED(rc), rc);
            rc = ::fsFsGetEntryType(fs, Path("/test/hello.txt"), std::addressof(type));
            Check("the old name is gone (PathNotFound)", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1)), rc);
            rc = ::fsFsGetEntryType(fs, Path("/test/greeting.txt"), std::addressof(type));
            Check("the new name is a file", R_SUCCEEDED(rc) && type == FsDirEntryType_File, rc);
            rc = ::fsFsRenameFile(fs, Path("/test/sub"), Path("/test/sub2"));
            Check("RenameFile on a directory answers PathNotFound", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1)), rc);
            rc = ::fsFsRenameDirectory(fs, Path("/test/sub"), Path("/test/moved"));
            Check("RenameDirectory", R_SUCCEEDED(rc), rc);
            rc = ::fsFsRenameFile(fs, Path(LongName), Path("/test/greeting.txt"));
            Check("RenameFile onto an existing file answers PathAlreadyExists", kumo::IsResult(rc, MAKERESULT(FsResultModule, 2)), rc);

            /* Deletes. */
            rc = ::fsFsDeleteDirectory(fs, Path("/test/moved"));
            Check("DeleteDirectory on a non-empty directory answers DirectoryNotEmpty", kumo::IsResult(rc, MAKERESULT(FsResultModule, 8)), rc);
            rc = ::fsFsDeleteFile(fs, Path("/test/moved"));
            Check("DeleteFile on a directory answers PathNotFound", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1)), rc);
            rc = ::fsFsDeleteFile(fs, Path(LongName));
            Check("DeleteFile", R_SUCCEEDED(rc), rc);
            rc = ::fsFsOpenFile(fs, Path(LongName), FsOpenMode_Read, nullptr);
            Check("OpenFile on a deleted file answers PathNotFound", kumo::IsResult(rc, MAKERESULT(FsResultModule, 1)), rc);

            /* A tree to delete recursively: /tree/a/b/c with files at several levels. */
            rc = ::fsFsCreateDirectory(fs, Path("/tree"));
            if (R_SUCCEEDED(rc)) { rc = ::fsFsCreateDirectory(fs, Path("/tree/a")); }
            if (R_SUCCEEDED(rc)) { rc = ::fsFsCreateDirectory(fs, Path("/tree/a/b")); }
            if (R_SUCCEEDED(rc)) { rc = ::fsFsCreateDirectory(fs, Path("/tree/a/b/c")); }
            if (R_SUCCEEDED(rc)) { rc = ::fsFsCreateDirectory(fs, Path("/tree/empty")); }
            if (R_SUCCEEDED(rc)) { rc = WriteWholeFile(fs, "/tree/one.txt", "1", 1); }
            if (R_SUCCEEDED(rc)) { rc = WriteWholeFile(fs, "/tree/a/two.txt", "2", 1); }
            if (R_SUCCEEDED(rc)) { rc = WriteWholeFile(fs, "/tree/a/b/c/three.txt", "3", 1); }
            Check("build a directory tree", R_SUCCEEDED(rc), rc);

            rc = ::fsFsCleanDirectoryRecursively(fs, Path("/tree/a"));
            rc2 = FindInDirectory(std::addressof(count), std::addressof(found), fs, "/tree/a", "", 0);
            Check("CleanDirectoryRecursively empties a directory but keeps it", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && count == 0, R_FAILED(rc) ? rc : rc2);

            rc = ::fsFsDeleteDirectoryRecursively(fs, Path("/tree"));
            rc2 = ::fsFsGetEntryType(fs, Path("/tree"), std::addressof(type));
            Check("DeleteDirectoryRecursively removes the tree", R_SUCCEEDED(rc) && kumo::IsResult(rc2, MAKERESULT(FsResultModule, 1)), R_FAILED(rc) ? rc : rc2);

            rc = ::fsFsCommit(fs);
            Check("Commit", R_SUCCEEDED(rc), rc);
        }

        void TestSdCard() {
            /* The device operator reports the card as inserted exactly when there is an SDCARD partition. */
            {
                ::FsDeviceOperator op;
                Result rc = ::fsOpenDeviceOperator(std::addressof(op));
                bool inserted = false, game_card = true;
                Result rc2 = R_SUCCEEDED(rc) ? ::fsDeviceOperatorIsSdCardInserted(std::addressof(op), std::addressof(inserted)) : rc;
                Result rc3 = R_SUCCEEDED(rc) ? ::fsDeviceOperatorIsGameCardInserted(std::addressof(op), std::addressof(game_card)) : rc;
                Check("IDeviceOperator answers IsSdCardInserted and IsGameCardInserted", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2) && R_SUCCEEDED(rc3) && !game_card, R_FAILED(rc) ? rc : rc2);

                /* FormatSdCardDryRun (fsp-srv command 26, no libnx wrapper) fails with SdCardNotPresent when there is none. */
                const Result dry_run = ::serviceDispatch(::fsGetServiceSession(), 26);
                Check("IsSdCardInserted agrees with FormatSdCardDryRun", R_SUCCEEDED(rc2) && inserted == R_SUCCEEDED(dry_run) && (R_SUCCEEDED(dry_run) || kumo::IsResult(dry_run, MAKERESULT(FsResultModule, 2001))), dry_run);
                if (R_SUCCEEDED(rc)) {
                    ::fsDeviceOperatorClose(std::addressof(op));
                }
            }

            /* libnx has no wrapper for FormatSdCardFileSystem, which is fsp-srv command 19. */
            Result rc = ::serviceDispatch(::fsGetServiceSession(), 19);
            if (kumo::IsResult(rc, MAKERESULT(FsResultModule, 2001))) {
                Log("[fs_test] no SDCARD partition; skipping the filesystem tests\n");
                return;
            }
            Check("FormatSdCardFileSystem", R_SUCCEEDED(rc), rc);

            ::FsFileSystem sd;
            rc = ::fsOpenSdCardFileSystem(std::addressof(sd));
            Check("OpenSdCardFileSystem", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            TestFileSystem(std::addressof(sd));

            /* Two sessions on the same volume see each other's changes. */
            ::FsFileSystem sd2;
            rc = ::fsOpenSdCardFileSystem(std::addressof(sd2));
            ::FsDirEntryType type;
            Result rc2 = R_SUCCEEDED(rc) ? ::fsFsGetEntryType(std::addressof(sd2), Path("/test/greeting.txt"), std::addressof(type)) : rc;
            Check("a second OpenSdCardFileSystem session sees the same files", R_SUCCEEDED(rc) && R_SUCCEEDED(rc2), R_FAILED(rc) ? rc : rc2);
            if (R_SUCCEEDED(rc)) {
                ::fsFsClose(std::addressof(sd2));
            }

            ::fsFsClose(std::addressof(sd));
        }

        void TestSystemPartition() {
            ::FsFileSystem system;
            Result rc = ::fsOpenBisFileSystem(std::addressof(system), FsBisPartitionId_System, Path(""));
            if (kumo::IsResult(rc, MAKERESULT(FsResultModule, 1001))) {
                Log("[fs_test] no SYSTEM partition; skipping\n");
                return;
            }
            Check("OpenBisFileSystem(System)", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            /* The test image's SYSTEM partition is filled on the host. */
            size_t size = 0;
            std::memset(g_in, 0, 64);
            rc = ReadWholeFile(std::addressof(size), std::addressof(system), "/hostfile.txt", g_in, 64);
            constexpr const char Expected[] = "written on the host\n";
            Check("read a file the host wrote to SYSTEM", R_SUCCEEDED(rc) && size == sizeof(Expected) - 1 && std::memcmp(g_in, Expected, size) == 0, rc);

            rc = ReadWholeFile(std::addressof(size), std::addressof(system), "/etc/nested.txt", g_in, 64);
            Check("read a file in a subdirectory of SYSTEM", R_SUCCEEDED(rc) && size == 7 && std::memcmp(g_in, "nested\n", 7) == 0, rc);

            ::fsFsClose(std::addressof(system));

            /* The BIS subdirectory view must hold a separate root and reject traversal. */
            ::FsFileSystem confined;
            rc = ::fsOpenBisFileSystem(&confined, FsBisPartitionId_System, Path("/etc"));
            Check("OpenBisFileSystem with a root directory", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                rc = ReadWholeFile(&size, &confined, "/nested.txt", g_in, 64);
                Check("BIS view resolves paths relative to its subdirectory",
                      R_SUCCEEDED(rc) && size == 7 && std::memcmp(g_in, "nested\n", 7) == 0, rc);
                ::FsDirEntryType type;
                rc = ::fsFsGetEntryType(&confined, Path("/../hostfile.txt"), &type);
                Check("BIS view rejects upward traversal", kumo::IsResult(rc, MAKERESULT(FsResultModule, 6005)), rc);
                rc = ::fsFsGetEntryType(&confined, Path("/.. /hostfile.txt"), &type);
                Check("BIS view rejects FatFs trailing-space aliases", kumo::IsResult(rc, MAKERESULT(FsResultModule, 6005)), rc);

                rc = ::fsFsQueryEntry(&confined, g_in, 13, g_out, 7, Path("/nested.txt"),
                                      FsFileSystemQueryId_SetConcatenationFileAttribute);
                Check("QueryEntry decodes its fixed path and two NonSecure descriptors",
                      kumo::IsResult(rc, MAKERESULT(FsResultModule, 3001)), rc);

                ::FsFile child;
                rc = ::fsFsOpenFile(&confined, Path("/nested.txt"), FsOpenMode_Read, &child);
                ::fsFsClose(&confined);
                Check("open a child before closing its parent", R_SUCCEEDED(rc), rc);
                if (R_SUCCEEDED(rc)) {
                    u64 bytes = 0;
                    rc = ::fsFileRead(&child, 0, g_in, 64, FsReadOption_None, &bytes);
                    Check("file object survives close of its parent",
                          R_SUCCEEDED(rc) && bytes == 7 && std::memcmp(g_in, "nested\n", 7) == 0, rc);
                    ::FsRangeInfo info;
                    std::memset(&info, 0xa5, sizeof(info));
                    rc = ::fsFileOperateRange(&child, FsOperationId_QueryRange, 0, 7, &info);
                    const ::FsRangeInfo empty{};
                    Check("IFile.OperateRange writes a zeroed 0x40-byte range record",
                          R_SUCCEEDED(rc) && std::memcmp(&info, &empty, sizeof(info)) == 0, rc);
                    ::fsFileClose(&child);
                }
            }

            ::FsStorage partition;
            rc = ::fsOpenBisStorage(&partition, FsBisPartitionId_System);
            Check("OpenBisStorage(System) for range commands", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                ::FsRangeInfo info;
                std::memset(&info, 0xa5, sizeof(info));
                rc = ::fsStorageOperateRange(&partition, FsOperationId_QueryRange, 0, 512, &info);
                const ::FsRangeInfo empty{};
                Check("partition IStorage.OperateRange forwards raw-device range flags",
                      R_SUCCEEDED(rc) && std::memcmp(&info, &empty, sizeof(info)) == 0, rc);
                rc = ::fsStorageOperateRange(&partition, FsOperationId_QueryRange, -1, 512, &info);
                Check("partition IStorage.OperateRange preserves the negative-offset result",
                      kumo::IsResult(rc, MAKERESULT(FsResultModule, 6061)), rc);
                ::fsStorageClose(&partition);
            }

            /* Only some BIS partitions hold a filesystem. */
            ::FsFileSystem other;
            rc = ::fsOpenBisFileSystem(std::addressof(other), FsBisPartitionId_CalibrationBinary, Path(""));
            Check("OpenBisFileSystem(CalibrationBinary) is rejected", R_FAILED(rc), rc);
        }

    }

    void RunTest() {
        Log("[fs_test] starting\n");

        if (TestService()) {
            TestStorage();
            TestSdCard();
            TestSystemPartition();
        }

        Log("[fs_test] done: %d checks, %d failure(s)\n", g_checks, g_failures);

        /* Idle forever; this KIP is immortal. */
        while (true) {
            svcSleepThread(INT64_C(60000000000));
        }
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
    void __libnx_exit(int) { for (;;) svcSleepThread(1000000000); }
}
int main(int, char **) { RunTest(); return 0; }
