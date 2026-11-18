#include "fs_save_filesystem.hpp"
#include <vector>

namespace kumo::horizon::hzfs::savedata {
namespace {
using namespace kumo::horizon::fs;
using namespace kumo::horizon::fs::fsa;

Result Join(Path &out, const Path &dir, const char *name) {
    char text[0x301];
    const int size = std::snprintf(text, sizeof(text), "%s/%s", dir.GetString(), name);
    KUMO_UNLESS(size >= 0 && static_cast<size_t>(size) < sizeof(text), ResultTooLongPath());
    return out.InitializeWithNormalization(text);
}

Result CopyFile(IFileSystem &fs, const Path &target, const Path &source, s64 extent, std::span<std::byte> buffer) {
    std::unique_ptr<IFile> src, dst;
    KUMO_TRY(fs.OpenFile(&src, source, OpenMode_Read));
    KUMO_TRY(fs.CreateFile(target, extent, 0));
    KUMO_TRY(fs.OpenFile(&dst, target, OpenMode_Write));
    for (s64 offset = 0; offset < extent;) {
        const size_t size = std::min<u64>(extent - offset, buffer.size());
        size_t read;
        KUMO_TRY(src->Read(&read, offset, buffer.data(), size, ReadOption::None));
        KUMO_UNLESS(read == size, ResultUnexpected());
        KUMO_TRY(dst->Write(offset, buffer.data(), size, WriteOption::None));
        offset += size;
    }
    return dst->Flush();
}

Result CopyTree(IFileSystem &fs, const Path &target, const Path &source, std::span<std::byte> buffer) {
    struct Frame {
        Path source, target;
        std::unique_ptr<IDirectory> dir;
    };
    try {
        std::vector<Frame> stack;
        auto descend = [&](const Path &from, const Path &to) -> Result {
            Frame frame{from, to, {}};
            KUMO_TRY(fs.OpenDirectory(&frame.dir, from, OpenDirectoryMode_All));
            stack.push_back(std::move(frame));
            return 0;
        };
        KUMO_TRY(descend(source, target));
        while (!stack.empty()) {
            const Frame &current = stack.back();
            DirectoryEntry entry;
            s64 count;
            KUMO_TRY(current.dir->Read(&count, &entry, 1));
            if (count == 0) {
                stack.pop_back();
                continue;
            }
            Path from, to;
            KUMO_TRY(Join(from, current.source, entry.name));
            KUMO_TRY(Join(to, current.target, entry.name));
            if (entry.type == DirectoryEntryType_Directory) {
                KUMO_TRY(fs.CreateDirectory(to));
                KUMO_TRY(descend(from, to));
            } else {
                KUMO_TRY(CopyFile(fs, to, from, entry.file_size, buffer));
            }
        }
        return 0;
    } catch (const std::bad_alloc &) {
        return ResultAllocationMemoryFailed();
    }
}

Result RetryLocked(const auto &operation) {
    Result rc = operation();
    for (int attempt = 0; attempt < 10 && rc == ResultTargetLocked(); ++attempt) {
        svcSleepThread(100'000'000);
        rc = operation();
    }
    return rc;
}

struct Journal {
    std::shared_ptr<IFileSystem> root;
    std::unique_ptr<IFile> lock;
    std::atomic<size_t> writers{0};
    Path working, committed, staging;
    explicit Journal(std::shared_ptr<IFileSystem> fs) : root(std::move(fs)) {}

    Result Copy(const Path &target, const Path &source) {
        constexpr size_t BufferSize = 32 * 1024;
        auto buffer = std::unique_ptr<std::byte[]>(new (std::nothrow) std::byte[BufferSize]);
        KUMO_UNLESS(buffer, ResultAllocationMemoryFailed());
        const Result rc = root->DeleteDirectoryRecursively(target);
        if (rc != ResultPathNotFound())
            KUMO_TRY(rc);
        KUMO_TRY(root->CreateDirectory(target));
        KUMO_TRY(CopyTree(*root, target, source, {buffer.get(), BufferSize}));
        return root->Flush();
    }

