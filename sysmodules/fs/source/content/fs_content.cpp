#include "fs_filesystem.hpp"
#include "fs_content.hpp"
#include "../fs_log.hpp"
#include "../fat/fs_fat_volumes.hpp"
#include "../nca/fs_nca.hpp"
#include "../partition/fs_partitions.hpp"

namespace kumo::horizon::hzfs::content {

    namespace {

        struct ContentPath { char str[0x301]; };

        constexpr const char ContentDirectory[]            = "/Contents";
        constexpr const char SdCardNintendoDirectory[]     = "/Nintendo";
        constexpr const char SdCardContentDirectory[]      = "/Nintendo/Contents";
        constexpr const char RegisteredContentDirectory[]  = "/registered";
        constexpr const char NcaExtension[]                = ".nca";

        /* ncm nests content at most two directories below "registered". */
        constexpr int MaxContentDirectoryDepth = 2;

        Result EnsureDirectory(fs::fsa::IFileSystem *file_system, const char *path) {
            fs::Path fs_path;
            KUMO_TRY(fs_path.InitializeWithNormalization(path));

            { const Result rc = file_system->CreateDirectory(fs_path); if (rc != fs::ResultPathAlreadyExists()) { KUMO_TRY(rc); } }
            KUMO_SUCCEED();
        }

        Result OpenSubDirectory(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<fs::fsa::IFileSystem> base_fs, const char *path) {
            fs::Path fs_path;
            KUMO_TRY(fs_path.InitializeWithNormalization(path));

            auto sub_fs = fs::AllocateShared<fs::fsa::SubDirectoryFileSystem>(std::move(base_fs));
            KUMO_UNLESS(sub_fs != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
            KUMO_TRY(sub_fs->Initialize(fs_path));

            *out = std::move(sub_fs);
            KUMO_SUCCEED();
        }

        Result OpenVolumeFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, fat::Volume volume) {
            if (volume == fat::Volume_SdCard) {
                KUMO_UNLESS(partition::FindPartition(partition::SdCardPartitionName) != nullptr, fs::ResultSdCardNotPresent());
            }
            KUMO_RETURN(fat::OpenVolume(out, volume));
        }

        /* The filesystems a content path can name. */
        Result OpenSystemContentStorage(std::shared_ptr<fs::fsa::IFileSystem> *out) { KUMO_RETURN(OpenContentStorage(out, fs::ContentStorageId::System)); }
        Result OpenUserContentStorage(std::shared_ptr<fs::fsa::IFileSystem> *out)   { KUMO_RETURN(OpenContentStorage(out, fs::ContentStorageId::User)); }
        Result OpenSdCardContentStorage(std::shared_ptr<fs::fsa::IFileSystem> *out) { KUMO_RETURN(OpenContentStorage(out, fs::ContentStorageId::SdCard)); }
        Result OpenSdCard(std::shared_ptr<fs::fsa::IFileSystem> *out)               { KUMO_RETURN(OpenVolumeFileSystem(out, fat::Volume_SdCard)); }
        Result OpenSystem(std::shared_ptr<fs::fsa::IFileSystem> *out)               { KUMO_RETURN(OpenVolumeFileSystem(out, fat::Volume_System)); }
        Result OpenUser(std::shared_ptr<fs::fsa::IFileSystem> *out)                 { KUMO_RETURN(OpenVolumeFileSystem(out, fat::Volume_User)); }

        constexpr const struct {
            const char *name;
            Result (*open)(std::shared_ptr<fs::fsa::IFileSystem> *out);
        } MountNames[] = {
            /* Longer names first: "@SystemContent" also starts with "@System". */
            { fs::impl::ContentStorageSystemMountName, OpenSystemContentStorage },
            { fs::impl::ContentStorageUserMountName,   OpenUserContentStorage   },
            { fs::impl::ContentStorageSdCardMountName, OpenSdCardContentStorage },
            { fs::impl::SdCardFileSystemMountName,     OpenSdCard               },
            { fs::impl::BisSystemPartitionMountName,   OpenSystem               },
            { fs::impl::BisUserPartitionMountName,     OpenUser                 },
        };

