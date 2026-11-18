#include <kumo/ipc/object_server.hpp>
#include "object_fixture.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
alignas(16) static unsigned char tls[256];
static std::vector<Handle> closed;
static Handle next_handle = 100;
void *armGetTls() { return tls; }
void mutexLock(Mutex *) {}
void mutexUnlock(Mutex *) {}
Result smInitialize() { return 0; }
void smExit() {}
u64 smEncodeName(const char *) { return 0; }
Result smRegisterService(Handle *out, u64, bool, s32) {
    *out = next_handle++;
    return 0;
}
Result svcCloseHandle(Handle h) {
    closed.push_back(h);
    return 0;
}
Result svcCreateSession(Handle *s, Handle *c, u32, u64) {
    *s = next_handle++;
    *c = next_handle++;
    return 0;
}
Result svcAcceptSession(Handle *, Handle) { return 1; }
Result svcWaitSynchronization(s32 *, const Handle *, s32, u64) { return 1; }
Result svcReplyAndReceive(s32 *, const Handle *, s32, Handle, u64) { return 1; }
using namespace kumo::ipc;
namespace kumo::ipc {
struct ObjectServerTestPeer {
    using Worker = ObjectServer::WorkerContext;
    inline static Worker worker{};
    static Result Accept(ObjectServer &s, size_t port, size_t &i) {
        return s.AddSession(next_handle++, port, MakeObject<test::Fixture>(), nullptr, true, &i);
    }
    static size_t Open(ObjectServer &s, size_t port = 0) {
        size_t i;
        assert(Accept(s, port, i) == 0);
        return i;
    }
    static bool Process(ObjectServer &s, size_t i, Worker &context = worker) { return s.Process(i, context); }
    static void Complete(ObjectServer &s, bool sent = true, Worker &context = worker) { s.CompleteReply(context.pending, sent); }
    static void Close(ObjectServer &s, size_t i) { s.CloseSession(i); }
    static size_t Child(ObjectServer &) { return worker.pending.child; }
    static void *Pointer(ObjectServer &) { return worker.pointer_buffer; }
};
} // namespace kumo::ipc
static CmifResponse Response(bool domain, u32 size = 0) {
    CmifResponse r{};
    assert(cmifParseResponse(&r, tls, domain, size) == 0);
    return r;
}
static void Request(u32 object, u32 command) {
    std::memset(tls, 0, 256);
    CmifRequestFormat f{};
    f.object_id = object;
    f.request_id = command;
    cmifMakeRequest(tls, f);
}
static u32 Convert(ObjectServer &s, size_t session) {
    cmifMakeControlRequest(tls, 0, 0);
    assert(ObjectServerTestPeer::Process(s, session));
    auto r = Response(false, 4);
    u32 id;
    std::memcpy(&id, r.data, 4);
    ObjectServerTestPeer::Complete(s);
    return id;
}
static void TestGlueBuffers(ObjectServer &server, size_t session, u32 object) {
    auto *pool = static_cast<u8 *>(ObjectServerTestPeer::Pointer(server));
    const auto process = [&] { return ObjectServerTestPeer::Process(server, session); };
    for (u32 command : {500, 501}) {
        CmifRequestFormat f{};
        f.object_id = object;
        f.request_id = command;
        f.num_in_pointers = 2;
        f.server_pointer_size = ObjectServer::PointerBytes;
        pool[0] = 3;
        pool[0xD0] = 10;
        auto request = cmifMakeRequest(tls, f);
        auto *statics = request.hipc.send_statics;
        cmifRequestInPointer(&request, pool, 0xD0);
        cmifRequestInPointer(&request, pool + 0xD0, 0xD0);
        const auto valid = statics[1];
        statics[1].index = 0;
        assert(!process());
        std::swap(statics[0], statics[1]);
        statics[0].index = 1;
        statics[1].index = 0;
        assert(!process());
        std::swap(statics[0], statics[1]);
        statics[1] = valid;
        statics[1].size--;
        assert(!process());
        statics[1] = hipcMakeSendStatic(pool + ObjectServer::PointerBytes - 0xC0, 0xD0, 1);
        assert(!process());
        statics[1] = hipcMakeSendStatic(pool + 0xC0, 0xD0, 1);
        assert(!process());
        statics[1] = valid;
        assert(process());
        s64 difference;
        std::memcpy(&difference, Response(object != 0, sizeof(difference)).data, sizeof(difference));
        assert(difference == -7);
        ObjectServerTestPeer::Complete(server);
    }
    for (u32 command : {201, 202}) {
        const bool with_rule = command == 201;
        u8 rule[0x4000]{9};
        s64 timestamps[2]{};
        const u64 calendar = 42;
        const auto make_request = [&](size_t length, u64 input) {
            CmifRequestFormat f{};
            f.object_id = object;
            f.request_id = command;
            f.data_size = sizeof(input);
            f.num_in_buffers = with_rule;
            f.num_out_pointers = 1;
            f.server_pointer_size = ObjectServer::PointerBytes;
            auto request = cmifMakeRequest(tls, f);
            std::memcpy(request.data, &input, sizeof(input));
            if (with_rule) cmifRequestInBuffer(&request, rule, sizeof(rule), HipcBufferMode_Normal);
            cmifRequestOutPointer(&request, timestamps, length);
        };
        make_request(sizeof(timestamps), calendar);
        auto *header = reinterpret_cast<HipcHeader *>(tls);
        const auto valid = *header;
        header->recv_static_mode = 0;
        assert(!process());
        *header = valid;
        header->num_data_words--;
        assert(!process());
        make_request(ObjectServer::PointerBytes + 1, calendar);
        assert(!process());
        for (size_t length : {sizeof(timestamps), sizeof(s64), size_t(0)}) {
            make_request(length, calendar);
            assert(process());
            assert(*static_cast<s32 *>(Response(object != 0, sizeof(s32)).data) == (length != 0));
            auto hipc = hipcParseRequest(tls);
            assert(hipc.meta.num_send_statics == 1);
            const auto &descriptor = hipc.data.send_statics[0];
            assert(descriptor.index == 0 && hipcGetStaticSize(&descriptor) == length);
            auto *output = hipcGetStaticAddress(&descriptor);
            assert(output == pool + ObjectServer::PointerBytes - (length ? 16 : 0));
            if (length) {
                u64 value;
                std::memcpy(&value, output, sizeof(value));
                assert(value == calendar + (with_rule ? rule[0] : 0));
                if (length == sizeof(timestamps)) {
                    std::memcpy(&value, static_cast<u8 *>(output) + sizeof(value), sizeof(value));
                    assert(value == 0);
                }
            }
            ObjectServerTestPeer::Complete(server);
        }
        make_request(sizeof(timestamps), 0);
        assert(process());
        CmifResponse error{};
        assert(cmifParseResponse(&error, tls, object != 0, 0) == MAKERESULT(Module_Libnx, LibnxError_BadInput));
        assert(hipcParseRequest(tls).meta.num_send_statics == 0);
        ObjectServerTestPeer::Complete(server);
    }
}
static void TestGlueSessionLimits() {
    ObjectServer server({56, 56, 128});
    const char *names[] = {"time:u", "time:a", "time:r"};
    const size_t caps[] = {8, 2, 1};
    size_t roots[3];
    for (size_t port = 0; port < 3; ++port) {
        assert(server.Register({names[port], caps[port], nullptr, [](void *) { return MakeObject<test::Fixture>(); }}) == 0);
        for (size_t i = 0; i < caps[port]; ++i)
            roots[port] = ObjectServerTestPeer::Open(server, port);
        size_t rejected;
        assert(ObjectServerTestPeer::Accept(server, port, rejected) != 0);
    }
    TestGlueBuffers(server, roots[2], 0);
    size_t last_child = ObjectServer::MaxSessions;
    for (size_t i = 0; i < 45; ++i) {
        const size_t root = roots[i % 3];
        if (i == 3)
            cmifMakeControlRequest(tls, 2, 0);
        else
            Request(0, 0);
        assert(ObjectServerTestPeer::Process(server, root));
        assert(*Response(false).move_handles != INVALID_HANDLE);
        last_child = ObjectServerTestPeer::Child(server);
        ObjectServerTestPeer::Complete(server);
    }
    Request(0, 0);
    assert(ObjectServerTestPeer::Process(server, roots[2]));
    CmifResponse error{};
    assert(cmifParseResponse(&error, tls, false, 0) == MAKERESULT(Module_Kernel, KernelError_OutOfResource));
    ObjectServerTestPeer::Complete(server);
    ObjectServerTestPeer::Close(server, last_child);
    size_t rejected;
    assert(ObjectServerTestPeer::Accept(server, 2, rejected) != 0);
    Request(0, 0);
    assert(ObjectServerTestPeer::Process(server, roots[2]));
    assert(*Response(false).move_handles != INVALID_HANDLE);
    ObjectServerTestPeer::Complete(server);
    // A surviving child must not prevent a new accept when a port slot opens.
    ObjectServerTestPeer::Close(server, roots[2]);
    ObjectServerTestPeer::Open(server, 2);
}
static void TestWorkerReplyStateAndCapacity() {
    ObjectServer server({8, 8, 0x4000});
    assert(server.Register({"test", 8, nullptr, [](void *) { return MakeObject<test::Fixture>(); }}) == 0);
    const size_t first = ObjectServerTestPeer::Open(server);
    const size_t second = ObjectServerTestPeer::Open(server);
    const u32 first_root = Convert(server, first);
    const u32 second_root = Convert(server, second);
    ObjectServerTestPeer::Worker first_worker, second_worker;

    // Interleave already-encoded replies for two independent domains.
    Request(first_root, 0);
    assert(ObjectServerTestPeer::Process(server, first, first_worker));
    const u32 rejected_object = *Response(true).objects;
    Request(second_root, 0);
    assert(ObjectServerTestPeer::Process(server, second, second_worker));
    const u32 published_object = *Response(true).objects;
    ObjectServerTestPeer::Complete(server, false, first_worker);
    ObjectServerTestPeer::Complete(server, true, second_worker);

    Request(rejected_object, 1);
    assert(ObjectServerTestPeer::Process(server, first));
    CmifResponse error{};
    assert(cmifParseResponse(&error, tls, true, 0) == MAKERESULT(10, 261));
    ObjectServerTestPeer::Complete(server);
    Request(published_object, 1);
    assert(ObjectServerTestPeer::Process(server, second));
    assert(Response(true).copy_handles[0] == 70);
    ObjectServerTestPeer::Complete(server);

    // A cloned session can close the target before the original reply sends.
    cmifMakeControlRequest(tls, 2, 0);
    assert(ObjectServerTestPeer::Process(server, second));
    const size_t clone = ObjectServerTestPeer::Child(server);
    ObjectServerTestPeer::Complete(server);
    Request(published_object, 1);
    assert(ObjectServerTestPeer::Process(server, second, first_worker));
    const int before_close = test::Fixture::live.load();
    cmifMakeCloseRequest(tls, published_object);
    assert(ObjectServerTestPeer::Process(server, clone, second_worker));
    ObjectServerTestPeer::Complete(server, true, second_worker);
    assert(test::Fixture::live == before_close);
    ObjectServerTestPeer::Complete(server, true, first_worker);
    assert(test::Fixture::live == before_close - 1);
    ObjectServerTestPeer::Close(server, clone);

    // Go past the default inline capacity and resolve an object in the larger table.
    u32 last = 0;
    for (size_t i = 0; i < ObjectServer::DefaultObjects; ++i) {
        Request(second_root, 0);
        assert(ObjectServerTestPeer::Process(server, second));
        last = *Response(true).objects;
        ObjectServerTestPeer::Complete(server);
    }
    Request(last, 1);
    assert(ObjectServerTestPeer::Process(server, second));
    assert(Response(true).copy_handles[1] == 71);
    ObjectServerTestPeer::Complete(server);
}

