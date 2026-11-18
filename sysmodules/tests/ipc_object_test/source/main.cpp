#include <switch.h>
#include <kumo/ipc/object_server.hpp>
#include <kumo/assert.hpp>
#include <kumo/result.hpp>
#include <tests/ipc/object_fixture.hpp>
#include <cstdio>
extern "C" {
u32 __nx_applet_type = AppletType_None;
void __libnx_initheap() {
    alignas(16) static char heap[1024 * 1024];
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}
void __appInit() {
    KUMO_ABORT_RESULT(smInitialize());
}
void __appExit() {
    smExit();
}
void __libnx_exit(int) {
    for (;;)
        svcSleepThread(1000000000);
}
}
using namespace kumo::ipc;
static ObjectServer server({128, 128, 0x4000});
static Handle entered_writer, entered_reader, release_writer, release_reader;
constexpr u64 ClientTimeout = 5'000'000'000;
class Blocking final : public ServiceObject {
  public:
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        static constexpr CommandOptions pointer{.buffers = BufferMode::FixedPointer, .input_pointer_size = 4};
        static constexpr CommandTable commands({
            MakeCommand<Blocking, NoData, u32, 1, 0,
                [](Blocking &, const NoData &, u32 &out, Arguments<1, 0> &args) -> Result {
                    KUMO_TRY(svcSignalEvent(entered_writer));
                    s32 index;
                    KUMO_TRY(svcWaitSynchronization(&index, &release_reader, 1, ClientTimeout));
                    std::memcpy(&out, args.In(0).GetPointer(), sizeof(out));
                    return 0;
                }>(0, pointer),
            MakeCommand<Blocking, NoData, u32, 1, 0,
                [](Blocking &, const NoData &, u32 &out, Arguments<1, 0> &args) -> Result {
                    std::memcpy(&out, args.In(0).GetPointer(), sizeof(out));
                    return 0;
                }>(1, pointer),
        });
        return kumo::ipc::Dispatch(this, commands, request, reply);
    }
};
static Thread client_thread;
alignas(0x1000) static u8 client_stack[64 * 1024];
static int checks = 0, failures = 0;
static void Check(const char *name, bool ok, Result rc = 0) {
    ++checks;
    if (!ok)
        ++failures;
    char text[200];
    int n = std::snprintf(text, sizeof(text), "[ipc_object_test] %s: %s (0x%x)\n", ok ? "PASS" : "FAIL", name,
                          rc);
    svcOutputDebugString(text, n);
}
static void CheckGlueCommands(::Service *service) {
    const u8 first[0xD0]{3}, second[0xD0]{10};
    for (u32 command : {500, 501}) {
        s64 difference = 0;
        Result rc = serviceDispatchOut(service, command, difference,
            .buffer_attrs = {
                SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In,
                SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In,
            },
            .buffers = {{first, sizeof(first)}, {second, sizeof(second)}});
        Check("two fixed snapshot pointers", R_SUCCEEDED(rc) && difference == -7, rc);
    }
    const u64 calendar = 42;
    TimeZoneRule rule{{9}};
    u64 timestamps[2]{~u64(0), ~u64(0)};
    s32 count = 0;
    Result rc = serviceDispatchInOut(service, 201, calendar, count,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In, SfBufferAttr_HipcPointer | SfBufferAttr_Out},
        .buffers = {{&rule, sizeof(rule)}, {timestamps, sizeof(timestamps)}});
    Check("mapped rule and pointer output",
          R_SUCCEEDED(rc) && count == 1 && timestamps[0] == 51 && timestamps[1] == 0, rc);
    count = 0;
    rc = serviceDispatchInOut(service, 202, calendar, count,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_Out},
        .buffers = {{timestamps, sizeof(timestamps[0])}});
    Check("variable pointer output", R_SUCCEEDED(rc) && count == 1 && timestamps[0] == 42, rc);
}
static void CheckConcurrentClone() {
    ::Service root{}, clone{};
    Result rc = smGetService(&root, "kumo:blk");
    if (R_SUCCEEDED(rc))
        rc = serviceConvertToDomain(&root);
    if (R_SUCCEEDED(rc))
        rc = serviceClone(&root, &clone);
    Check("open cloned blocking domain", R_SUCCEEDED(rc), rc);
    if (R_FAILED(rc)) {
        serviceClose(&root);
        return;
    }

    struct Call {
        ::Service *service;
        Result result;
        u32 output;
    } call{&root, 1, 0};
    Thread blocked;
    KUMO_ABORT_RESULT(threadCreate(&blocked, [](void *arg) {
        auto &call = *static_cast<Call *>(arg);
        const u32 input = 0x12345678;
        call.result = serviceDispatchOut(call.service, 0, call.output,
            .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In},
            .buffers = {{&input, sizeof(input)}});
    }, &call, nullptr, 64 * 1024, 45, -2));
    KUMO_ABORT_RESULT(threadStart(&blocked));
    s32 index;
    rc = svcWaitSynchronization(&index, &entered_reader, 1, ClientTimeout);
    Check("first domain request is in flight", R_SUCCEEDED(rc), rc);
    if (R_SUCCEEDED(rc)) {
        const u32 input = 0xfedcba98;
        u32 output = 0;
        rc = serviceDispatchOut(&clone, 1, output,
            .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In},
            .buffers = {{&input, sizeof(input)}});
        Check("clone dispatch while first request blocks", R_SUCCEEDED(rc) && output == input, rc);
    }
    KUMO_ABORT_RESULT(svcSignalEvent(release_writer));
    KUMO_ABORT_RESULT(threadWaitForExit(&blocked));
    KUMO_ABORT_RESULT(threadClose(&blocked));
    Check("blocked request retains its own pointer buffer", call.result == 0 && call.output == 0x12345678, call.result);
    serviceClose(&clone);
    serviceClose(&root);
}