        /* Split "@Mount:/a/b" into the mounted filesystem and "/a/b". */
        Result ParseMountName(std::shared_ptr<fs::fsa::IFileSystem> *out, const char **out_path, const char *path) {
            for (const auto &mount : MountNames) {
                const size_t len = std::strlen(mount.name);
                if (std::strncmp(path, mount.name, len) != 0 || path[len] != ':') {
                    continue;
                }

                /* ncm separates the mount name from the path with "://". */
                const char *rest = path + len + 1;
                while (rest[0] == '/' && rest[1] == '/') {
                    ++rest;
                }
                KUMO_UNLESS(rest[0] == '/', fs::ResultPathNotFound());

                KUMO_TRY(mount.open(out));
                *out_path = rest;
                KUMO_SUCCEED();
            }

            KUMO_THROW(fs::ResultPathNotFound());
        }

        bool HasSuffix(const char *str, const char *suffix) {
            const size_t len = std::strlen(str), suffix_len = std::strlen(suffix);
            return len >= suffix_len && strncasecmp(str + len - suffix_len, suffix, suffix_len) == 0;
        }

        /* The NCA content type and section that hold a filesystem type. */
        Result GetContentLocation(kumo::fs::PlaintextNca::ContentType *out_type, s32 *out_section, fs::impl::FileSystemProxyType type) {
            using ContentType = kumo::fs::PlaintextNca::ContentType;
            switch (type) {
                case fs::impl::FileSystemProxyType_Code:    *out_type = ContentType::Program; *out_section = nca::ProgramSection_Code; break;
                case fs::impl::FileSystemProxyType_Rom:     *out_type = ContentType::Program; *out_section = nca::ProgramSection_Data; break;
                case fs::impl::FileSystemProxyType_Logo:    *out_type = ContentType::Program; *out_section = nca::ProgramSection_Logo; break;
                case fs::impl::FileSystemProxyType_Control: *out_type = ContentType::Control; *out_section = 0;                        break;
                case fs::impl::FileSystemProxyType_Manual:  *out_type = ContentType::Manual;  *out_section = 0;                        break;
                case fs::impl::FileSystemProxyType_Meta:    *out_type = ContentType::Meta;    *out_section = 0;                        break;
                case fs::impl::FileSystemProxyType_Data:    *out_type = ContentType::Data;    *out_section = 0;                        break;
                default:
                    KUMO_THROW(fs::ResultInvalidArgument());
            }
            KUMO_SUCCEED();
        }

        bool IsContentType(const nca::Reader &reader, kumo::fs::PlaintextNca::ContentType type) {
            const auto actual = reader.archive.Type();
            /* Public data is opened the same way as data. */
            return actual == type || (type == kumo::fs::PlaintextNca::ContentType::Data && actual == kumo::fs::PlaintextNca::ContentType::PublicData);
        }

        /* Search dir_path of the content storage for the Program NCA of program_id. Sets *out to nullptr if there is none. */
        Result OpenContentReader(std::shared_ptr<nca::Reader> *out, std::shared_ptr<fs::fsa::IFileSystem> base_fs,
                                 const char *path, kumo::fs::PlaintextNca::ContentType type, u64 program_id) {
            KUMO_UNLESS(HasSuffix(path, NcaExtension), fs::ResultPathNotFound());
            std::shared_ptr<nca::Reader> reader;
            KUMO_TRY(nca::OpenReader(&reader, std::move(base_fs), path));
            KUMO_UNLESS(IsContentType(*reader, type), fs::ResultPreconditionViolation());
            KUMO_UNLESS(program_id == ncm::InvalidProgramId.value || reader->archive.ProgramId() == program_id,
                        fs::ResultInvalidNcaId());
            *out = std::move(reader);
            return 0;
        }

