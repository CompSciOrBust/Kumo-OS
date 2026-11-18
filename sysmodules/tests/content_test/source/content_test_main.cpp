/* Atmosphere integration test for content metadata, NCM and the location resolver.
 * Uses a host-prepared scratch disk and the single fsp-ldr session.
 * Independent filesystem and save-data coverage lives in content_fs_test.
 */
#include <stratosphere.hpp>

namespace ams {

    namespace {

        /* What the host installs on SYSTEM. */
        constexpr ncm::ProgramId TestProgramId      = { 0x0100000000001000 };
        constexpr u32            TestProgramVersion = 3;
        constexpr size_t         TestMainSize       = 300000;
        constexpr const char     TestNpdm[]         = "fake npdm";

        /* A second program whose NCA the host damaged after building it. */
        constexpr ncm::ProgramId DamagedProgramId = { 0x0100000000001001 };

        constexpr ncm::ProgramId MissingProgramId = { 0x0100000000001fff };

        constexpr int InstalledContentCount = 4;

        constexpr fs::SystemSaveDataId NcmSaveDataId  = 0x8000000000000120;

        constinit int g_failures = 0;
        constinit int g_checks   = 0;

        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

        void Log(const char *fmt, ...) {
            char buf[0x180];
            std::va_list vl;
            va_start(vl, fmt);
            const int len = util::VSNPrintf(buf, sizeof(buf), fmt, vl);
            va_end(vl);
            ::svcOutputDebugString(buf, std::min<size_t>(len, sizeof(buf) - 1));
        }

        void Check(const char *what, bool ok, Result rc = ResultSuccess()) {
            ++g_checks;
            if (!ok) {
                ++g_failures;
            }
            Log("[content_test] %s: %s (rc=0x%x)\n", ok ? "PASS" : "FAIL", what, rc.GetValue());
        }

        /* ===== Heap ===== */

        constinit u8 g_heap_memory[4_MB];
        constinit lmem::HeapHandle g_heap_handle = nullptr;

        void *Allocate(size_t size) {
            return lmem::AllocateFromExpHeap(g_heap_handle, size);
        }

        void Deallocate(void *p, size_t size) {
            AMS_UNUSED(size);
            if (p != nullptr) {
                lmem::FreeToExpHeap(g_heap_handle, p);
            }
        }

        constinit u8 g_buffer[TestMainSize + 16];

        u8 MainByte(size_t i) {
            return static_cast<u8>(i * 7 + 3);
        }

        /* ===== Helpers ===== */

        Result ReadWholeFile(size_t *out_size, const char *path, void *dst, size_t dst_size) {
            fs::FileHandle file;
            R_TRY(fs::OpenFile(std::addressof(file), path, fs::OpenMode_Read));
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            s64 size;
            R_TRY(fs::GetFileSize(std::addressof(size), file));
            R_UNLESS(static_cast<size_t>(size) <= dst_size, fs::ResultInvalidSize());

            R_TRY(fs::ReadFile(file, 0, dst, static_cast<size_t>(size)));
            *out_size = static_cast<size_t>(size);
            R_SUCCEED();
        }

        template<typename F>
        Result ForEachEntry(const char *path, F f) {
            fs::DirectoryHandle dir;
            R_TRY(fs::OpenDirectory(std::addressof(dir), path, fs::OpenDirectoryMode_All));
            ON_SCOPE_EXIT { fs::CloseDirectory(dir); };

            while (true) {
                s64 count;
                static fs::DirectoryEntry entry;
                R_TRY(fs::ReadDirectory(std::addressof(count), std::addressof(entry), dir, 1));
                if (count == 0) {
                    break;
                }
                f(entry);
            }
            R_SUCCEED();
        }

        bool VerifyMain(const char *mount_name) {
            char path[0x40];
            util::SNPrintf(path, sizeof(path), "%s:/main", mount_name);

            size_t size = 0;
            if (R_FAILED(ReadWholeFile(std::addressof(size), path, g_buffer, sizeof(g_buffer))) || size != TestMainSize) {
                return false;
            }
            for (size_t i = 0; i < size; ++i) {
                if (g_buffer[i] != MainByte(i)) {
                    return false;
                }
            }
            return true;
        }

        /* ===== Phase 1: FS alone ===== */

        constinit ncm::ContentId g_program_content_id = {};
        constinit bool g_have_program_content_id = false;