static void CheckWaitSetCapacity() {
    ::Service root{};
    Result rc = smGetService(&root, "kumo:t0");
    Check("connect for session capacity test", R_SUCCEEDED(rc), rc);
    if (R_FAILED(rc))
        return;

    // Keep enough returned sessions open to require a second kernel wait set.
    ::Service children[65]{};
    size_t count = 0;
    for (auto &child : children) {
        rc = serviceDispatch(&root, 0, .out_num_objects = 1, .out_objects = &child);
        if (R_FAILED(rc))
            break;
        ++count;
    }
    Check("open more sessions than one kernel wait set supports", count == 65, rc);
    if (count == 65) {
        const u32 input = 0x11223344;
        u32 output = 0;
        rc = serviceDispatchOut(&children[count - 1], 3, output,
            .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In},
            .buffers = {{&input, sizeof(input)}});
        Check("dispatch a fixed pointer request on the second wait set", R_SUCCEEDED(rc) && output == input, rc);
    }
    for (size_t i = 0; i < count; ++i)
        serviceClose(&children[i]);
    serviceClose(&root);
}

static void Client(void *) {
    CheckConcurrentClone();
    CheckWaitSetCapacity();
    for (const char *name : {"kumo:t0", "kumo:t1", "kumo:t2"}) {
        ::Service root{};
        Result rc = smGetService(&root, name);
        Check("connect port", R_SUCCEEDED(rc), rc);
        if (R_FAILED(rc))
            continue;
        ::Service plain_child{};
        rc = serviceDispatch(&root, 0, .out_num_objects = 1, .out_objects = &plain_child);
        Check("ordinary child session", R_SUCCEEDED(rc), rc);
        if (R_SUCCEEDED(rc)) {
            CheckGlueCommands(&plain_child);
            serviceClose(&plain_child);
        }
        rc = serviceConvertToDomain(&root);
        Check("convert domain", R_SUCCEEDED(rc), rc);
        if (R_FAILED(rc)) {
            serviceClose(&root);
            continue;
        }
        ::Service child{};
        rc = serviceDispatch(&root, 0, .out_num_objects = 1, .out_objects = &child);
        Check("domain child object", R_SUCCEEDED(rc), rc);
        if (R_FAILED(rc)) {
            serviceClose(&root);
            continue;
        }
        Handle events[2]{};
        rc = serviceDispatch(&child, 1,
                             .out_handle_attrs = {SfOutHandleAttr_HipcCopy, SfOutHandleAttr_HipcCopy},
                             .out_handles = events);
        Check("two copied events", R_SUCCEEDED(rc) && events[0] != events[1], rc);
        if (R_SUCCEEDED(rc)) {
            svcCloseHandle(events[0]);
            svcCloseHandle(events[1]);
        }
        u8 profile[0x17c]{};
        rc = serviceDispatch(
            &child, 2, .buffer_attrs = {SfBufferAttr_FixedSize | SfBufferAttr_HipcPointer | SfBufferAttr_Out},
            .buffers = {{profile, sizeof(profile)}});
        bool match = R_SUCCEEDED(rc);
        for (size_t i = 0; i < sizeof(profile); ++i)
            match &= profile[i] == static_cast<u8>(i);
        Check("fixed profile output pointer", match, rc);
        u32 client_id = 0x12345678, echo = 0;
        rc = serviceDispatchOut(
            &child, 3, echo,
            .buffer_attrs = {SfBufferAttr_FixedSize | SfBufferAttr_HipcPointer | SfBufferAttr_In},
            .buffers = {{&client_id, 4}});
        Check("fixed client ID input pointer", R_SUCCEEDED(rc) && echo == client_id, rc);
        u8 type = 0x5a;
        u32 count = 0;
        u8 profiles[0x75 * 4]{};
        rc = serviceDispatchInOut(&child, 4, type, count,
                                  .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                  .buffers = {{profiles, sizeof(profiles)}});
        match = R_SUCCEEDED(rc) && count == 4;
        for (u8 value : profiles)
            match &= value == type;
        Check("mapped profile array", match, rc);
        const s32 query_id = 7;
        const char path[0x301] = "/main";
        const u32 query_input = 0xfeedface;
        u32 query_output = 0;
        constexpr auto non_secure = SfBufferAttr_HipcMapAlias | SfBufferAttr_HipcMapTransferAllowsNonSecure;
        rc = serviceDispatchIn(&child, 15, query_id,
            .buffer_attrs = {
                non_secure | SfBufferAttr_Out,
                non_secure | SfBufferAttr_In,
                SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize | SfBufferAttr_In,
            },
            .buffers = {{&query_output, 4}, {&query_input, 4}, {path, sizeof(path)}});
        Check("fixed path and non-secure mapped buffers", R_SUCCEEDED(rc) && query_output == query_input, rc);
        u64 reserved = 0, pid = 0, expected = 0;
        svcGetProcessId(&expected, CUR_PROCESS_HANDLE);
        rc = serviceDispatchInOut(&child, 5, reserved, pid, .in_send_pid = true);
        Check("kernel supplied PID", R_SUCCEEDED(rc) && pid == expected, rc);
        // libnx clone control preserves the domain table and its objects.
        Handle clone = INVALID_HANDLE;
        rc = cmifCloneCurrentObject(root.session, &clone);
        Check("clone domain session", R_SUCCEEDED(rc), rc);
        if (R_SUCCEEDED(rc))
            svcCloseHandle(clone);
        serviceClose(&child);
        rc = serviceDispatch(&root, 0, .out_num_objects = 1, .out_objects = &child);
        Check("parent survives child close", R_SUCCEEDED(rc), rc);
        if (R_SUCCEEDED(rc))
            serviceClose(&child);
        serviceClose(&root);
    }
    char text[100];
    int n = std::snprintf(text, sizeof(text), "[ipc_object_test] done: %d checks, %d failure(s)\n", checks,
                          failures);
    svcOutputDebugString(text, n);
    for (;;)
        svcSleepThread(1000000000);
}
int main(int, char **) {
    Handle writers[2];
    for (size_t i = 0; i < 2; ++i)
        KUMO_ABORT_RESULT(svcCreateEvent(&writers[i], &test::Fixture::events[i]));
    const char *names[] = {"kumo:t0", "kumo:t1", "kumo:t2"};
    const size_t caps[] = {56, 2, 1};
    for (size_t i = 0; i < 3; ++i)
        KUMO_ABORT_RESULT(
            server.Register({names[i], caps[i], nullptr, [](void *) { return MakeObject<test::Fixture>(); }}));
    KUMO_ABORT_RESULT(svcCreateEvent(&entered_writer, &entered_reader));
    KUMO_ABORT_RESULT(svcCreateEvent(&release_writer, &release_reader));
    KUMO_ABORT_RESULT(server.Register({"kumo:blk", 2, nullptr, [](void *) { return MakeObject<Blocking>(); }}));
    KUMO_ABORT_RESULT(
        threadCreate(&client_thread, Client, nullptr, client_stack, sizeof(client_stack), 45, -2));
    KUMO_ABORT_RESULT(threadStart(&client_thread));
    KUMO_ABORT_RESULT(server.RunConcurrent({.count = 4, .priority = 45}));
    return 0;
}