        Result FindProgramContent(std::shared_ptr<nca::Reader> *out, const std::shared_ptr<fs::fsa::IFileSystem> &storage_fs, const char *dir_path, ncm::ProgramId program_id, int depth) {
            fs::Path fs_path;
            KUMO_TRY(fs_path.InitializeWithNormalization(dir_path));

            std::unique_ptr<fs::fsa::IDirectory> dir;
            KUMO_TRY(storage_fs->OpenDirectory(std::addressof(dir), fs_path, fs::OpenDirectoryMode_All));

            /* One entry at a time: the entries are large, and this runs on a service thread's stack. */
            auto entry = fs::AllocateShared<fs::DirectoryEntry>();
            KUMO_UNLESS(entry != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
            auto child = fs::AllocateShared<ContentPath>();
            KUMO_UNLESS(child != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());

            while (true) {
                s64 count;
                KUMO_TRY(dir->Read(std::addressof(count), entry.get(), 1));
                if (count == 0) {
                    break;
                }

                const int len = std::snprintf(child->str, sizeof(child->str), "%s/%s", dir_path, entry->name);
                if (len < 0 || static_cast<size_t>(len) >= sizeof(child->str)) {
                    continue;
                }

                if (entry->type == fs::DirectoryEntryType_Directory) {
                    if (depth < MaxContentDirectoryDepth) {
                        KUMO_TRY(FindProgramContent(out, storage_fs, child->str, program_id, depth + 1));
                        KUMO_SUCCEED_IF(*out != nullptr);
                    }
                    continue;
                }

                if (!HasSuffix(entry->name, NcaExtension)) {
                    continue;
                }

                /* A file that is not a valid NCA is not the one we want; keep looking. */
                std::shared_ptr<nca::Reader> reader;
                if (R_FAILED(nca::OpenReader(std::addressof(reader), storage_fs, child->str))) {
                    continue;
                }
                if (reader->archive.Type() == kumo::fs::PlaintextNca::ContentType::Program && reader->archive.ProgramId() == program_id.value) {
                    *out = std::move(reader);
                    KUMO_SUCCEED();
                }
            }

            *out = nullptr;
            KUMO_SUCCEED();
        }

        Result GetContentStorageId(fs::ContentStorageId *out, ncm::StorageId storage_id) {
            switch (storage_id) {
                case ncm::StorageId::BuiltInSystem: *out = fs::ContentStorageId::System; break;
                case ncm::StorageId::BuiltInUser:   *out = fs::ContentStorageId::User;   break;
                case ncm::StorageId::SdCard:        *out = fs::ContentStorageId::SdCard; break;
                default:
                    /* Host and game card storage do not exist here; a program with no storage has no base code. */
                    KUMO_THROW(fs::ResultTargetNotFound());
            }
            KUMO_SUCCEED();
        }

        constinit kumo::Mutex g_lr_mutex;
        constinit bool g_lr_initialized = false;

        /* Whether ncm's location resolver can be asked. FS starts before ncm, and may run without it. */
        bool IsLocationResolverAvailable() {
            std::scoped_lock lk(g_lr_mutex);

            if (!g_lr_initialized) {
                u8 has_service = 0;
                const SmServiceName name = smEncodeName("lr");
                if (R_FAILED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, has_service)) || !has_service) {
                    return false;
                }

                if (R_FAILED(lrInitialize())) return false;
                g_lr_initialized = true;
            }
            return true;
        }

