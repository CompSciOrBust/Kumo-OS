#include "fs_objects.hpp"

namespace kumo::horizon::hzfs::ipc {
namespace {
using namespace kumo::horizon::fs;
using namespace kumo::horizon::fs::fsa;

struct Transfer {
    static constexpr size_t WireBytes = 24;
    u32 option, pad;
    s64 offset, size;
    Result Check(size_t capacity) const {
        KUMO_UNLESS(offset >= 0, ResultInvalidOffset());
        KUMO_UNLESS(size >= 0 && static_cast<u64>(size) <= capacity, ResultInvalidSize());
        return 0;
    }
};

struct RangeRequest {
    static constexpr size_t WireBytes = 24;
    s32 operation;
    u32 pad;
    s64 offset, size;
    OperationId Operation() const { return static_cast<OperationId>(operation); }
};

class File final : public ServiceObject {
    std::shared_ptr<IFileSystem> owner_;
    std::unique_ptr<IFile> file_;
    kumo::Mutex mutex_;

  public:
    File(std::shared_ptr<IFileSystem> owner, std::unique_ptr<IFile> file)
        : owner_(std::move(owner)), file_(std::move(file)) {}
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        const u32 id = request.input.header.command_id;
        if (id > 6 || (id == 5 && hosversionBefore(4, 0, 0)) || (id == 6 && hosversionBefore(12, 0, 0)))
            return false;
        std::scoped_lock lock(mutex_);
        static constexpr CommandTable commands({
            MakeCommand<File, Transfer, s64, 0, 1,
                        [](File &self, const Transfer &in, s64 &out, auto &args) -> Result {
                            auto buffer = args.Out(0);
                            KUMO_TRY(in.Check(buffer.GetSize()));
                            size_t count;
                            KUMO_TRY(self.file_->Read(&count, in.offset, buffer.GetPointer(), in.size,
                                                      static_cast<ReadOption>(in.option)));
                            out = count;
                            return 0;
                        }>(0, NonSecure),
            MakeCommand<File, Transfer, NoData, 1, 0,
                        [](File &self, const Transfer &in, NoData &, auto &args) -> Result {
                            auto buffer = args.In(0);
                            KUMO_TRY(in.Check(buffer.GetSize()));
                            return self.file_->Write(in.offset, buffer.GetPointer(), in.size,
                                                     static_cast<WriteOption>(in.option));
                        }>(1, NonSecure),
            MakeCommand<File, NoData, NoData, 0, 0,
                        [](File &self, const NoData &, NoData &, auto &) { return self.file_->Flush(); }>(2),
            MakeCommand<File, s64, NoData, 0, 0,
                        [](File &self, s64 in, NoData &, auto &) { return self.file_->SetSize(in); }>(3),
            MakeCommand<File, NoData, s64, 0, 0,
                        [](File &self, const NoData &, s64 &out, auto &) { return self.file_->GetSize(&out); }>(4),
            MakeCommand<File, RangeRequest, QueryRangeInfo, 0, 0,
                        [](File &self, const RangeRequest &in, QueryRangeInfo &out, auto &) -> Result {
                            if (in.Operation() == OperationId::QueryRange) {
                                QueryRangeInfo info{};
                                KUMO_TRY(
                                    self.file_->OperateRange(&info, sizeof(info), in.Operation(), in.offset, in.size));
                                out.aes_ctr_key_type = info.aes_ctr_key_type;
                                out.speed_emulation_type = info.speed_emulation_type;
                            } else if (in.Operation() == OperationId::Invalidate) {
                                return self.file_->OperateRange(nullptr, 0, in.Operation(), in.offset, in.size);
                            }
                            return 0;
                        }>(5),
            MakeCommand<File, RangeRequest, NoData, 1, 1,
                        [](File &self, const RangeRequest &in, NoData &, auto &args) {
                            const auto dst = args.Out(0);
                            const auto src = args.In(0);
                            return self.file_->OperateRange(dst.GetPointer(), dst.GetSize(), in.Operation(), in.offset,
                                                            in.size, src.GetPointer(), src.GetSize());
                        }>(6, NonSecure),
        });
        return commands.Dispatch(this, request, reply);
    }
};

class Directory final : public ServiceObject {
    std::shared_ptr<IFileSystem> owner_;
    std::unique_ptr<IDirectory> dir_;
    kumo::Mutex mutex_;

  public:
    Directory(std::shared_ptr<IFileSystem> owner, std::unique_ptr<IDirectory> dir)
        : owner_(std::move(owner)), dir_(std::move(dir)) {}
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        if (request.input.header.command_id > 1)
            return false;
        std::scoped_lock lock(mutex_);
        static constexpr CommandTable commands({
            MakeCommand<Directory, NoData, s64, 0, 1,
                        [](Directory &self, const NoData &, s64 &out, auto &args) -> Result {
                            auto buf = args.Out(0);
                            const size_t capacity = buf.GetSize() / sizeof(DirectoryEntry);
                            // A mapped address may be unaligned. Keep bounded aligned
                            // scratch while preserving the cursor for each returned entry.
                            std::array<DirectoryEntry, 8> entries;
                            while (static_cast<size_t>(out) < capacity) {
                                const size_t batch = std::min(entries.size(), capacity - static_cast<size_t>(out));
                                s64 count;
                                KUMO_TRY(self.dir_->Read(&count, entries.data(), batch));
                                if (count == 0)
                                    break;
                                std::memcpy(buf.GetPointer() + out * sizeof(DirectoryEntry), entries.data(),
                                            count * sizeof(DirectoryEntry));
                                out += count;
                                if (static_cast<size_t>(count) < batch)
                                    break;
                            }
                            return 0;
                        }>(0, Mapped),
            MakeCommand<Directory, NoData, s64, 0, 0,
                        [](Directory &self, const NoData &, s64 &out, auto &) {
                            return self.dir_->GetEntryCount(&out);
                        }>(1),
        });
        return commands.Dispatch(this, request, reply);
    }
};

