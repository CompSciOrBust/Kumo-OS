#include "fs_file_system_proxy.hpp"
#include "fs_objects.hpp"
#include "../content/fs_content.hpp"
#include "../fat/fs_fat_volumes.hpp"
#include "../partition/fs_partitions.hpp"
#include "../registry/fs_program_registry.hpp"
#include "../savedata/fs_save_data.hpp"
#include "../fs_log.hpp"
#include <kumo/fs/storage_service.hpp>

namespace kumo::horizon::hzfs::ipc {
namespace {
using namespace kumo::horizon::fs;
using Fs = std::shared_ptr<fsa::IFileSystem>;

struct ContentRequest {
    static constexpr size_t WireBytes = 16;
    u8 attributes;
    u8 pad[3];
    u32 type;
    u64 program_id;
};
struct LegacyContentRequest {
    static constexpr size_t WireBytes = 16;
    u32 type, pad;
    u64 program_id;
};
struct LoaderRequest {
    static constexpr size_t WireBytes = 16;
    u8 attributes, storage_id;
    u8 pad[6];
    u64 program_id;
};
struct LegacyLoaderRequest {
    static constexpr size_t WireBytes = 16;
    u8 attributes;
    u8 pad[7];
    u64 program_id;
};
struct SaveKey {
    static constexpr size_t WireBytes = 16;
    SaveDataSpaceId space;
    u8 pad[7];
    u64 id;
};
struct SaveAttributeRequest {
    static constexpr size_t WireBytes = 0x48;
    SaveDataSpaceId space;
    u8 pad[7];
    SaveDataAttribute attribute;
};
struct CreateSave {
    static constexpr size_t WireBytes = 0x80;
    SaveDataAttribute attribute;
    SaveDataCreationInfo creation;
};
struct RegisterProgram {
    static constexpr size_t WireBytes = 0x28;
    u8 storage_id;
    u8 pad[7];
    u64 process_id, program_id;
    s64 data_size, desc_size;
};

template <class T> consteval auto SetProcess(u32 id) {
    return MakeCommand<T, u64, NoData, 0, 0, [](T &self, u64, NoData &, auto &args) -> Result {
        self.process_id.store(args.Pid().value, std::memory_order_relaxed);
        return 0;
    }>(id, Pid);
}

Result CheckSdCard() { return partition::FindPartition(partition::SdCardPartitionName) ? 0 : ResultSdCardNotPresent(); }

Result OpenContent(ConcurrentReply &reply, InBuffer buffer, u32 type, u64 program_id) {
    std::string_view path;
    KUMO_TRY(DecodePath(buffer, path));
    Fs fs;
    KUMO_TRY(content::OpenFileSystemByPath(&fs, path.data(), static_cast<impl::FileSystemProxyType>(type), program_id));
    return ReturnFileSystem(reply, std::move(fs));
}

Result ReadExtra(OutBuffer buffer, SaveDataSpaceId space, u64 id) {
    KUMO_UNLESS(buffer.GetSize() == sizeof(SaveDataExtraData), ResultInvalidArgument());
    SaveDataExtraData data{};
    KUMO_TRY(savedata::ReadExtraData(&data, space, savedata::MakeSystemSaveDataAttribute(id)));
    std::memcpy(buffer.GetPointer(), &data, sizeof(data));
    return 0;
}

struct UnimplementedCommand {
    u32 id, minimum, maximum;
};
template <size_t N>
bool DispatchUnimplemented(ConcurrentRequest &request, ConcurrentReply &reply,
                           const UnimplementedCommand (&commands)[N]) {
    const u32 version = hosversionGet();
    for (const auto &command : commands) {
        if (command.id == request.input.header.command_id && command.minimum <= version && version <= command.maximum) {
            reply.result = ResultNotImplemented();
            return true;
        }
    }
    return false;
}

#include "fs_unimplemented_commands.inc"

class DeviceOperator final : public ServiceObject {
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        static constexpr CommandTable commands({
            MakeCommand<DeviceOperator, NoData, u8, 0, 0,
                        [](DeviceOperator &, const NoData &, u8 &out, auto &) -> Result {
                            out = R_SUCCEEDED(CheckSdCard());
                            return 0;
                        }>(0),
            MakeCommand<DeviceOperator, NoData, u8, 0, 0,
                        [](DeviceOperator &, const NoData &, u8 &, auto &) -> Result { return 0; }>(200),
        });
        switch (request.input.header.command_id) {
        case 0:
        case 200:
            return commands.Dispatch(this, request, reply);
        default:
            return DispatchUnimplemented(request, reply, UnimplementedDeviceCommands);
        }
    }
};

class Proxy final : public ServiceObject {
  public:
    std::atomic<u64> process_id{0};

    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        static constexpr CommandTable commands({
            SetProcess<Proxy>(1),
            MakeCommand<Proxy, LegacyContentRequest, NoData, 1, 0,
                        [](Proxy &, const LegacyContentRequest &in, NoData &, auto &args) {
                            return OpenContent(args.reply, args.In(0), in.type, in.program_id);
                        }>(8, PathBuffer),
            MakeCommand<Proxy, ContentRequest, NoData, 1, 0,
                        [](Proxy &, const ContentRequest &in, NoData &, auto &args) {
                            return OpenContent(args.reply, args.In(0), in.type, in.program_id);
                        }>(10, PathBuffer),
            MakeCommand<Proxy, u32, NoData, 1, 0,
                        [](Proxy &, u32 id, NoData &, auto &args) -> Result {
                            std::string_view root;
                            KUMO_TRY(DecodePath(args.In(0), root));
                            fat::Volume volume;
                            KUMO_TRY(fat::GetBisVolume(&volume, static_cast<BisPartitionId>(id)));
                            Fs fs;
                            KUMO_TRY(fat::OpenVolume(&fs, volume));
                            if (!root.empty()) {
                                Path path;
                                KUMO_TRY(path.InitializeWithNormalization(root));
                                auto subdir = AllocateShared<fsa::SubDirectoryFileSystem>(std::move(fs));
                                KUMO_UNLESS(subdir, ResultAllocationMemoryFailed());
                                KUMO_TRY(subdir->Initialize(path));
                                fs = std::move(subdir);
                            }
                            return ReturnFileSystem(args.reply, std::move(fs));
                        }>(11, PathBuffer),
            MakeCommand<Proxy, u32, NoData, 0, 0,
                        [](Proxy &, u32 id, NoData &, auto &args) -> Result {
                            IStorage *storage;
                            KUMO_TRY(partition::GetBisStorage(&storage, static_cast<BisPartitionId>(id)));
                            return ReturnObject<kumo::fs::StorageService>(args.reply, *storage);
                        }>(12),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &) -> Result { return 0; }>(13),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &args) -> Result {
                            KUMO_TRY(CheckSdCard());
                            Fs fs;
                            KUMO_TRY(fat::OpenVolume(&fs, fat::Volume_SdCard));
                            return ReturnFileSystem(args.reply, std::move(fs));
                        }>(18),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &) -> Result {
                            KUMO_TRY(CheckSdCard());
                            return fat::FormatVolume(fat::Volume_SdCard);
                        }>(19),
            MakeCommand<Proxy, u64, NoData, 0, 0,
                        [](Proxy &, u64 id, NoData &, auto &) {
                            return savedata::DeleteSystemSaveData(SaveDataSpaceId::System,
                                                                  savedata::MakeSystemSaveDataAttribute(id));
                        }>(21),
            MakeCommand<Proxy, CreateSave, NoData, 0, 0,
                        [](Proxy &, const CreateSave &in, NoData &, auto &) {
                            return savedata::CreateSystemSaveData(in.attribute, in.creation);
                        }>(23),
            MakeCommand<Proxy, SaveKey, NoData, 0, 0,
                        [](Proxy &, const SaveKey &in, NoData &, auto &) {
                            return savedata::DeleteSystemSaveData(in.space,
                                                                  savedata::MakeSystemSaveDataAttribute(in.id));
                        }>(25),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &) { return CheckSdCard(); }>(26),
            MakeCommand<Proxy, NoData, u8, 0, 0,
                        [](Proxy &, const NoData &, u8 &out, auto &) -> Result {
                            out = 1;
                            return 0;
                        }>(27),
            MakeCommand<Proxy, SaveAttributeRequest, NoData, 0, 0,
                        [](Proxy &, const SaveAttributeRequest &in, NoData &, auto &) {
                            return savedata::DeleteSystemSaveData(in.space, in.attribute);
                        }>(28),
            MakeCommand<Proxy, SaveAttributeRequest, NoData, 0, 0,
                        [](Proxy &, const SaveAttributeRequest &in, NoData &, auto &args) -> Result {
                            Fs fs;
                            KUMO_TRY(savedata::OpenSystemSaveData(&fs, in.space, in.attribute));
                            return ReturnFileSystem(args.reply, std::move(fs));
                        }>(52),
            MakeCommand<Proxy, SaveKey, NoData, 0, 1,
                        [](Proxy &, const SaveKey &in, NoData &, auto &args) {
                            return ReadExtra(args.Out(0), in.space, in.id);
                        }>(57, Mapped),
            MakeCommand<Proxy, u64, NoData, 0, 1,
                        [](Proxy &, u64 id, NoData &, auto &args) {
                            return ReadExtra(args.Out(0), SaveDataSpaceId::System, id);
                        }>(58, Mapped),
            MakeCommand<Proxy, SaveKey, NoData, 1, 0,
                        [](Proxy &, const SaveKey &in, NoData &, auto &args) -> Result {
                            const auto buffer = args.In(0);
                            KUMO_UNLESS(buffer.GetSize() == sizeof(SaveDataExtraData), ResultInvalidArgument());
                            u32 flags;
                            std::memcpy(&flags, buffer.GetPointer() + offsetof(SaveDataExtraData, flags),
                                        sizeof(flags));
                            return savedata::WriteExtraDataFlags(in.space, savedata::MakeSystemSaveDataAttribute(in.id),
                                                                 flags);
                        }>(59, Mapped),
            MakeCommand<Proxy, u32, NoData, 0, 0,
                        [](Proxy &, u32 id, NoData &, auto &args) -> Result {
                            Fs fs;
                            KUMO_TRY(content::OpenContentStorage(&fs, static_cast<ContentStorageId>(id)));
                            return ReturnFileSystem(args.reply, std::move(fs));
                        }>(110),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &self, const NoData &, NoData &, auto &args) -> Result {
                            registry::ProgramInfo program{};
                            const u64 pid = self.process_id.load(std::memory_order_relaxed);
                            KUMO_UNLESS(pid != 0 && registry::GetProgramInfo(&program, pid), ResultTargetNotFound());
                            std::shared_ptr<IStorage> storage;
                            KUMO_TRY(content::OpenProgramRomStorage(&storage, program.program_id, program.storage_id));
                            return ReturnObject<kumo::fs::StorageService>(args.reply, std::move(storage));
                        }>(200),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &args) {
                            return ReturnObject<DeviceOperator>(args.reply);
                        }>(400),
            MakeCommand<Proxy, NoData, NoData, 0, 0,
                        [](Proxy &, const NoData &, NoData &, auto &) -> Result { return 0; }>(1003),
        });
        switch (request.input.header.command_id) {
        case 8:
        case 19:
        case 25:
        case 26:
        case 27:
        case 59:
            if (hosversionBefore(2, 0, 0))
                return false;
            break;
        case 10:
            if (hosversionBefore(16, 0, 0))
                return false;
            break;
        case 28:
            if (hosversionBefore(4, 0, 0))
                return false;
            break;
        case 57:
            if (hosversionBefore(3, 0, 0))
                return false;
            break;
        case 1003:
            if (hosversionGet() > MAKEHOSVERSION(21, 2, 0))
                return false;
            break;
        case 1:
        case 11:
        case 12:
        case 13:
        case 18:
        case 21:
        case 23:
        case 52:
        case 58:
        case 110:
        case 200:
        case 400:
            break;
        default:
            return DispatchUnimplemented(request, reply, UnimplementedProxyCommands);
        }
        return commands.Dispatch(this, request, reply);
    }
};