        void TestContentStorage() {
            Result rc = fs::MountContentStorage("cs", fs::ContentStorageId::System);
            Check("mount the system content storage", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return;
            }

            /* Every installed NCA; and, by reading them as ncm does, which are metadata. */
            int nca_count = 0, meta_count = 0;
            bool found_program_meta = false;
            rc = ForEachEntry("cs:/registered", [&](const fs::DirectoryEntry &entry) {
                ++nca_count;

                char path[0x80];
                util::SNPrintf(path, sizeof(path), "@SystemContent://registered/%s", entry.name);

                ncm::AutoBuffer meta;
                if (R_FAILED(ncm::TryReadContentMetaPath(std::addressof(meta), path, ncm::ReadContentMetaPathWithoutExtendedDataOrDigest))) {
                    return;
                }
                ++meta_count;

                ncm::PackagedContentMetaReader reader(meta.Get(), meta.GetSize());
                const auto key = reader.GetKey();
                if (key.id == TestProgramId.value) {
                    found_program_meta = key.version == TestProgramVersion && key.type == ncm::ContentMetaType::SystemProgram
                                      && reader.GetContentCount() == 1 && reader.GetContentInfo(0)->info.GetType() == ncm::ContentType::Program;
                    if (found_program_meta) {
                        g_program_content_id      = reader.GetContentInfo(0)->info.GetId();
                        g_have_program_content_id = true;
                    }
                }
            });
            Check("list the installed content", R_SUCCEEDED(rc) && nca_count == InstalledContentCount, rc);
            Check("two of the NCAs open as content metadata, the others are refused", meta_count == 2);
            Check("the test program's metadata has its ID, version and one Program content", found_program_meta);

            if (g_have_program_content_id) {
                char path[0x80];
                const auto name = ncm::GetContentIdString(g_program_content_id);
                util::SNPrintf(path, sizeof(path), "cs:/registered/%s.nca", name.data);

                bool has_file = false;
                rc = fs::HasFile(std::addressof(has_file), path);
                Check("the content the metadata names is in the storage", R_SUCCEEDED(rc) && has_file, rc);
            }

            fs::Unmount("cs");

            rc = fs::MountContentStorage("csu", fs::ContentStorageId::User);
            Check("mount the user content storage (created on first use)", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                fs::Unmount("csu");
            }

            rc = fs::MountContentStorage("cs0", fs::ContentStorageId::System0);
            Check("the System0 content storage does not exist", R_FAILED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                fs::Unmount("cs0");
            }
        }

        void TestCode(const char *how) {
            char what[0x80];

            fs::CodeVerificationData verification;
            Result rc = fs::MountCode(std::addressof(verification), "code", "/", fs::ContentAttributes_None, TestProgramId, ncm::StorageId::BuiltInSystem);
            util::SNPrintf(what, sizeof(what), "open the test program's code (%s)", how);
            Check(what, R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                util::SNPrintf(what, sizeof(what), "main reads back intact, 300000 bytes (%s)", how);
                Check(what, VerifyMain("code"));

                size_t size = 0;
                rc = ReadWholeFile(std::addressof(size), "code:/main.npdm", g_buffer, sizeof(g_buffer));
                util::SNPrintf(what, sizeof(what), "main.npdm reads back intact (%s)", how);
                Check(what, R_SUCCEEDED(rc) && size == sizeof(TestNpdm) - 1 && std::memcmp(g_buffer, TestNpdm, size) == 0, rc);

                int count = 0;
                rc = ForEachEntry("code:/", [&](const fs::DirectoryEntry &) { ++count; });
                util::SNPrintf(what, sizeof(what), "the code filesystem lists its 3 files (%s)", how);
                Check(what, R_SUCCEEDED(rc) && count == 3, rc);

                rc = fs::CreateFile("code:/new", 16);
                util::SNPrintf(what, sizeof(what), "the code filesystem is read-only (%s)", how);
                Check(what, R_FAILED(rc), rc);

                fs::Unmount("code");
            }

            rc = fs::MountCode(std::addressof(verification), "code", "/", fs::ContentAttributes_None, MissingProgramId, ncm::StorageId::BuiltInSystem);
            util::SNPrintf(what, sizeof(what), "a program that is not installed is not found (%s)", how);
            Check(what, R_FAILED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                fs::Unmount("code");
            }

            /* Damage is caught either when the section is opened or when the damaged block is read. */
            rc = fs::MountCode(std::addressof(verification), "code", "/", fs::ContentAttributes_None, DamagedProgramId, ncm::StorageId::BuiltInSystem);
            if (R_SUCCEEDED(rc)) {
                size_t size = 0;
                rc = ReadWholeFile(std::addressof(size), "code:/main", g_buffer, sizeof(g_buffer));
                fs::Unmount("code");
            }
            util::SNPrintf(what, sizeof(what), "a damaged NCA fails hash verification (%s)", how);
            Check(what, R_FAILED(rc), rc);
        }

