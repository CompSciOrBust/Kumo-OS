#include "bsd_dns.hpp"
#include "bsd_stack.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <strings.h>
extern "C" {
#include <lwip/dns.h>
}

namespace kumo::horizon::hznet::dns {
// The reverse transport runs with the same stack lock as the lwIP resolver.
Status LookupPtr(u32 address, const bool *cancelled, HostName &name);

namespace {

constexpr u64 TimeoutNs = 12'000'000'000;
constexpr u64 IdleHandleNs = 60'000'000'000;

struct Query {
    bool used = false;
    bool waiting = false;
    bool complete = false;
    bool found = false;
    u32 address = 0;
};

struct Cancellation {
    u64 pid = 0;
    u64 expiry = 0;
    u32 handle = 0;
    bool active = false;
    bool cancelled = false;
};

// lwIP keeps the callback argument until it responds or exhausts its retries.
std::array<Query, DNS_MAX_REQUESTS> g_queries;
std::array<Cancellation, 64> g_cancellations;
u32 g_next_handle = 1;

Cancellation *FindCancellation(u64 pid, u32 handle) {
    const u64 now = svcGetSystemTick();
    for (auto &entry : g_cancellations) {
        if (entry.handle == handle && entry.pid == pid && (entry.active || entry.expiry > now)) {
            return &entry;
        }
    }
    return nullptr;
}

class Request {
    Cancellation *entry_ = nullptr;
    Status status_ = Status::Success;

public:
    Request(u64 pid, u32 handle) {
        if (!handle) return;
        auto *entry = FindCancellation(pid, handle);
        if (!entry || entry->active) {
            status_ = Status::InvalidHandle;
            return;
        }
        entry_ = entry;
        entry_->active = true;
        if (entry_->cancelled) status_ = Status::Cancelled;
    }
    ~Request() { if (entry_) *entry_ = {}; }
    Status InitialStatus() const { return status_; }
    const bool *CancelledFlag() const { return entry_ ? &entry_->cancelled : nullptr; }
    bool Cancelled() const { return entry_ && entry_->cancelled; }
};

void Answer(const char *, const ip_addr_t *address, void *argument) {
    auto &query = *static_cast<Query *>(argument);
    if (!query.waiting) {
        query = {};
        return;
    }
    query.found = address != nullptr;
    if (query.found) query.address = ip4_addr_get_u32(ip_2_ip4(address));
    query.complete = true;
    stack::NotifyActivity();
}

} // namespace

Status FinishImmediateRequest(u64 pid, u32 cancel_handle) {
    stack::ScopedLock lock;
    const Request request(pid, cancel_handle);
    return request.InitialStatus();
}

bool ParseAddress(const char *name, u32 &address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, name, &parsed) != 1) return false;
    address = parsed.s_addr;
    return true;
}

u32 AllocateCancelHandle(u64 pid) {
    stack::ScopedLock lock;
    const u64 now = svcGetSystemTick();
    for (auto &entry : g_cancellations) {
        if (entry.active || (entry.handle && entry.expiry > now)) continue;

        u32 handle;
        do {
            handle = g_next_handle++;
        } while (!handle || FindCancellation(pid, handle));
        entry = {.pid = pid, .expiry = now + armNsToTicks(IdleHandleNs), .handle = handle};
        return handle;
    }
    return 0;
}

void Cancel(u64 pid, u32 handle) {
    if (!handle) return;
    stack::ScopedLock lock;
    if (auto *entry = FindCancellation(pid, handle)) {
        entry->cancelled = true;
        stack::NotifyActivity();
    }
}

Status Resolve(const char *name, u64 pid, u32 cancel_handle, u32 &address) {
    stack::ScopedLock lock;
    const Request request(pid, cancel_handle);
    if (request.InitialStatus() != Status::Success) return request.InitialStatus();

    if (ParseAddress(name, address)) return Status::Success;
    if (!strcasecmp(name, "localhost") || !strcasecmp(name, "localhost.")) {
        address = lwip_htonl(0x7f000001);
        return Status::Success;
    }
    if (ip_addr_isany(dns_getserver(0))) return Status::TemporaryFailure;
    auto query = std::find_if(g_queries.begin(), g_queries.end(), [](const Query &candidate) {
        return !candidate.used;
    });
    if (query == g_queries.end()) return Status::TemporaryFailure;

    ip_addr_t immediate{};
    *query = {.used = true, .waiting = true};
    const err_t rc = dns_gethostbyname(name, &immediate, Answer, &*query);
    if (rc != ERR_INPROGRESS) {
        *query = {};
        if (rc == ERR_OK) {
            address = ip4_addr_get_u32(ip_2_ip4(&immediate));
            return Status::Success;
        }
        return rc == ERR_ARG ? Status::NotFound : Status::TemporaryFailure;
    }

    stack::KickNetworkThread();
    const u64 deadline = svcGetSystemTick() + armNsToTicks(TimeoutNs);
    while (!query->complete && !request.Cancelled()) {
        const u64 now = svcGetSystemTick();
        if (now >= deadline) break;
        stack::WaitForActivity(std::chrono::nanoseconds(armTicksToNs(deadline - now)));
    }
    const bool cancelled = request.Cancelled();
    if (!query->complete) {
        query->waiting = false;
        return cancelled ? Status::Cancelled : Status::TemporaryFailure;
    }
    const bool found = query->found;
    address = query->address;
    *query = {};
    if (cancelled) return Status::Cancelled;
    return found ? Status::Success : Status::NotFound;
}

Status Reverse(u32 address, u64 pid, u32 cancel_handle, HostName &name) {
    stack::ScopedLock lock;
    const Request request(pid, cancel_handle);
    if (request.InitialStatus() != Status::Success) return request.InitialStatus();
    if (address == lwip_htonl(0x7f000001)) {
        std::memcpy(name, "localhost", sizeof("localhost"));
        return Status::Success;
    }
    return LookupPtr(address, request.CancelledFlag(), name);
}

}