class Loader final : public ServiceObject {
  public:
    std::atomic<u64> process_id{0};
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        static constexpr CommandTable commands({
            MakeCommand<Loader, LoaderRequest, NoData, 0, 1,
                        [](Loader &, const LoaderRequest &in, NoData &, auto &args) -> Result {
                            const auto verif = args.Out(0);
                            if (verif.GetSize())
                                std::memset(verif.GetPointer(), 0, verif.GetSize());
                            Fs fs;
                            const Result rc = content::OpenCodeFileSystem(&fs, {in.program_id},
                                                                          static_cast<ncm::StorageId>(in.storage_id));
                            if (R_FAILED(rc)) {
                                Log("[fs] program %016lx: no code (rc=0x%x)\n", in.program_id, rc);
                                return rc;
                            }
                            return ReturnFileSystem(args.reply, std::move(fs));
                        }>(0, Mapped),
            MakeCommand<Loader, u64, u8, 0, 0, [](Loader &, u64, u8 &, auto &) -> Result { return 0; }>(1),
            SetProcess<Loader>(2),
        });
        static constexpr CommandTable legacy({
            MakeCommand<Loader, LegacyLoaderRequest, NoData, 1, 1,
                        [](Loader &, const LegacyLoaderRequest &in, NoData &, auto &args) {
                            const auto verif = args.Out(0);
                            if (verif.GetSize())
                                std::memset(verif.GetPointer(), 0, verif.GetSize());
                            return OpenContent(args.reply, args.In(0), impl::FileSystemProxyType_Code, in.program_id);
                        }>(
                0,
                {.buffers = BufferMode::FixedPointerInput, .input_pointer_size = PathBytes, .normal_map_alias = true}),
        });
        const u32 id = request.input.header.command_id;
        if (id == 0 && hosversionBefore(20, 0, 0)) {
            if (hosversionAtLeast(17, 0, 0)) {
                return hosversionGet() <= MAKEHOSVERSION(19, 0, 1) && legacy.Dispatch(this, request, reply);
            }
            reply.result = ResultNotImplemented();
            return true;
        }
        if (id > 2 || (id == 2 && hosversionBefore(4, 0, 0)))
            return false;
        return commands.Dispatch(this, request, reply);
    }
};