    Result Initialize() {
        Path lock_path;
        KUMO_TRY(lock_path.InitializeWithNormalization("/.lock"));
        KUMO_TRY(working.InitializeWithNormalization("/1"));
        KUMO_TRY(committed.InitializeWithNormalization("/0"));
        KUMO_TRY(staging.InitializeWithNormalization("/_"));

        Result rc = root->OpenFile(&lock, lock_path, OpenMode_ReadWrite);
        if (rc == ResultPathNotFound()) {
            KUMO_TRY(root->CreateFile(lock_path, 0, 0));
            rc = root->OpenFile(&lock, lock_path, OpenMode_ReadWrite);
        }
        KUMO_TRY(rc);
        DirectoryEntryType type;
        rc = root->GetEntryType(&type, working);
        if (rc == ResultPathNotFound())
            KUMO_TRY(root->CreateDirectory(working));
        else
            KUMO_TRY(rc);

        rc = root->GetEntryType(&type, committed);
        if (rc == ResultPathNotFound()) {
            KUMO_TRY(Copy(staging, working));
            KUMO_TRY(root->RenameDirectory(staging, committed));
            return root->Flush();
        }
        KUMO_TRY(rc);
        return Copy(working, committed);
    }

    Result Commit() {
        KUMO_UNLESS(writers.load() == 0, ResultWriteModeFileNotClosed());
        DirectoryEntryType type;
        Result rc = root->GetEntryType(&type, committed);
        if (rc != ResultPathNotFound()) {
            KUMO_TRY(rc);
            const Result remove = root->DeleteDirectoryRecursively(staging);
            if (remove != ResultPathNotFound())
                KUMO_TRY(remove);
            KUMO_TRY(RetryLocked([&] { return root->RenameDirectory(committed, staging); }));
        }
        KUMO_TRY(RetryLocked([&] { return Copy(staging, working); }));
        KUMO_TRY(RetryLocked([&] { return root->RenameDirectory(staging, committed); }));
        return root->Flush();
    }
};

class JournalFile final : public IFile {
    std::shared_ptr<Journal> journal_;
    std::unique_ptr<IFile> file_;
    bool writable_;

  public:
    JournalFile(std::shared_ptr<Journal> journal, std::unique_ptr<IFile> file, bool writable)
        : journal_(std::move(journal)), file_(std::move(file)), writable_(writable) {
        if (writable_)
            ++journal_->writers;
    }
    ~JournalFile() override {
        file_.reset();
        if (writable_)
            --journal_->writers;
    }

  private:
    Result DoRead(size_t *out, s64 offset, void *buf, size_t size, const ReadOption &option) override {
        return file_->Read(out, offset, buf, size, option);
    }
    Result DoWrite(s64 offset, const void *buf, size_t size, const WriteOption &option) override {
        return file_->Write(offset, buf, size, option);
    }
    Result DoFlush() override { return file_->Flush(); }
    Result DoSetSize(s64 size) override { return file_->SetSize(size); }
    Result DoGetSize(s64 *out) override { return file_->GetSize(out); }
    Result DoOperateRange(void *out, size_t n, OperationId id, s64 offset, s64 size, const void *in,
                          size_t m) override {
        return file_->OperateRange(out, n, id, offset, size, in, m);
    }
};

class JournalFileSystem final : public SubDirectoryFileSystem {
    std::shared_ptr<Journal> journal_;
    kumo::Mutex mutex_;

  public:
    explicit JournalFileSystem(std::shared_ptr<Journal> journal)
        : SubDirectoryFileSystem(journal->root), journal_(std::move(journal)) {}
    Result Initialize() { return SubDirectoryFileSystem::Initialize(journal_->working); }

  private:
    Result DoOpenFile(std::unique_ptr<IFile> *out, const Path &path, OpenMode mode) override {
        std::scoped_lock lock(mutex_);
        Path resolved;
        KUMO_TRY(Resolve(resolved, path));
        std::unique_ptr<IFile> file;
        KUMO_TRY(base_->OpenFile(&file, resolved, mode));
        out->reset(new (std::nothrow) JournalFile(journal_, std::move(file), mode & OpenMode_Write));
        KUMO_UNLESS(*out, ResultAllocationMemoryFailed());
        return 0;
    }
    Result DoCommit() override {
        std::scoped_lock lock(mutex_);
        return journal_->Commit();
    }
};
}

Result MountJournal(std::shared_ptr<IFileSystem> *out, std::shared_ptr<IFileSystem> save_root) {
    auto journal = AllocateShared<Journal>(std::move(save_root));
    KUMO_UNLESS(journal, ResultAllocationMemoryFailed());
    KUMO_TRY(journal->Initialize());
    auto fs = AllocateShared<JournalFileSystem>(std::move(journal));
    KUMO_UNLESS(fs, ResultAllocationMemoryFailed());
    KUMO_TRY(fs->Initialize());
    *out = std::move(fs);
    return 0;
}
}
