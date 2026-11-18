#pragma once
#define armGetTls libnxArmGetTls
extern "C" {
#include <switch/types.h>
#include <switch/result.h>
#include <switch/sf/hipc.h>
#include <switch/sf/cmif.h>
}
#undef armGetTls
using Mutex = unsigned;
using CondVar = unsigned;
void mutexInit(Mutex *);
void mutexLock(Mutex *);
void mutexUnlock(Mutex *);
bool mutexIsLockedByCurrentThread(Mutex *);
void condvarInit(CondVar *);
void condvarWakeOne(CondVar *);
Result condvarWait(CondVar *, Mutex *);
Result smInitialize();
void smExit();
u64 smEncodeName(const char *);
Result smRegisterService(Handle *, u64, bool, s32);
Result svcCloseHandle(Handle);
Result svcCreateSession(Handle *, Handle *, u32, u64);
Result svcAcceptSession(Handle *, Handle);
Result svcReplyAndReceive(s32 *, const Handle *, s32, Handle, u64);
void *armGetTls();

Result svcWaitSynchronization(s32 *, const Handle *, s32, u64);