class ProgramRegistry final : public ServiceObject {
  public:
    std::atomic<u64> process_id{0};
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        const u32 id = request.input.header.command_id;
        if (id > 2 && id != 256)
            return false;
        static constexpr CommandTable commands({
            MakeCommand<ProgramRegistry, RegisterProgram, NoData, 2, 0,
                        [](ProgramRegistry &, const RegisterProgram &in, NoData &, auto &args) -> Result {
                            KUMO_UNLESS(in.data_size >= 0 && static_cast<u64>(in.data_size) <= args.In(0).GetSize(),
                                        ResultInvalidSize());
                            KUMO_UNLESS(in.desc_size >= 0 && static_cast<u64>(in.desc_size) <= args.In(1).GetSize(),
                                        ResultInvalidSize());
                            return registry::RegisterProgram(in.process_id, {in.program_id},
                                                             static_cast<ncm::StorageId>(in.storage_id));
                        }>(0, Mapped),
            MakeCommand<ProgramRegistry, u64, NoData, 0, 0,
                        [](ProgramRegistry &, u64 pid, NoData &, auto &) { return registry::UnregisterProgram(pid); }>(
                1),
            SetProcess<ProgramRegistry>(2),
            MakeCommand<ProgramRegistry, u8, NoData, 0, 0,
                        [](ProgramRegistry &, u8, NoData &, auto &) { return ResultNotImplemented(); }>(256),
        });
        return commands.Dispatch(this, request, reply);
    }
};
}

ObjectRef OpenFileSystemProxy(void *) { return MakeObject<Proxy>(); }
ObjectRef OpenLoaderProxy(void *) { return MakeObject<Loader>(); }
ObjectRef OpenProgramRegistry(void *) { return MakeObject<ProgramRegistry>(); }
}
