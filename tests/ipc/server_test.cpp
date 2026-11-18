#include <kumo/ipc/dispatch.hpp>
#include <kumo/ipc/server.hpp>
#include <kumo/ipc/concurrent_server.hpp>
#include <cassert>
#include <vector>
#include <cstdio>
#include <algorithm>
alignas(16) static unsigned char tls[256];
static std::vector<Handle> closed;
static Handle next_handle = 100;
static int refs = 1, calls = 0;
static Result create_result = 0, wait_result = 0;
static s32 wait_index = -1;
static int receive_steps = 0;
static int dispatch_mode = 0;
struct WorkerIdle {};
static Result condition_error = 0;
void mutexInit(Mutex *) {} void mutexLock(Mutex *) {} void mutexUnlock(Mutex *) {}
void condvarInit(CondVar *) {} void condvarWakeOne(CondVar *) {} Result condvarWait(CondVar *, Mutex *) { if (condition_error) return condition_error; throw WorkerIdle{}; }
Result smInitialize() { return 0; } void smExit() {}
u64 smEncodeName(const char *) { return 0; }
Result smRegisterService(Handle *h, u64, bool, s32) { *h=next_handle++; return 0; }
Result svcCloseHandle(Handle h) { closed.push_back(h); return 0; }
Result svcCreateSession(Handle *s, Handle *c, u32, u64) { if(create_result) return create_result; *s=next_handle++; *c=next_handle++; return 0; }
Result svcAcceptSession(Handle *, Handle) { return 1; }
Result svcReplyAndReceive(s32 *index, const Handle *, s32, Handle, u64) { if (receive_steps++ == 0) { *index=0; return 0; } *index=wait_index; return wait_result; }
void *armGetTls() { return tls; }
using namespace kumo::ipc;
namespace kumo::ipc {
struct ServerTestPeer {
    static bool Process(Server &s) { return s.Process(); }
    static bool Enqueue(ConcurrentServer &s, size_t service) { return s.Enqueue({42, &s.services_[service], &refs}); }
    static bool Process(ConcurrentServer &s, Handle &pending) { return s.Process({42, &s.services_[0], &refs}, pending); }
    static void Serve(ConcurrentServer &s) { s.Serve({42, &s.services_[0], &refs}); }
    static size_t Active(ConcurrentServer &s, size_t service) { return s.service_active_[service]; }
};
}
static void PrepareRequest(u32 command, bool control = false, size_t copies = 0) {
    std::memset(tls,0,sizeof(tls));
    CmifRequestFormat f{}; f.request_id=command; f.num_handles=copies;
    auto r=cmifMakeRequest(tls,f);
    if(copies) r.hipc.copy_handles[0]=77;
    if(control) reinterpret_cast<HipcHeader *>(tls)->type=CmifCommandType_Control;
}
    struct Flag {
        static constexpr size_t WireBytes=1;
        u8 value;
        bool IsValidWire() const { return value<=1; }
    };