struct CreateFile {
    static constexpr size_t WireBytes = 16;
    s32 options;
    u32 pad;
    s64 size;
};

class FileSystem final : public ServiceObject {
    std::shared_ptr<IFileSystem> fs_;
    kumo::Mutex mutex_;

    template <auto Method> static consteval auto PathCommand(u32 id) {
        return MakeCommand<FileSystem, NoData, NoData, 1, 0,
                           [](FileSystem &self, const NoData &, NoData &, auto &args) -> Result {
                               Path path;
                               KUMO_TRY(DecodePath(args.In(0), path));
                               return ((*self.fs_).*Method)(path);
                           }>(id, PathBuffer);
    }

    template <auto Method> static consteval auto RenameCommand(u32 id) {
        return MakeCommand<FileSystem, NoData, NoData, 2, 0,
                           [](FileSystem &self, const NoData &, NoData &, auto &args) -> Result {
                               Path from, to;
                               KUMO_TRY(DecodePath(args.In(0), from));
                               KUMO_TRY(DecodePath(args.In(1), to));
                               return ((*self.fs_).*Method)(from, to);
                           }>(id, PathBuffer);
    }

    template <class Output, auto Method> static consteval auto QueryPath(u32 id) {
        return MakeCommand<FileSystem, NoData, Output, 1, 0,
                           [](FileSystem &self, const NoData &, Output &out, auto &args) -> Result {
                               Path path;
                               KUMO_TRY(DecodePath(args.In(0), path));
                               return ((*self.fs_).*Method)(&out, path);
                           }>(id, PathBuffer);
    }

    template <class Object, class Backend, auto Method> static consteval auto OpenPath(u32 id) {
        return MakeCommand<FileSystem, u32, NoData, 1, 0,
                           [](FileSystem &self, u32 mode, NoData &, auto &args) -> Result {
                               Path path;
                               KUMO_TRY(DecodePath(args.In(0), path));
                               std::unique_ptr<Backend> backend;
                               KUMO_TRY(((*self.fs_).*Method)(&backend, path, mode));
                               return ReturnObject<Object>(args.reply, self.fs_, std::move(backend));
                           }>(id, PathBuffer);
    }

  public:
    explicit FileSystem(std::shared_ptr<IFileSystem> fs) : fs_(std::move(fs)) {}
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        const u32 id = request.input.header.command_id;
        if (id > 15 || ((id == 13 || id == 14) && hosversionBefore(3, 0, 0)) || (id == 15 && hosversionBefore(4, 0, 0)))
            return false;
        std::scoped_lock lock(mutex_);
        static constexpr CommandTable commands({
            MakeCommand<FileSystem, CreateFile, NoData, 1, 0,
                        [](FileSystem &self, const CreateFile &in, NoData &, auto &args) -> Result {
                            Path path;
                            KUMO_TRY(DecodePath(args.In(0), path));
                            return self.fs_->CreateFile(path, in.size, in.options);
                        }>(0, PathBuffer),
            PathCommand<&IFileSystem::DeleteFile>(1),
            PathCommand<&IFileSystem::CreateDirectory>(2),
            PathCommand<&IFileSystem::DeleteDirectory>(3),
            PathCommand<&IFileSystem::DeleteDirectoryRecursively>(4),
            RenameCommand<&IFileSystem::RenameFile>(5),
            RenameCommand<&IFileSystem::RenameDirectory>(6),
            MakeCommand<FileSystem, NoData, u32, 1, 0,
                        [](FileSystem &self, const NoData &, u32 &out, auto &args) -> Result {
                            Path path;
                            KUMO_TRY(DecodePath(args.In(0), path));
                            DirectoryEntryType type;
                            KUMO_TRY(self.fs_->GetEntryType(&type, path));
                            out = type;
                            return 0;
                        }>(7, PathBuffer),
            OpenPath<File, IFile, &IFileSystem::OpenFile>(8),
            OpenPath<Directory, IDirectory, &IFileSystem::OpenDirectory>(9),
            MakeCommand<FileSystem, NoData, NoData, 0, 0,
                        [](FileSystem &self, const NoData &, NoData &, auto &) { return self.fs_->Commit(); }>(10),
            QueryPath<s64, &IFileSystem::GetFreeSpaceSize>(11),
            QueryPath<s64, &IFileSystem::GetTotalSpaceSize>(12),
            PathCommand<&IFileSystem::CleanDirectoryRecursively>(13),
            QueryPath<FileTimeStampRaw, &IFileSystem::GetFileTimeStampRaw>(14),
            MakeCommand<FileSystem, s32, NoData, 2, 1,
                        [](FileSystem &, s32, NoData &, auto &args) -> Result {
                            Path path;
                            KUMO_TRY(DecodePath(args.In(0), path));
                            return ResultNotImplemented();
                        }>(15,
                           {.buffers = BufferMode::FixedPointerAndNonSecureMapped, .input_pointer_size = PathBytes}),
        });
        return commands.Dispatch(this, request, reply);
    }
};
}

Result ReturnFileSystem(ConcurrentReply &reply, std::shared_ptr<fs::fsa::IFileSystem> fs) {
    return ReturnObject<FileSystem>(reply, std::move(fs));
}
}
