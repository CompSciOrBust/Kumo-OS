#include "dns_server.hpp"
#include "../socket/bsd_wire.hpp"
#include "../stack/bsd_dns.hpp"
#include <kumo/ipc/concurrent_server.hpp>
#include <kumo/ipc/dispatch.hpp>
#include <netdb.h>
#include <string_view>

namespace kumo::horizon::hznet::ipc {
namespace {
using namespace kumo::ipc;
using namespace wire;

constexpr u32 AddrInfoMagic = 0xbeefcafe;
constexpr size_t MaxName = 255;
constexpr size_t WorkerCount = 8;
constexpr size_t StackSize = 32 * 1024;
constexpr CommandOptions Mapped{.buffers = BufferMode::Mapped};
constexpr CommandOptions PidMapped{.pid = true, .buffers = BufferMode::Mapped};
constexpr CommandOptions PidAuto{.pid = true};
constexpr CommandOptions PidMixed{.pid = true, .mapped_input_prefix = 3};
constexpr Result Unsupported = MAKERESULT(Module_Kernel, KernelError_NotImplemented);
constexpr Result BadInput = MAKERESULT(Module_Libnx, LibnxError_BadInput);

struct LookupInput { static constexpr size_t WireBytes = 16; u32 flags, handle; u64 pid_placeholder; };
struct ReverseInput { static constexpr size_t WireBytes = 24; u32 length, family, handle, padding; u64 pid_placeholder; };
struct OptionsInput { static constexpr size_t WireBytes = 16; u32 version, count; u64 pid_placeholder; };
struct HostOutput { static constexpr size_t WireBytes = 12; s32 host_error, error; u32 size; };
struct NameOutput { static constexpr size_t WireBytes = 8; s32 error, result; };
struct AddrOutput { static constexpr size_t WireBytes = 12; s32 error, result; u32 size; };
struct HostOptionsOutput { static constexpr size_t WireBytes = 12; u32 size; s32 host_error, error; };
struct AddrOptionsOutput { static constexpr size_t WireBytes = 16; u32 size; s32 result, host_error, error; };
struct Hints { u32 flags, family, type, protocol; };

struct ServicePort { std::string_view name; u16 port; u32 protocol; };
constexpr ServicePort Services[] = {
    {"ftp", 21, IPPROTO_TCP_}, {"ssh", 22, IPPROTO_TCP_}, {"telnet", 23, IPPROTO_TCP_},
    {"smtp", 25, IPPROTO_TCP_}, {"domain", 53, IPPROTO_TCP_}, {"domain", 53, IPPROTO_UDP_},
    {"http", 80, IPPROTO_TCP_}, {"pop3", 110, IPPROTO_TCP_}, {"ntp", 123, IPPROTO_UDP_},
    {"imap", 143, IPPROTO_TCP_}, {"https", 443, IPPROTO_TCP_}, {"submission", 587, IPPROTO_TCP_},
    {"imaps", 993, IPPROTO_TCP_}, {"pop3s", 995, IPPROTO_TCP_},
};

struct Transport { u32 type, protocol; };
constexpr Transport Transports[] = {{SOCK_STREAM_, IPPROTO_TCP_}, {SOCK_DGRAM_, IPPROTO_UDP_}};

template<class T> void Write(u8 *&out, T value) {
    std::memcpy(out, &value, sizeof(value));
    out += sizeof(value);
}

u32 ReadWord(const u8 *data) {
    u32 value;
    std::memcpy(&value, data, sizeof(value));
    return std::byteswap(value);
}

bool CopyInput(InBuffer buffer, char (&text)[MaxName + 1]) {
    if (!buffer.GetSize()) {
        text[0] = 0;
        return true;
    }
    const auto *data = reinterpret_cast<const char *>(buffer.GetPointer());
    const auto *end = static_cast<const char *>(std::memchr(data, 0, std::min(buffer.GetSize(), sizeof(text))));
    if (!end) return false;
    std::memcpy(text, data, end - data + 1);
    return true;
}

bool CopyString(OutBuffer buffer, std::string_view text) {
    if (buffer.GetSize() <= text.size()) return false;
    auto *out = reinterpret_cast<char *>(buffer.GetPointer());
    if (!text.empty()) std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    return true;
}

bool ParsePort(std::string_view service, u16 &port) {
    u32 number = 0;
    for (const char digit : service) {
        if (digit < '0' || digit > '9') return false;
        number = number * 10 + digit - '0';
        if (number > UINT16_MAX) return false;
    }
    port = number;
    return true;
}

const ServicePort *FindPort(std::string_view service, u32 protocol) {
    for (const auto &entry : Services) {
        if (entry.name == service && entry.protocol == protocol) return &entry;
    }
    return nullptr;
}

bool ReadHints(InBuffer buffer, Hints &hints) {
    if (buffer.GetSize() == 0) return true;
    if (buffer.GetSize() < 4) return false;
    const auto *data = static_cast<const u8 *>(buffer.GetPointer());
    if (ReadWord(data) == 0) return true;
    // A hints structure is one addrinfo record with no address or canonical name.
    if (buffer.GetSize() < 33 || ReadWord(data) != AddrInfoMagic || ReadWord(data + 20) != 0 ||
        data[28] != 0 || ReadWord(data + 29) != 0) return false;
    hints = {ReadWord(data + 4), ReadWord(data + 8), ReadWord(data + 12), ReadWord(data + 16)};
    return true;
}

HostOutput HostError(dns::Status status) {
    switch (status) {
        case dns::Status::Success: return {};
        case dns::Status::NotFound: return {HOST_NOT_FOUND, 0, 0};
        case dns::Status::TemporaryFailure: return {TRY_AGAIN, 0, 0};
        case dns::Status::Cancelled: return {NETDB_INTERNAL, Errno_ECANCELED, 0};
        case dns::Status::InvalidHandle: return {NETDB_INTERNAL, Errno_EINVAL, 0};
    }
    return {NO_RECOVERY, 0, 0};
}

AddrOutput AddrError(dns::Status status) {
    switch (status) {
        case dns::Status::Success: return {};
        case dns::Status::NotFound: return {0, EAI_NONAME, 0};
        case dns::Status::TemporaryFailure: return {0, EAI_AGAIN, 0};
        case dns::Status::Cancelled: return {Errno_ECANCELED, EAI_SYSTEM, 0};
        case dns::Status::InvalidHandle: return {Errno_EINVAL, EAI_SYSTEM, 0};
    }
    return {0, EAI_FAIL, 0};
}

HostOutput WriteHost(OutBuffer buffer, const char *name, u32 address) {
    const size_t name_size = std::strlen(name) + 1;
    const size_t size = name_size + 16;
    if (buffer.GetSize() < size) return {NETDB_INTERNAL, Errno_ENOBUFS, 0};
    auto *out = static_cast<u8 *>(buffer.GetPointer());
    std::memcpy(out, name, name_size);
    out += name_size;
    Write(out, u32{0}); // Alias count.
    Write(out, std::byteswap(u16{AF_INET_}));
    Write(out, std::byteswap(u16{sizeof(address)}));
    Write(out, std::byteswap(u32{1}));
    Write(out, std::byteswap(address));
    return {0, 0, static_cast<u32>(size)};
}

HostOutput HostByName(InBuffer input, OutBuffer output, u64 pid, u32 handle) {
    char host[MaxName + 1];
    if (!CopyInput(input, host) || !host[0]) return {NO_RECOVERY, Errno_EINVAL, 0};
    u32 address;
    const auto status = dns::Resolve(host, pid, handle, address);
    return status == dns::Status::Success ? WriteHost(output, host, address) : HostError(status);
}

AddrOutput AddrInfo(InBuffer node, InBuffer service, InBuffer hint, OutBuffer output, u64 pid, u32 handle) {
    Hints hints{};
    if (!ReadHints(hint, hints)) return {0, EAI_BADHINTS, 0};
    constexpr u32 flags = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV |
                          AI_ADDRCONFIG | AI_V4MAPPED_CFG | AI_V4MAPPED | AI_ALL;
    if (hints.flags & ~flags) return {0, EAI_BADFLAGS, 0};
    if (hints.family != AF_UNSPEC_ && hints.family != AF_INET_) return {0, EAI_FAMILY, 0};
    if (hints.type && hints.type != SOCK_STREAM_ && hints.type != SOCK_DGRAM_) return {0, EAI_SOCKTYPE, 0};
    if (hints.protocol && hints.protocol != IPPROTO_TCP_ && hints.protocol != IPPROTO_UDP_) return {0, EAI_PROTOCOL, 0};

    char host[MaxName + 1], port_text[MaxName + 1];
    if (!CopyInput(node, host) || !CopyInput(service, port_text) || (!host[0] && !port_text[0])) {
        return {0, EAI_NONAME, 0};
    }

    u16 port = 0;
    const bool numeric_port = ParsePort(port_text, port);
    if (!numeric_port && (hints.flags & AI_NUMERICSERV)) return {0, EAI_NONAME, 0};
    struct Endpoint { Transport transport; u16 port; };
    std::array<Endpoint, std::size(Transports)> endpoints{};
    size_t count = 0;
    for (const auto &transport : Transports) {
        if ((hints.type && hints.type != transport.type) || (hints.protocol && hints.protocol != transport.protocol)) continue;
        const auto *entry = numeric_port ? nullptr : FindPort(port_text, transport.protocol);
        if (!numeric_port && !entry) continue;
        endpoints[count++] = {transport, entry ? entry->port : port};
    }
    if (!count) return {0, EAI_SERVICE, 0};

    u32 address;
    const char *query = host[0] ? host : (hints.flags & AI_PASSIVE ? "0.0.0.0" : "127.0.0.1");
    if ((hints.flags & AI_NUMERICHOST) && !dns::ParseAddress(query, address)) return {0, EAI_NONAME, 0};
    const auto status = dns::Resolve(query, pid, handle, address);
    if (status != dns::Status::Success) return AddrError(status);

    const char *canonical = hints.flags & AI_CANONNAME ? host : "";
    const size_t name_size = std::strlen(canonical) + 1;
    const size_t size = count * (24 + sizeof(SockAddrIn) + 1) + name_size - 1 + sizeof(u32);
    if (output.GetSize() < size) return {0, EAI_OVERFLOW, 0};
    auto *out = static_cast<u8 *>(output.GetPointer());
    for (size_t i = 0; i < count; ++i) {
        const auto &endpoint = endpoints[i];
        Write(out, std::byteswap(AddrInfoMagic));
        Write(out, std::byteswap(hints.flags));
        Write(out, std::byteswap(u32{AF_INET_}));
        Write(out, std::byteswap(endpoint.transport.type));
        Write(out, std::byteswap(endpoint.transport.protocol));
        Write(out, std::byteswap(u32{sizeof(SockAddrIn)}));
        // The resolver serialization swaps the already network-order sockaddr fields.
        Write(out, SockAddrIn{sizeof(SockAddrIn), AF_INET_, endpoint.port, std::byteswap(address), {}});
        if (i == 0) {
            std::memcpy(out, canonical, name_size);
            out += name_size;
        } else {
            *out++ = 0;
        }
    }
    Write(out, u32{0});
    return {0, 0, static_cast<u32>(size)};
}

HostOutput HostByAddr(const ReverseInput &input, InBuffer buffer, OutBuffer output) {
    if (input.family != AF_INET_ || input.length != sizeof(u32) || buffer.GetSize() != sizeof(u32)) {
        return {NO_RECOVERY, Errno_EINVAL, 0};
    }
    u32 address;
    std::memcpy(&address, buffer.GetPointer(), sizeof(address));
    dns::HostName name;
    const auto status = dns::Reverse(address, 0, 0, name);
    return status == dns::Status::Success ? WriteHost(output, name, address) : HostError(status);
}

void FormatAddress(dns::HostName &name, u32 address) {
    const u32 ip = std::byteswap(address);
    std::snprintf(name, sizeof(name), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

NameOutput NameInfo(LookupInput input, InBuffer sockaddr, OutBuffer host, OutBuffer service, u64 pid) {
    constexpr u32 flags = NI_NOFQDN | NI_NUMERICHOST | NI_NAMEREQD | NI_NUMERICSERV | NI_DGRAM | NI_NUMERICSCOPE;
    if (input.flags & ~flags) return {0, EAI_BADFLAGS};
    if (sockaddr.GetSize() != sizeof(SockAddrIn)) return {0, EAI_FAMILY};
    SockAddrIn address{};
    std::memcpy(&address, sockaddr.GetPointer(), sizeof(address));
    if (address.family != AF_INET_) return {0, EAI_FAMILY};
    if (!host.GetSize() && !service.GetSize()) return {0, EAI_NONAME};
    if (!host.GetSize() || (input.flags & NI_NUMERICHOST)) {
        const auto status = dns::FinishImmediateRequest(pid, input.handle);
        if (status != dns::Status::Success) {
            const auto error = AddrError(status);
            return {error.error, error.result};
        }
    }

    if (host.GetSize()) {
        dns::HostName name;
        if (input.flags & NI_NUMERICHOST) {
            FormatAddress(name, address.address);
        } else {
            const auto status = dns::Reverse(address.address, pid, input.handle, name);
            if (status == dns::Status::Cancelled || status == dns::Status::InvalidHandle ||
                (status != dns::Status::Success && (input.flags & NI_NAMEREQD))) {
                const auto error = AddrError(status);
                return {error.error, error.result};
            }
            if (status != dns::Status::Success) {
                FormatAddress(name, address.address);
            } else if (input.flags & NI_NOFQDN) {
                if (char *dot = std::strchr(name, '.')) *dot = 0;
            }
        }
        if (!CopyString(host, name)) return {0, EAI_OVERFLOW};
    }

    if (service.GetSize()) {
        const u16 port = std::byteswap(address.port);
        const u32 protocol = input.flags & NI_DGRAM ? IPPROTO_UDP_ : IPPROTO_TCP_;
        if (!(input.flags & NI_NUMERICSERV)) {
            for (const auto &entry : Services) {
                if (entry.port == port && entry.protocol == protocol) {
                    return {0, CopyString(service, entry.name) ? 0 : EAI_OVERFLOW};
                }
            }
        }
        char numeric[6];
        std::snprintf(numeric, sizeof(numeric), "%u", static_cast<unsigned>(port));
        if (!CopyString(service, numeric)) return {0, EAI_OVERFLOW};
    }
    return {};
}

constexpr std::string_view HostErrors[] = {
    "Resolver error 0 (no error)", "Unknown host", "Host name lookup failure",
    "Unknown server error", "No address associated with name",
};
constexpr std::string_view AddrErrors[] = {
    "Success", "Address family for hostname not supported", "Temporary failure in name resolution",
    "Invalid value for ai_flags", "Non-recoverable failure in name resolution", "Address family not supported",
    "Memory allocation failure", "No address associated with hostname", "Name or service not known",
    "Service not supported for socket type", "Socket type not supported", "System error",
    "Invalid value for hints", "Unknown protocol", "Argument buffer overflow",
};

struct Resolver {};
Resolver g_resolver;
constexpr CommandTable Commands({
    MakeCommand<Resolver, LookupInput, HostOutput, 1, 1,
        [](Resolver &, LookupInput in, HostOutput &out, auto &args) -> Result {
            out = HostByName(args.In(0), args.Out(0), args.Pid().value, in.handle);
            return 0;
        }>(2, PidMapped),
    MakeCommand<Resolver, ReverseInput, HostOutput, 1, 1,
        [](Resolver &, ReverseInput in, HostOutput &out, auto &args) -> Result {
            out = HostByAddr(in, args.In(0), args.Out(0));
            return 0;
        }>(3, Mapped),
    MakeCommand<Resolver, u32, NoData, 0, 1,
        [](Resolver &, u32 error, NoData &, auto &args) -> Result {
            const auto text = error < std::size(HostErrors) ? HostErrors[error] : "Unknown error";
            return CopyString(args.Out(0), text) ? 0 : BadInput;
        }>(4, Mapped),
    MakeCommand<Resolver, u32, NoData, 0, 1,
        [](Resolver &, u32 error, NoData &, auto &args) -> Result {
            const auto text = error < std::size(AddrErrors) ? AddrErrors[error] : "Unknown error";
            return CopyString(args.Out(0), text) ? 0 : BadInput;
        }>(5, Mapped),
    MakeCommand<Resolver, LookupInput, AddrOutput, 3, 1,
        [](Resolver &, LookupInput in, AddrOutput &out, auto &args) -> Result {
            out = AddrInfo(args.In(0), args.In(1), args.In(2), args.Out(0), args.Pid().value, in.handle);
            return 0;
        }>(6, PidMapped),
    MakeCommand<Resolver, LookupInput, NameOutput, 1, 2,
        [](Resolver &, LookupInput in, NameOutput &out, auto &args) -> Result {
            out = NameInfo(in, args.In(0), args.Out(0), args.Out(1), args.Pid().value);
            return 0;
        }>(7, PidMapped),
    MakeCommand<Resolver, u64, u32, 0, 0,
        [](Resolver &, u64, u32 &out, auto &args) -> Result {
            out = dns::AllocateCancelHandle(args.Pid().value);
            return out ? 0 : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }>(8, PidMapped),
    MakeCommand<Resolver, LookupInput, NoData, 0, 0,
        [](Resolver &, LookupInput in, NoData &, auto &args) -> Result {
            dns::Cancel(args.Pid().value, in.flags);
            return 0;
        }>(9, PidMapped),
    MakeCommand<Resolver, OptionsInput, HostOptionsOutput, 2, 1,
        [](Resolver &, OptionsInput in, HostOptionsOutput &out, auto &args) -> Result {
            const auto result = in.count ? HostOutput{NO_RECOVERY, Errno_EOPNOTSUPP, 0}
                                         : HostByName(args.In(0), args.Out(0), args.Pid().value, 0);
            out = {result.size, result.host_error, result.error};
            return 0;
        }>(10, PidAuto),
    MakeCommand<Resolver, OptionsInput, AddrOptionsOutput, 4, 1,
        [](Resolver &, OptionsInput in, AddrOptionsOutput &out, auto &args) -> Result {
            const auto result = in.count ? AddrOutput{Errno_EOPNOTSUPP, EAI_SYSTEM, 0}
                                         : AddrInfo(args.In(0), args.In(1), args.In(2), args.Out(0), args.Pid().value, 0);
            out = {result.size, result.result, 0, result.error};
            return 0;
        }>(12, PidMixed),
});

ConcurrentServer g_server;
alignas(0x1000) u8 g_worker_stacks[WorkerCount][StackSize];
Thread g_workers[WorkerCount];
Thread g_listener;
alignas(0x1000) u8 g_listener_stack[StackSize];

bool DispatchResolver(void *object, ConcurrentRequest &request, ConcurrentReply &reply) {
    if (Dispatch(object, Commands, request, reply)) return true;
    const u32 id = request.input.header.command_id;
    switch (id) {
        case 0: case 1: case 11: case 13: case 14: case 15: case 16:
        case 100: case 101: case 102: case 103: case 104:
            reply.result = Unsupported;
            return true;
        default:
            return false;
    }
}

void *Open(void *) { return &g_resolver; }
void Release(void *) {}
void Worker(void *) { KUMO_ABORT_RESULT(g_server.Worker()); }
void Listen(void *) { KUMO_ABORT_RESULT(g_server.Run()); }

} // namespace

void StartDnsServer() {
    KUMO_ABORT_RESULT(g_server.Initialize(WorkerCount));
    KUMO_ABORT_RESULT(g_server.Register({"sfdnsres", WorkerCount, nullptr, Open, Release, Release, DispatchResolver}));
    s32 priority;
    KUMO_ABORT_RESULT(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE));
    for (size_t i = 0; i < WorkerCount; ++i) {
        KUMO_ABORT_RESULT(threadCreate(&g_workers[i], Worker, nullptr, g_worker_stacks[i], StackSize, priority, -2));
        KUMO_ABORT_RESULT(threadStart(&g_workers[i]));
    }
    KUMO_ABORT_RESULT(threadCreate(&g_listener, Listen, nullptr, g_listener_stack, StackSize, priority, -2));
    KUMO_ABORT_RESULT(threadStart(&g_listener));
}

}
