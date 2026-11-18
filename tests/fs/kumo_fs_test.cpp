#include <kumo/fs/nca.hpp>
#include <kumo/fs/partition_fs.hpp>
#include <kumo/fs/rom_fs.hpp>
#include <kumo/fs/storage_service.hpp>
#include <kumo/ipc/dispatch.hpp>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

void sha256CalculateHash(void *out, const void *data, size_t size) {
#ifdef __APPLE__
    assert(size <= UINT32_MAX);
    CC_SHA256(data, static_cast<CC_LONG>(size), static_cast<unsigned char *>(out));
#else
    SHA256(static_cast<const unsigned char *>(data), size, static_cast<unsigned char *>(out));
#endif
}

Result svcCloseHandle(Handle) { return 0; }

using namespace kumo::fs;

class Memory final : public Storage {
public:
    explicit Memory(const char *path) {
        std::ifstream stream(path, std::ios::binary);
        for (std::istreambuf_iterator<char> it(stream), end; it != end; ++it) bytes.push_back(std::byte(*it));
        assert(!bytes.empty());
    }

    Result Read(u64 offset, std::span<std::byte> out) override {
        if (!Contains(bytes.size(), offset, out.size())) return OutOfRange;
        std::copy_n(bytes.begin() + offset, out.size(), out.begin());
        return 0;
    }

    Result Write(u64 offset, std::span<const std::byte> in) override {
        if (!Contains(bytes.size(), offset, in.size())) return OutOfRange;
        std::copy(in.begin(), in.end(), bytes.begin() + offset);
        return 0;
    }

    Result GetSize(u64 &size) override {
        size = bytes.size();
        return 0;
    }

    Result QueryRange(s64 offset, s64 size, RangeInfo &info) override {
        assert(offset == 17 && size == 23);
        info.aes_ctr_key_type = 4;
        info.speed_emulation_type = 8;
        info.reserved[0] = 0xff;
        return 0;
    }

    std::vector<std::byte> bytes;
};

void CheckPackage(Storage &storage) {
    PartitionFileSystem fs;
    assert(fs.Open(storage, 0) == AllocationFailed);
    assert(fs.Open(storage, 4096) == 0);
    assert(fs.EntryCount() == 2);
    PartitionFileSystem::Directory directory;
    assert(fs.OpenDirectory("/", directory, false) == 0 && directory.EntryCount() == 0);
    assert(fs.OpenDirectory("/main", directory) == PathNotFound);
    assert(fs.OpenDirectory("/", directory) == 0 && directory.EntryCount() == 2);
    PartitionFileSystem::Entry entry;
    assert(fs.GetEntry(0, entry) == 0 && entry.name == "main" && entry.size == 5000);
    assert(fs.GetEntry(1, entry) == 0 && entry.name == "empty" && entry.size == 0);
    assert(fs.GetEntry(2, entry) == OutOfRange);
    bool found;
    assert(directory.Next(entry, found) == 0 && found && entry.name == "main");
    PartitionFileSystem::File file;
    assert(fs.OpenFile("/missing", file) == PathNotFound);
    assert(fs.OpenFile("/main/child", file) == PathNotFound);
    assert(fs.OpenFile("/main", file) == 0);
    std::array<std::byte, 5000> bytes;
    assert(file.Read(0, bytes) == 0);
    for (size_t i = 0; i < bytes.size(); ++i) assert(bytes[i] == std::byte(i % 251));
    assert(file.Read(1, bytes) == OutOfRange);
    assert(file.Read(UINT64_MAX, bytes) == OutOfRange);
    size_t read;
    assert(file.ReadFile(4999, bytes, read) == 0 && read == 1 && bytes[0] == std::byte(4999 % 251));
    assert(file.ReadFile(5000, bytes, read) == 0 && read == 0);
    assert(fs.OpenFile("/empty", file) == 0);
    assert(file.ReadFile(0, bytes, read) == 0 && read == 0);
}