        /* The path of a program's NCA, as the loader itself would resolve it: registered redirections first. */
        Result ResolveProgramPath(ContentPath *out, ncm::ProgramId program_id, ncm::StorageId storage_id) {
            {
                LrRegisteredLocationResolver resolver{};
                KUMO_TRY(lrOpenRegisteredLocationResolver(&resolver));
                const kumo::ScopeExit close([&] { serviceClose(&resolver.s); });
                KUMO_SUCCEED_IF(R_SUCCEEDED(lrRegLrResolveProgramPath(&resolver, program_id.value, out->str)));
            }
            LrLocationResolver resolver{};
            KUMO_TRY(lrOpenLocationResolver(static_cast<NcmStorageId>(storage_id), &resolver));
            const kumo::ScopeExit close([&] { serviceClose(&resolver.s); });
            return lrLrResolveProgramPath(&resolver, program_id.value, out->str);
        }

    }

    Result OpenContentStorage(std::shared_ptr<fs::fsa::IFileSystem> *out, fs::ContentStorageId id) {
        std::shared_ptr<fs::fsa::IFileSystem> volume_fs;
        const char *directory = ContentDirectory;

        switch (id) {
            case fs::ContentStorageId::System:
                KUMO_TRY(OpenVolumeFileSystem(std::addressof(volume_fs), fat::Volume_System));
                break;
            case fs::ContentStorageId::User:
                KUMO_TRY(OpenVolumeFileSystem(std::addressof(volume_fs), fat::Volume_User));
                break;
            case fs::ContentStorageId::SdCard:
                KUMO_TRY(OpenVolumeFileSystem(std::addressof(volume_fs), fat::Volume_SdCard));
                KUMO_TRY(EnsureDirectory(volume_fs.get(), SdCardNintendoDirectory));
                directory = SdCardContentDirectory;
                break;
            default:
                /* System0 is a second system partition that this OS does not have. */
                KUMO_THROW(fs::ResultPartitionNotFound());
        }

        KUMO_TRY(EnsureDirectory(volume_fs.get(), directory));
        KUMO_RETURN(OpenSubDirectory(out, std::move(volume_fs), directory));
    }

    Result OpenFileSystemByPath(std::shared_ptr<fs::fsa::IFileSystem> *out, const char *path, fs::impl::FileSystemProxyType type, u64 program_id) {
        std::shared_ptr<fs::fsa::IFileSystem> base_fs;
        const char *content_path;
        KUMO_TRY(ParseMountName(std::addressof(base_fs), std::addressof(content_path), path));

        if (type == fs::impl::FileSystemProxyType_Package) {
            KUMO_RETURN(nca::OpenPackageFileSystem(out, std::move(base_fs), content_path));
        }

        kumo::fs::PlaintextNca::ContentType content_type;
        s32 section;
        KUMO_TRY(GetContentLocation(std::addressof(content_type), std::addressof(section), type));

        std::shared_ptr<nca::Reader> reader;
        KUMO_TRY(OpenContentReader(&reader, std::move(base_fs), content_path, content_type, program_id));

        KUMO_RETURN(nca::OpenSectionFileSystem(out, std::move(reader), section));
    }

    static Result OpenProgramReader(std::shared_ptr<nca::Reader> *out, ncm::ProgramId program_id, ncm::StorageId storage_id) {
        if (IsLocationResolverAvailable()) {
            auto path = fs::AllocateShared<ContentPath>();
            KUMO_UNLESS(path != nullptr, fs::ResultAllocationMemoryFailedInFileSystemProxyImplA());
            KUMO_TRY(ResolveProgramPath(path.get(), program_id, storage_id));
            std::shared_ptr<fs::fsa::IFileSystem> base_fs;
            const char *content_path;
            KUMO_TRY(ParseMountName(&base_fs, &content_path, path->str));
            return OpenContentReader(out, std::move(base_fs), content_path,
                                     kumo::fs::PlaintextNca::ContentType::Program, program_id.value);
        }

        /* Without ncm there is no database to ask, so look at the content itself. */
        fs::ContentStorageId content_storage_id;
        KUMO_TRY(GetContentStorageId(std::addressof(content_storage_id), storage_id));

        std::shared_ptr<fs::fsa::IFileSystem> storage_fs;
        KUMO_TRY(OpenContentStorage(std::addressof(storage_fs), content_storage_id));

        std::shared_ptr<nca::Reader> reader;
        { const Result rc = FindProgramContent(std::addressof(reader), storage_fs, RegisteredContentDirectory, program_id, 0); KUMO_TRY(rc == fs::ResultPathNotFound() ? fs::ResultTargetNotFound() : rc); }
        KUMO_UNLESS(reader != nullptr, fs::ResultTargetNotFound());
        Log("[fs] program %016lx: found in the content storage (ncm is not running)\n", program_id.value);

        *out = std::move(reader);
        return 0;
    }

    Result OpenCodeFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, ncm::ProgramId program_id, ncm::StorageId storage_id) {
        std::shared_ptr<nca::Reader> reader;
        KUMO_TRY(OpenProgramReader(&reader, program_id, storage_id));
        KUMO_RETURN(nca::OpenSectionFileSystem(out, std::move(reader), nca::ProgramSection_Code));
    }

    Result OpenProgramRomStorage(std::shared_ptr<fs::IStorage> *out, ncm::ProgramId program_id, ncm::StorageId storage_id) {
        std::shared_ptr<nca::Reader> reader;
        KUMO_TRY(OpenProgramReader(&reader, program_id, storage_id));
        KUMO_UNLESS(reader->archive.HasSection(nca::ProgramSection_Data), fs::ResultTargetNotFound());
        KUMO_RETURN(nca::OpenRomStorage(out, std::move(reader), nca::ProgramSection_Data));
    }

}