int main() {
    ConcurrentServer server;
    assert(server.Initialize(3)==0);
    ConcurrentService service{"a",2,nullptr,[](void*)->void*{return &refs;},[](void*){++refs;},[](void*){--refs;},
        [](void*,ConcurrentRequest &,ConcurrentReply &r){++calls;r.Write<u32>(123); if(dispatch_mode) r.move_handle=88; if(dispatch_mode==2) return false; if(dispatch_mode==3) r.result=1; return true;}};
    assert(server.Register(service)==0);
    service.name="b";service.max_sessions=1;assert(server.Register(service)==0);
    assert(server.Register(service)!=0); // reservations cannot exceed workers
    assert(ServerTestPeer::Enqueue(server,0));
    Handle pending=INVALID_HANDLE;
    PrepareRequest(2,true);
    assert(ServerTestPeer::Process(server,pending));
    assert(pending!=INVALID_HANDLE && refs==2 && ServerTestPeer::Active(server,0)==2);
    pending=INVALID_HANDLE;
    PrepareRequest(2,true);
    const size_t before=closed.size();
    assert(ServerTestPeer::Process(server,pending));
    assert(pending==INVALID_HANDLE && refs==2 && closed.size()==before+2);
    assert(ServerTestPeer::Enqueue(server,1)); // other service retains reserved capacity
    PrepareRequest(0,false,1);
    assert(ServerTestPeer::Process(server,pending));
    assert(closed.back()==77 && calls==1); // received handle closed once after dispatch
    PrepareRequest(0,false,1);
    reinterpret_cast<HipcHeader *>(tls)->num_data_words=1023;
    assert(!ServerTestPeer::Process(server,pending));
    assert(closed.back()==77 && calls==1); // malformed envelope still closes installed handles

    // Moved reply ownership: close on handler rejection/error and failed send,
    // but never close a handle whose reply was sent before the wait failed.
    dispatch_mode=2; PrepareRequest(0); size_t n=closed.size();
    assert(!ServerTestPeer::Process(server,pending));
    assert(closed.size()==n+1 && closed.back()==88);
    dispatch_mode=3; PrepareRequest(0); n=closed.size();
    assert(ServerTestPeer::Process(server,pending));
    assert(pending==INVALID_HANDLE && closed.size()==n+1 && closed.back()==88);
    dispatch_mode=1; PrepareRequest(0); receive_steps=0; wait_result=1; wait_index=-1; n=closed.size();
    ServerTestPeer::Serve(server);
    assert(closed.size()==n+2 && closed[n]==88 && closed[n+1]==42);
    PrepareRequest(0); receive_steps=0; wait_index=0; n=closed.size();
    ServerTestPeer::Serve(server);
    assert(closed.size()==n+1 && closed.back()==42);
    dispatch_mode=0;
    PrepareRequest(2,true,1); n=closed.size();
    assert(!ServerTestPeer::Process(server,pending));
    assert(closed.size()==n+1 && closed.back()==77); // controls reject extra handles

    create_result=1; PrepareRequest(2,true); n=closed.size();
    const int saved_refs=refs;
    assert(ServerTestPeer::Process(server,pending));
    assert(pending==INVALID_HANDLE && refs==saved_refs && closed.size()==n);
    create_result=0;

    // A disconnected session releases the service object and restores quota.
    ConcurrentServer lifecycle;
    assert(lifecycle.Initialize(1)==0);
    service.max_sessions=1; assert(lifecycle.Register(service)==0);
    refs=1; assert(ServerTestPeer::Enqueue(lifecycle,0));
    PrepareRequest(0); receive_steps=0; wait_index=-1;
    try { lifecycle.Worker(); assert(false); } catch (const WorkerIdle &) {}
    assert(refs==0 && ServerTestPeer::Active(lifecycle,0)==0);
    assert(ServerTestPeer::Enqueue(lifecycle,0));

    Server simple;
    static constexpr CommandTable simple_commands({MakeCommand<int, NoData, NoData, 0, 0,
        [](int &, const NoData &, NoData &, Arguments<0,0> &a)->Result { a.ReturnCopy(BorrowedHandle{90}); return 0; }>(1)});
    int simple_context=0;
    kumo::ipc::Service simple_service{"simple", [](void *c, ConcurrentRequest &r, ConcurrentReply &out) {
        return Dispatch(c,simple_commands,r,out);
    }, &simple_context};
    assert(simple.Register(simple_service,1)==0);
    PrepareRequest(1); n=closed.size(); assert(ServerTestPeer::Process(simple));
    assert(closed.size()==n); // borrowed output handle stays owned by the service
    PrepareRequest(1,false,1); assert(!ServerTestPeer::Process(simple)); assert(closed.back()==77);
    PrepareRequest(99); assert(ServerTestPeer::Process(simple)); // unknown command keeps session alive

    struct Service { int called=0; } object;
    constexpr CommandTable commands({MakeCommand<Service,u32,u64,0,0,
        [](Service &s,const u32 &in,u64 &out,Arguments<0,0>&)->Result{++s.called;out=in+1;return 0;}>(7)});
    PrepareRequest(7);
    ConcurrentRequest request{};
    assert(DecodeCmif(tls,sizeof(tls),request.input));
    request.data=tls+request.input.data_offset;
    request.input.data_size=3;
    ConcurrentReply reply;
    assert(!Dispatch(&object,commands,request,reply) && object.called==0);
    request.input.data_size=4;
    u32 value=41;std::memcpy(tls+request.input.data_offset,&value,4);
    assert(Dispatch(&object,commands,request,reply) && object.called==1 && reply.size==8);
    u64 answer;std::memcpy(&answer,reply.data,8);assert(answer==42);
    request.copy_count=1;
    assert(!Dispatch(&object,commands,request,reply) && object.called==1);
    request.input.header.command_id=99;
    assert(Dispatch(&object,commands,request,reply) && reply.result==MAKERESULT(10,221));
    // Auto buffers: PID requirements, type-derived pointer-size table, and
    // ownership transfer are tested together against libnx's wire builder.
    constexpr CommandTable buffers({MakeCommand<Service,u32,u32,1,1,
        [](Service &s,const u32 &in,u32 &out,Arguments<1,1>& a)->Result{
            ++s.called; assert(a.In(0).GetSize()==4 && a.Out(0).GetSize()==4);
            auto owned=a.TakeCopy(0); assert(owned.GetOsHandle()==77);
            out=in; return 0;
        }>(8,{true,1})});
    CmifRequestFormat f{}; f.request_id=8;f.data_size=4;f.send_pid=true;
    f.num_handles=1;f.num_in_auto_buffers=1;f.num_out_auto_buffers=1;
    auto wire=cmifMakeRequest(tls,f);
    u32 source=9,destination=0;
    cmifRequestInAutoBuffer(&wire,&source,4,HipcBufferMode_Normal);
    cmifRequestOutAutoBuffer(&wire,&destination,4,HipcBufferMode_Normal);
    std::memcpy(wire.data,&source,4);
    request={}; assert(DecodeCmif(tls,sizeof(tls),request.input));
    request.data=tls+request.input.data_offset;request.copy_count=1;request.handles[0]=77;
    n=closed.size();
    assert(Dispatch(&object,buffers,request,reply));
    assert(request.handles[0]==INVALID_HANDLE && closed.size()==n+1 && closed.back()==77);
    request.handles[0]=77;
    auto *sizes=reinterpret_cast<u16 *>(reinterpret_cast<unsigned char *>(request.input.hipc.data.data_words)+16+sizeof(CmifInHeader)+4);
    *sizes=1; assert(!Dispatch(&object,buffers,request,reply));
    *sizes=0;request.input.hipc.meta.send_pid=false;
    assert(!Dispatch(&object,buffers,request,reply));
    // Boundaries are relative to each handle class, never the combined list.
    request.copy_count=1; request.move_count=1; request.handles[0]=70; request.handles[1]=71;
    reply.move_handle=INVALID_HANDLE;
    {
        Arguments<0,0> a{{},{},request,reply};
        assert(a.TakeCopy(1).GetOsHandle()==INVALID_HANDLE);
        assert(a.TakeMove(1).GetOsHandle()==INVALID_HANDLE);
        assert(request.handles[0]==70 && request.handles[1]==71);
        auto moved=a.TakeMove(0); assert(moved.GetOsHandle()==71);
        a.ReturnMove(std::move(moved)); assert(reply.move_handle==71);
        auto copy=a.TakeCopy(0); assert(copy.GetOsHandle()==70);
        a.ReturnCopy(BorrowedHandle{90});
    }
    assert(closed.back()==70 && reply.copy_handle==90);
    svcCloseHandle(reply.move_handle);reply.move_handle=INVALID_HANDLE;

    assert(lifecycle.Register(service)!=0); // frozen by Worker
    ConcurrentServer uninitialized;
    assert(uninitialized.Worker()!=0 && uninitialized.Run()!=0);
    receive_steps=1;wait_index=-1;wait_result=MAKERESULT(Module_Kernel,KernelError_InvalidHandle);
    assert(server.Run()==wait_result);
    assert(server.Register(service)!=0 && server.Run()!=0);
    ConcurrentServer condition_server;
    assert(condition_server.Initialize(1)==0 && condition_server.Register(service)==0);
    condition_error=MAKERESULT(Module_Kernel,KernelError_InvalidState);
    assert(condition_server.Worker()==condition_error);
    condition_error=0;
    assert(simple.Run()==wait_result);
    assert(simple.Register(simple_service,1)!=0 && simple.Run()!=0);

    // Literal little-endian CMIF fixture, independent of libnx's writer.
    alignas(16) unsigned char golden[256]{};
    golden[0]=4;golden[4]=9;
    std::memcpy(golden+16,"SFCI",4);golden[24]=7;golden[32]=41;
    request={};assert(DecodeCmif(golden,sizeof(golden),request.input));
    request.data=golden+request.input.data_offset;
    reply={};assert(Dispatch(&object,commands,request,reply));
    std::memset(golden,0xa5,sizeof(golden));
    assert(EncodeReply(golden,reply.result,0,reply.data,reply.size,INVALID_HANDLE));
    unsigned char expected[256]{};expected[4]=10;
    std::memcpy(expected+16,"SFCO",4);expected[32]=42;
    assert(std::memcmp(golden,expected,sizeof(golden))==0);

    static constexpr CommandTable flags({MakeCommand<int,Flag,NoData,0,0,
        [](int &, const Flag &, NoData &, Arguments<0,0>&)->Result{return 0;}>(1)});
    PrepareRequest(1); request={};assert(DecodeCmif(tls,sizeof(tls),request.input));
    request.data=tls+request.input.data_offset;
    tls[request.input.data_offset]=2;
    assert(!Dispatch(&simple_context,flags,request,reply));
    tls[request.input.data_offset]=1;
    assert(Dispatch(&simple_context,flags,request,reply));
    std::puts("IPC dispatch and session lifecycle tests passed");
}