void CheckStorageWire(Memory &storage) {
    using namespace kumo::ipc;
    StorageService service(storage);
    alignas(16) unsigned char tls[256]{};
    u8 buffer[8]{};
    const auto dispatch = [&](ConcurrentReply &reply) {
        ConcurrentRequest request{};
        assert(DecodeCmif(tls, sizeof(tls), request.input));
        request.data = tls + request.input.data_offset;
        return service.Dispatch(request, reply);
    };
    CmifRequestFormat format{};
    format.data_size = 16;
    format.num_out_buffers = 1;
    auto wire = cmifMakeRequest(tls, format);
    cmifRequestOutBuffer(&wire, buffer, sizeof(buffer), HipcBufferMode_Normal);
    ConcurrentReply reply{};
    assert(!dispatch(reply));
    wire = cmifMakeRequest(tls, format);
    cmifRequestOutBuffer(&wire, buffer, sizeof(buffer), HipcBufferMode_NonSecure);
    const u8 transfer[16] = {1, 0, 0, 0, 0, 0, 0, 0, 3};
    std::memcpy(wire.data, transfer, sizeof(transfer));
    assert(dispatch(reply) && reply.result == 0 && std::memcmp(buffer, "FS0", 3) == 0);
    static_cast<u8 *>(wire.data)[8] = 9;
    assert(dispatch(reply) && reply.result == InvalidSize);
    static_cast<u8 *>(wire.data)[7] = 0x80;
    assert(dispatch(reply) && reply.result == InvalidOffset);

    format = {};
    format.request_id = 5;
    format.data_size = 24;
    wire = cmifMakeRequest(tls, format);
    const u8 query[24] = {3, 0, 0, 0, 0, 0, 0, 0, 17, 0, 0, 0, 0, 0, 0, 0, 23};
    std::memcpy(wire.data, query, sizeof(query));
    reply = {};
    assert(dispatch(reply) && reply.result == 0 && reply.size == 0x40);
    const u8 expected[0x40] = {4, 0, 0, 0, 8};
    assert(std::memcmp(reply.data, expected, sizeof(expected)) == 0);
}

void CheckMixedBuffers() {
    using namespace kumo::ipc;
    static constexpr CommandTable table({
        MakeCommand<int, NoData, NoData, 1, 1,
            [](int &called, const NoData &, NoData &, Arguments<1, 1> &args) -> Result {
                assert(args.In(0).GetSize() == 0x301 && args.Out(0).GetSize() == 4);
                assert(std::memcmp(args.In(0).GetPointer(), "/main", 6) == 0);
                std::memcpy(args.Out(0).GetPointer(), "test", 4);
                ++called;
                return 0;
            }>(0, {.buffers = BufferMode::FixedPointerInput, .input_pointer_size = 0x301, .normal_map_alias = true}),
    });
    alignas(16) unsigned char tls[256]{}, pointers[0x800]{};
    unsigned char output[4]{};
    std::memcpy(pointers, "/main", 6);
    CmifRequestFormat format{};
    format.num_in_pointers = 1;
    format.num_out_buffers = 1;
    auto wire = cmifMakeRequest(tls, format);
    cmifRequestInPointer(&wire, pointers, 0x301);
    cmifRequestOutBuffer(&wire, output, sizeof(output), HipcBufferMode_Normal);
    ConcurrentRequest request{};
    assert(DecodeCmif(tls, sizeof(tls), request.input));
    request.data = tls + request.input.data_offset;
    request.pointer_buffer = pointers;
    request.pointer_buffer_size = sizeof(pointers);
    ConcurrentReply reply{};
    int called = 0;
    assert(Dispatch(&called, table, request, reply));
    assert(called == 1 && std::memcmp(output, "test", 4) == 0);
    request.input.hipc.data.recv_buffers[0].mode = HipcBufferMode_NonSecure;
    assert(!Dispatch(&called, table, request, reply));
    request.input.hipc.data.recv_buffers[0].mode = HipcBufferMode_Normal;
    request.pointer_buffer_size = 0x300;
    assert(!Dispatch(&called, table, request, reply) && called == 1);

    static constexpr CommandTable query_entry({
        MakeCommand<int, s32, NoData, 2, 1,
            [](int &calls, const s32 &id, NoData &, Arguments<2, 1> &args) -> Result {
                assert(id == 7 && args.In(0).GetSize() == 0x301);
                assert(std::memcmp(args.In(0).GetPointer(), "/main", 6) == 0);
                assert(args.In(1).GetSize() == 4 && args.Out(0).GetSize() == 4);
                std::memcpy(args.Out(0).GetPointer(), args.In(1).GetPointer(), 4);
                ++calls;
                return 0;
            }>(15, {.buffers = BufferMode::FixedPointerAndNonSecureMapped, .input_pointer_size = 0x301}),
    });
    format.request_id = 15;
    format.num_in_buffers = 1;
    format.data_size = 4;
    const unsigned char id[4]{7};
    const auto query = [&](HipcBufferMode mode) {
        auto message = cmifMakeRequest(tls, format);
        cmifRequestInBuffer(&message, "data", 4, mode);
        cmifRequestInPointer(&message, pointers, 0x301);
        cmifRequestOutBuffer(&message, output, sizeof(output), HipcBufferMode_NonSecure);
        std::memcpy(message.data, id, sizeof(id));
        assert(DecodeCmif(tls, sizeof(tls), request.input));
        request.data = tls + request.input.data_offset;
        request.pointer_buffer_size = sizeof(pointers);
        return Dispatch(&called, query_entry, request, reply);
    };
    assert(!query(HipcBufferMode_Normal) && called == 1);
    assert(query(HipcBufferMode_NonSecure) && called == 2 && std::memcmp(output, "data", 4) == 0);
}