        /* ===== Phase 2: Atmosphere's ncm, hosted here ===== */

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x400;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = false;
        };

        constexpr size_t ManagerSessions = 16;
        constexpr size_t MaxSessions     = 32;

        using ServerManager = sf::hipc::ServerManager<1, ServerOptions, MaxSessions>;

        sf::UnmanagedServiceObject<ncm::IContentManager, ncm::ContentManagerImpl> g_ncm_manager;
        sf::UnmanagedServiceObject<lr::ILocationResolverManager, lr::LocationResolverManagerImpl> g_lr_manager;

        ServerManager g_ncm_server;
        ServerManager g_lr_server;

        alignas(os::ThreadStackAlignment) u8 g_ncm_thread_stack[16_KB];
        alignas(os::ThreadStackAlignment) u8 g_lr_thread_stack[16_KB];
        os::ThreadType g_ncm_thread;
        os::ThreadType g_lr_thread;

        void ServerThread(void *server) {
            static_cast<ServerManager *>(server)->LoopProcess();
        }

        Result StartServer(ServerManager *server, os::ThreadType *thread, void *stack, size_t stack_size) {
            R_TRY(os::CreateThread(thread, ServerThread, server, stack, stack_size, os::GetThreadPriority(os::GetCurrentThread())));
            os::StartThread(thread);
            R_SUCCEED();
        }

        bool StartNcm() {
            ncm::InitializeWithObject(g_ncm_manager.GetShared());

            /* The configuration of a normal boot: neither forced rebuild nor import. */
            constexpr ncm::ContentManagerConfig Config = { false, false };
            Result rc = g_ncm_manager.GetImpl().Initialize(Config);
            Check("Atmosphere's content manager initializes against FS", R_SUCCEEDED(rc), rc);
            if (R_FAILED(rc)) {
                return false;
            }

            rc = g_ncm_server.RegisterObjectForServer(g_ncm_manager.GetShared(), sm::ServiceName::Encode("ncm"), ManagerSessions);
            if (R_SUCCEEDED(rc)) {
                rc = StartServer(std::addressof(g_ncm_server), std::addressof(g_ncm_thread), g_ncm_thread_stack, sizeof(g_ncm_thread_stack));
            }
            if (R_SUCCEEDED(rc)) {
                rc = g_lr_server.RegisterObjectForServer(g_lr_manager.GetShared(), sm::ServiceName::Encode("lr"), ManagerSessions);
            }
            if (R_SUCCEEDED(rc)) {
                rc = StartServer(std::addressof(g_lr_server), std::addressof(g_lr_thread), g_lr_thread_stack, sizeof(g_lr_thread_stack));
            }
            Check("register the ncm and lr services", R_SUCCEEDED(rc), rc);
            return R_SUCCEEDED(rc);
        }