static void TestConvertedDomainLimit() {
    ObjectServer server({.sessions = 4, .domains = 4, .converted_domains = 1});
    assert(server.Register({"test", 4, nullptr, [](void *) { return MakeObject<test::Fixture>(); }}) == 0);
    const size_t first = ObjectServerTestPeer::Open(server);
    const size_t second = ObjectServerTestPeer::Open(server);
    cmifMakeControlRequest(tls, 0, 0);
    assert(ObjectServerTestPeer::Process(server, first));
    assert(Response(false, 4).data);
    ObjectServerTestPeer::Complete(server, false); // Failed reply frees the slot.
    Convert(server, second);

    cmifMakeControlRequest(tls, 0, 0);
    assert(ObjectServerTestPeer::Process(server, first));
    CmifResponse response{};
    assert(cmifParseResponse(&response, tls, false, 0) == MAKERESULT(11, 200));
    ObjectServerTestPeer::Complete(server);

    ObjectServerTestPeer::Close(server, second);
    const u32 root = Convert(server, first);
    CmifRequestFormat format{};
    format.object_id = root;
    format.context = 0x1234;
    format.request_id = 1;
    cmifMakeRequest(tls, format);
    assert(ObjectServerTestPeer::Process(server, first));
    assert(Response(true).copy_handles[0] == 70);
    ObjectServerTestPeer::Complete(server);
}