void CheckRom(Storage &storage) {
    RomFileSystem fs;
    assert(fs.Open(storage, 1) == AllocationFailed);
    assert(fs.Open(storage, 4096) == 0);
    EntryType type;
    assert(fs.GetEntryType("/", type) == 0 && type == EntryType::Directory);
    assert(fs.GetEntryType("/sub/data", type) == 0 && type == EntryType::File);
    assert(fs.GetEntryType("/data", type) == PathNotFound);
    RomFileSystem::Directory cursor;
    assert(fs.OpenDirectory("/", cursor, false, true) == 0);
    RomFileSystem::Entry entry;
    bool found;
    assert(cursor.Next(entry, found) == 0 && !found);
    assert(fs.OpenDirectory("/", cursor) == 0);
    u64 count;
    assert(cursor.GetEntryCount(count) == 0 && count == 1);
    assert(cursor.Next(entry, found) == 0 && found && entry.name == "sub" && entry.type == EntryType::Directory);
    assert(cursor.Next(entry, found) == 0 && !found);
    assert(cursor.GetEntryCount(count) == 0 && count == 1);
    assert(fs.OpenDirectory("/sub", cursor) == 0);
    assert(cursor.Next(entry, found) == 0 && found && entry.name == "data" && entry.size == 16);
    FileView file;
    assert(fs.OpenFile("/sub/data", file) == 0);
    std::byte output[32];
    size_t read;
    assert(file.ReadFile(0, output, read) == 0 && read == 16);
    assert(std::memcmp(output, "hello from RomFS", read) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 7);
    Memory package(argv[1]);
    CheckPackage(package);
    CheckStorageWire(package);
    CheckMixedBuffers();
    PartitionFileSystem writable;
    PartitionFileSystem::File file;
    assert(writable.Open(package, 4096) == 0);
    assert(writable.OpenFile("/main", file, OpenMode::ReadWrite) == 0);
    const std::byte sample{0x99};
    assert(file.Write(0, {&sample, 1}) == 0);
    std::byte actual;
    assert(file.Read(0, {&actual, 1}) == 0 && actual == sample);
    assert(file.Write(5000, {&sample, 1}) == Error(6201));
    assert(file.SetSize(5001) == Error(6376));
    StorageSlice view;
    assert(view.Open(package, UINT64_MAX, 1) == OutOfRange);
    assert(view.Open(package, 1, 3) == 0);
    const std::byte byte{0x43};
    assert(view.Write(2, {&byte, 1}) == 0 && package.bytes[3] == byte);
    assert(view.Write(3, {&byte, 1}) == OutOfRange);
    PartitionFileSystem fs;
    assert(fs.Open(package, 4096) == Error(4644));
    package.bytes[3] = std::byte{'0'};
    const auto original = package.bytes;
    for (size_t offset : {0x10u, 0x20u, 0x28u}) {
        package.bytes[offset] = std::byte{0xff};
        assert(fs.Open(package, 4096) == Error(4646));
        package.bytes = original;
    }

    Memory image(argv[2]);
    PlaintextNca reader;
    assert(reader.Open(image) == 0);
    assert(reader.ProgramId() == 0x0100000000001000 && reader.Type() == PlaintextNca::ContentType::Program);
    assert(reader.HasSection(0) && !reader.HasSection(1) && !reader.HasSection(UINT32_MAX));
    PlaintextNca::Section section;
    const PlaintextNca::Limits limits{0x8000, 0x1000};
    assert(reader.OpenSection(1, limits, section) == OutOfRange);
    assert(reader.OpenSection(0, {63, 4096}, section) == AllocationFailed);
    assert(reader.OpenSection(0, limits, section) == 0);
    assert(section.Type() == PlaintextNca::FileSystemType::PartitionFs);
    CheckPackage(section);

    image.bytes[0x400] ^= std::byte{1};
    assert(reader.OpenSection(0, limits, section) == NcaFsHeaderHashFailed);
    image.bytes[0x400] ^= std::byte{1};
    image.bytes[0xc00] ^= std::byte{1};
    assert(reader.OpenSection(0, limits, section) == NcaDataHashFailed);
    image.bytes[0xc00] ^= std::byte{1};

    // The corruption is outside the requested byte range, in its second block.
    image.bytes[0xe00 + 4200] ^= std::byte{1};
    std::byte crossing[2]{std::byte{0xff}, std::byte{0xff}};
    assert(section.Read(4095, crossing) == NcaDataHashFailed);
    assert(crossing[0] == std::byte{} && crossing[1] == std::byte{});
    image.bytes[0x244] = std::byte{0xff};
    assert(reader.Open(image) == Error(4510));
    Memory romfs(argv[3]);
    CheckRom(romfs);
    RomFileSystem malformed;
    romfs.bytes[0x18] = std::byte{0xff}; // Directory table points past EOF.
    assert(malformed.Open(romfs, 4096) == Error(4000));

    Memory ivfc(argv[4]);
    assert(reader.Open(ivfc) == 0);
    assert(reader.OpenSection(0, {0x8000, 512}, section) == AllocationFailed);
    assert(reader.OpenSection(0, limits, section) == 0);
    assert(section.Type() == PlaintextNca::FileSystemType::RomFs);
    CheckRom(section);
    std::byte first;
    ivfc.bytes[0xc00] ^= std::byte{1};
    assert(section.Read(0, {&first, 1}) == Error(4604));
    ivfc.bytes[0xc00] ^= std::byte{1};
    ivfc.bytes[0xe50] ^= std::byte{1};
    assert(section.Read(0, {&first, 1}) == Error(4613));
    ivfc.bytes[0xe50] ^= std::byte{1};
    auto *fs_header = ivfc.bytes.data() + 0x400;
    fs_header[0x28] = std::byte{0xff}; // Invalid block order in authenticated metadata.
    sha256CalculateHash(ivfc.bytes.data() + 0x280, fs_header, 0x200);
    assert(reader.Open(ivfc) == 0);
    assert(reader.OpenSection(0, limits, section) == Error(4529));
    Memory compressed(argv[5]);
    assert(reader.Open(compressed) == 0);
    assert(reader.OpenSection(0, {0x7fff, 4096}, section) == AllocationFailed);
    assert(reader.OpenSection(0, {0x10000, 4096}, section) == 0);
    PartitionFileSystem mapped;
    assert(mapped.Open(section, 4096) == 0);
    assert(mapped.OpenFile("/mixed", file) == 0);
    const char expected[] = "123\0\0\0\0\0\0\0AAAAAAAAAAAabcde";
    std::byte decoded[sizeof(expected) - 1];
    assert(file.Read(0, decoded) == 0 && std::memcmp(decoded, expected, sizeof(decoded)) == 0);
    // The compressed bytes and the index are both behind SHA-256 verification.
    compressed.bytes[0xe00 + 0x60] ^= std::byte{1};
    std::fill(std::begin(decoded), std::end(decoded), std::byte{0xff});
    assert(file.Read(0, decoded) == NcaDataHashFailed);
    assert(std::all_of(std::begin(decoded), std::end(decoded), [](std::byte b) { return b == std::byte{}; }));
    compressed.bytes[0xe00 + 0x60] ^= std::byte{1};
    compressed.bytes[0xe00 + 0x200 + 0x4000] ^= std::byte{1};
    assert(reader.OpenSection(0, {0x10000, 4096}, section) == NcaDataHashFailed);
    compressed.bytes[0xe00 + 0x200 + 0x4000] ^= std::byte{1};
    assert(file.Read(0, decoded) == 0); // Failed reopen preserved the live reader.

    Memory patch(argv[6]);
    assert(reader.Open(patch) == 0);
    assert(reader.OpenSection(0, {0x10000, 4096}, section) == 0);
    CheckPackage(section);
    patch.bytes[0xc00 + 0x400] ^= std::byte{1};
    assert(section.Read(0x200, {&first, 1}) == NcaDataHashFailed);
    std::puts("Kumo storage, PFS0, RomFS, plaintext NCA integrity, mapping and storage IPC tests passed");
}