        void TestNcm() {
            /* The database ncm built from the content storage. */
            ncm::ContentMetaDatabase db;
            Result rc = ncm::OpenContentMetaDatabase(std::addressof(db), ncm::StorageId::BuiltInSystem);
            Check("open ncm's system content meta database", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                ncm::ContentId content_id = {};
                rc = db.GetLatestProgram(std::addressof(content_id), TestProgramId);
                Check("ncm's database names the test program's content", R_SUCCEEDED(rc) && g_have_program_content_id && content_id == g_program_content_id, rc);

                const auto count = db.ListContentMeta(nullptr, 0);
                Check("ncm's database has both installed programs", count.total == 2);
            }

            u32 flags = 0;
            rc = fs::GetSaveDataFlags(std::addressof(flags), fs::SaveDataSpaceId::System, NcmSaveDataId);
            Check("ncm created its database save data (8000000000000120)", R_SUCCEEDED(rc), rc);

            /* Install a new content the way ncm's installer does: placeholder, write, register. */
            ncm::ContentStorage storage;
            rc = ncm::OpenContentStorage(std::addressof(storage), ncm::StorageId::BuiltInSystem);
            Check("open ncm's system content storage", R_SUCCEEDED(rc), rc);
            if (R_SUCCEEDED(rc)) {
                constexpr size_t NewSize = 100000;
                for (size_t i = 0; i < NewSize; ++i) {
                    g_buffer[i] = static_cast<u8>(i * 13 + 1);
                }

                constexpr ncm::ContentId NewId = { { 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89 } };
                const ncm::PlaceHolderId placeholder = storage.GeneratePlaceHolderId();

                rc = storage.CreatePlaceHolder(placeholder, NewId, NewSize);
                const Result rc2 = R_SUCCEEDED(rc) ? storage.WritePlaceHolder(placeholder, 0, g_buffer, NewSize) : rc;
                const Result rc3 = R_SUCCEEDED(rc2) ? storage.Register(placeholder, NewId) : rc2;
                Check("ncm installs a content: placeholder, write, register", R_SUCCEEDED(rc3), rc3);

                bool has = false;
                s64 size = 0;
                rc = storage.Has(std::addressof(has), NewId);
                const Result rc4 = storage.GetSize(std::addressof(size), NewId);
                Check("ncm finds the new content, with its size", R_SUCCEEDED(rc) && has && R_SUCCEEDED(rc4) && size == NewSize, R_FAILED(rc) ? rc : rc4);

                std::memset(g_buffer, 0, NewSize);
                rc = storage.ReadContentIdFile(g_buffer, NewSize, NewId, 0);
                bool same = R_SUCCEEDED(rc);
                for (size_t i = 0; same && i < NewSize; ++i) {
                    same = g_buffer[i] == static_cast<u8>(i * 13 + 1);
                }
                Check("ncm reads the new content back intact", same, rc);

                rc = storage.Delete(NewId);
                const Result rc5 = storage.Has(std::addressof(has), NewId);
                Check("ncm deletes the new content", R_SUCCEEDED(rc) && R_SUCCEEDED(rc5) && !has, rc);
            }

            /* The location resolver, which FS itself now uses to find program code. */
            lr::Initialize();
            lr::LocationResolver resolver;
            rc = lr::OpenLocationResolver(std::addressof(resolver), ncm::StorageId::BuiltInSystem);
            if (R_SUCCEEDED(rc)) {
                static lr::Path path;
                rc = resolver.ResolveProgramPath(std::addressof(path), TestProgramId);
                if (R_SUCCEEDED(rc)) {
                    Log("[content_test] lr resolves the test program to %s\n", path.str);
                }
            }
            Check("lr resolves the test program's path", R_SUCCEEDED(rc), rc);
        }

    }

    namespace init {

        void InitializeSystemModule() {
            g_heap_handle = lmem::CreateExpHeap(g_heap_memory, sizeof(g_heap_memory), lmem::CreateOption_ThreadSafe);
            AMS_ABORT_UNLESS(g_heap_handle != nullptr);

            R_ABORT_UNLESS(sm::Initialize());
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() { /* ... */ }

    }

    void NORETURN Exit(int rc) {
        AMS_UNUSED(rc);
        AMS_ABORT("Exit called by immortal process");
    }

    void Main() {
        Log("[content_test] starting\n");

        fs::InitializeWithMultiSessionForSystem();
        fs::SetAllocator(Allocate, Deallocate);
        fs::SetEnabledAutoAbort(false);
        R_ABORT_UNLESS(::fsldrInitialize());

        Log("[content_test] Atmosphere content metadata, ncm and lr\n");
        TestContentStorage();
        if (StartNcm()) {
            TestNcm();
            TestCode("resolved by lr");
        }

        Log("[content_test] done: %d checks, %d failure(s)\n", g_checks, g_failures);

        /* Keep serving ncm and lr; this KIP is immortal. */
        while (true) {
            os::SleepThread(TimeSpan::FromSeconds(60));
        }
    }

}

void *operator new(size_t size) { return ams::Allocate(size); }
void *operator new(size_t size, const std::nothrow_t &) { return ams::Allocate(size); }
void operator delete(void *p) { ams::Deallocate(p, 0); }
void operator delete(void *p, size_t size) { ams::Deallocate(p, size); }
void *operator new[](size_t size) { return ams::Allocate(size); }
void *operator new[](size_t size, const std::nothrow_t &) { return ams::Allocate(size); }
void operator delete[](void *p) { ams::Deallocate(p, 0); }
void operator delete[](void *p, size_t size) { ams::Deallocate(p, size); }