int main() {
    TestConvertedDomainLimit();
    assert(test::Fixture::live == 0);
    TestWorkerReplyStateAndCapacity();
    assert(test::Fixture::live == 0);
    TestGlueSessionLimits();
    assert(test::Fixture::live == 0);
    {
        ObjectServer server({8, 4, 3});
        assert(server.Register({"test", 8, nullptr, [](void *) { return MakeObject<test::Fixture>(); }}) ==
               0);
        size_t first = ObjectServerTestPeer::Open(server), second = ObjectServerTestPeer::Open(server);
        // Ordinary object returns are sessions, with transactional publication.
        Request(0, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        size_t child = ObjectServerTestPeer::Child(server);
        auto response = Response(false);
        assert(*response.move_handles != INVALID_HANDLE);
        ObjectServerTestPeer::Complete(server, false);
        assert(test::Fixture::live == 2);
        Request(0, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        child = ObjectServerTestPeer::Child(server);
        ObjectServerTestPeer::Complete(server);
        ObjectServerTestPeer::Close(server, child);
        assert(test::Fixture::live == 2);
        u32 root = Convert(server, first), other = Convert(server, second);
        assert(root != other);
        Request(other, 1);
        assert(ObjectServerTestPeer::Process(server, first));
        CmifResponse error{};
        assert(cmifParseResponse(&error, tls, true, 0) == MAKERESULT(10, 261));
        ObjectServerTestPeer::Complete(server);
        Request(root, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        u32 object = *Response(true).objects;
        ObjectServerTestPeer::Complete(server);
        assert(test::Fixture::live == 3);
        // Object quota exhaustion releases the just-created child.
        Request(root, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        assert(cmifParseResponse(&error, tls, true, 0) != 0);
        ObjectServerTestPeer::Complete(server);
        assert(test::Fixture::live == 3);
        Request(object, 1);
        assert(ObjectServerTestPeer::Process(server, first));
        response = Response(true);
        assert(response.copy_handles[0] == 70 && response.copy_handles[1] == 71);
        ObjectServerTestPeer::Complete(server);
        // Export a domain child as an ordinary session without changing its identity.
        auto *export_input = cmifMakeControlRequest(tls, 1, sizeof(u32));
        std::memcpy(export_input, &object, sizeof(object));
        assert(ObjectServerTestPeer::Process(server, first));
        size_t exported = ObjectServerTestPeer::Child(server);
        Response(false);
        ObjectServerTestPeer::Complete(server);
        Request(0, 1);
        assert(ObjectServerTestPeer::Process(server, exported));
        Response(false);
        ObjectServerTestPeer::Complete(server);
        ObjectServerTestPeer::Close(server, exported);
        assert(test::Fixture::live == 3);
        TestGlueBuffers(server, first, object);
        // Fixed output pointer is server-owned memory, returned as a send-static.
        CmifRequestFormat f{};
        f.object_id = object;
        f.request_id = 2;
        f.num_out_fixed_pointers = 1;
        auto request = cmifMakeRequest(tls, f);
        unsigned char profile[0x17c]{};
        cmifRequestOutFixedPointer(&request, profile, sizeof(profile));
        assert(ObjectServerTestPeer::Process(server, first));
        Response(true);
        auto hipc = hipcParseRequest(tls);
        assert(hipc.meta.num_send_statics == 1);
        auto *bytes = static_cast<u8 *>(hipcGetStaticAddress(hipc.data.send_statics));
        assert(hipcGetStaticSize(hipc.data.send_statics) == sizeof(profile) &&
               bytes[379] == static_cast<u8>(379));
        ObjectServerTestPeer::Complete(server);
        // Emulate kernel remapping a fixed input into the advertised pool.
        f = {};
        f.object_id = object;
        f.request_id = 3;
        f.num_in_pointers = 1;
        request = cmifMakeRequest(tls, f);
        u32 value = 0x12345678;
        auto *pool = ObjectServerTestPeer::Pointer(server);
        std::memcpy(pool, &value, 4);
        cmifRequestInPointer(&request, pool, 4);
        assert(ObjectServerTestPeer::Process(server, first));
        assert(*static_cast<u32 *>(Response(true, 4).data) == value);
        ObjectServerTestPeer::Complete(server);
        request = cmifMakeRequest(tls, f);
        cmifRequestInPointer(&request, &value, 4);
        assert(!ObjectServerTestPeer::Process(server, first)); // reject address outside receive pool
        // Closing a child never closes its parent/session. IDs are not recycled.
        cmifMakeCloseRequest(tls, object);
        assert(ObjectServerTestPeer::Process(server, first));
        ObjectServerTestPeer::Complete(server);
        assert(test::Fixture::live == 2);
        Request(root, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        u32 replacement = *Response(true).objects;
        assert(replacement != object);
        ObjectServerTestPeer::Complete(server, false);
        assert(test::Fixture::live == 2);
        Request(object, 1);
        assert(ObjectServerTestPeer::Process(server, first));
        assert(cmifParseResponse(&error, tls, true, 0) == MAKERESULT(10, 261));
        ObjectServerTestPeer::Complete(server);
        // Domain clones retain object tables until the final session closes.
        cmifMakeControlRequest(tls, 2, 0);
        assert(ObjectServerTestPeer::Process(server, first));
        child = ObjectServerTestPeer::Child(server);
        ObjectServerTestPeer::Complete(server);
        ObjectServerTestPeer::Close(server, first);
        assert(test::Fixture::live == 2);
        Request(root, 1);
        assert(ObjectServerTestPeer::Process(server, child));
        ObjectServerTestPeer::Complete(server);
        ObjectServerTestPeer::Close(server, child);
        assert(test::Fixture::live == 1);
        // Invalid domain envelope and conversion rollback.
        Request(other, 1);
        auto *header = reinterpret_cast<CmifDomainInHeader *>(tls + 16);
        header->num_in_objects = 1;
        assert(!ObjectServerTestPeer::Process(server, second));
        size_t plain = ObjectServerTestPeer::Open(server);
        cmifMakeControlRequest(tls, 0, 0);
        assert(ObjectServerTestPeer::Process(server, plain));
        ObjectServerTestPeer::Complete(server, false);
        Request(0, 1);
        assert(ObjectServerTestPeer::Process(server, plain));
        Response(false);
        ObjectServerTestPeer::Complete(server);
    }
    assert(test::Fixture::live == 0);
    puts("Object server domains, pointers, quotas and lifetime tests passed");
}
