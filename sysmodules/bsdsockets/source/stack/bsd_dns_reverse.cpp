#include "bsd_dns.hpp"
#include "bsd_stack.hpp"
#include <span>
#include <strings.h>
#include <string_view>
extern "C" {
#include <lwip/dns.h>
#include <lwip/udp.h>
}

namespace kumo::horizon::hznet::dns {
namespace {

constexpr u16 PtrType = 12;
constexpr u16 InternetClass = 1;
constexpr size_t HeaderSize = 12;
constexpr size_t MaxPacketSize = 512;
constexpr u64 ServerTimeoutNs = 2'000'000'000;

u16 ReadWord(std::span<const u8> message, size_t offset) {
    return static_cast<u16>((message[offset] << 8) | message[offset + 1]);
}
void WriteWord(std::span<u8> message, size_t offset, u16 value) {
    message[offset] = value >> 8;
    message[offset + 1] = value & 0xff;
}

// Decode a DNS name, including compression pointers. Offset advances past the
// original encoded name, even when the remaining labels come from elsewhere.
bool ReadName(std::span<const u8> packet, size_t &offset, HostName &name) {
    size_t cursor = offset;
    size_t length = 0;
    bool jumped = false;
    for (size_t labels = 0; labels < 128 && cursor < packet.size(); ++labels) {
        const u8 count = packet[cursor++];
        if ((count & 0xc0) == 0xc0) {
            if (cursor >= packet.size()) return false;
            const size_t target = ((count & 0x3f) << 8) | packet[cursor++];
            if (target < HeaderSize || target >= cursor - 2) return false;
            if (!jumped) offset = cursor;
            cursor = target;
            jumped = true;
            continue;
        }
        if (count > 63 || count > packet.size() - cursor) return false;
        if (!count) {
            if (!jumped) offset = cursor;
            name[length] = 0;
            return true;
        }
        if (length + (length != 0) + count >= sizeof(name)) return false;
        if (length) name[length++] = '.';
        for (size_t i = 0; i < count; ++i) {
            const u8 ch = packet[cursor++];
            if (ch < '!' || ch > '~' || ch == '.') return false;
            name[length++] = ch;
        }
    }
    return false;
}

struct Query {
    ip_addr_t server{};
    HostName question{};
    HostName answer{};
    u16 transaction = 0;
    bool complete = false;
    Status status = Status::TemporaryFailure;
};

void Receive(void *argument, udp_pcb *, pbuf *p, const ip_addr_t *address, u16 port) {
    auto &query = *static_cast<Query *>(argument);
    const size_t size = p->tot_len;
    std::array<u8, MaxPacketSize> bytes;
    const bool valid = !query.complete && size >= HeaderSize && size <= bytes.size() &&
                       port == 53 && ip_addr_cmp(address, &query.server) &&
                       pbuf_copy_partial(p, bytes.data(), size, 0) == size;
    pbuf_free(p);
    if (!valid) return;

    const std::span packet(bytes.data(), size);
    const u16 flags = ReadWord(packet, 2);
    if (ReadWord(packet, 0) != query.transaction || (flags & 0xf800) != 0x8000 ||
        ReadWord(packet, 4) != 1) return;
    size_t offset = HeaderSize;
    HostName name;
    if (!ReadName(packet, offset, name) || strcasecmp(name, query.question) ||
        size - offset < 4 || ReadWord(packet, offset) != PtrType ||
        ReadWord(packet, offset + 2) != InternetClass) return;
    offset += 4;

    const u16 response_code = flags & 0xf;
    if (!(flags & 0x0200) && response_code == 0) {
        query.status = Status::NotFound;
        for (u16 i = 0; i < ReadWord(packet, 6); ++i) {
            if (!ReadName(packet, offset, name) || size - offset < 10) return;
            const u16 type = ReadWord(packet, offset);
            const u16 dns_class = ReadWord(packet, offset + 2);
            const size_t length = ReadWord(packet, offset + 8);
            offset += 10;
            if (length > size - offset) return;
            if (type == PtrType && dns_class == InternetClass && !strcasecmp(name, query.question)) {
                size_t end = offset;
                if (!ReadName(packet, end, query.answer) || !query.answer[0] || end != offset + length) return;
                query.status = Status::Success;
                break;
            }
            offset += length;
        }
    } else {
        query.status = response_code == 3 ? Status::NotFound : Status::TemporaryFailure;
    }
    query.complete = true;
    stack::NotifyActivity();
}

Status QueryServer(udp_pcb *socket, Query &query, const bool *cancelled) {
    ::randomGet(&query.transaction, sizeof(query.transaction));
    std::array<u8, MaxPacketSize> wire{};
    WriteWord(wire, 0, query.transaction);
    WriteWord(wire, 2, 0x0100); // Recursion desired.
    WriteWord(wire, 4, 1);
    size_t offset = HeaderSize;
    std::string_view question(query.question);
    while (!question.empty()) {
        const size_t dot = question.find('.');
        const auto label = question.substr(0, dot);
        wire[offset++] = label.size();
        std::memcpy(wire.data() + offset, label.data(), label.size());
        offset += label.size();
        question.remove_prefix(dot == question.npos ? question.size() : dot + 1);
    }
    wire[offset++] = 0;
    WriteWord(wire, offset, PtrType);
    WriteWord(wire, offset + 2, InternetClass);
    offset += 4;

    auto *p = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16>(offset), PBUF_RAM);
    if (!p) return Status::TemporaryFailure;
    const err_t copied = pbuf_take(p, wire.data(), static_cast<u16>(offset));
    const err_t sent = copied == ERR_OK ? udp_sendto(socket, p, &query.server, 53) : copied;
    pbuf_free(p);
    if (sent != ERR_OK) return Status::TemporaryFailure;
    stack::KickNetworkThread();

    const u64 deadline = svcGetSystemTick() + armNsToTicks(ServerTimeoutNs);
    while (!query.complete && !(cancelled && *cancelled)) {
        const u64 now = svcGetSystemTick();
        if (now >= deadline) return Status::TemporaryFailure;
        stack::WaitForActivity(std::chrono::nanoseconds(armTicksToNs(deadline - now)));
    }
    return cancelled && *cancelled ? Status::Cancelled : query.status;
}

} // namespace

Status LookupPtr(u32 address, const bool *cancelled, HostName &name) {
    if (ip_addr_isany(dns_getserver(0)) && ip_addr_isany(dns_getserver(1))) return Status::TemporaryFailure;

    auto *socket = udp_new();
    if (!socket) return Status::TemporaryFailure;
    struct Close {
        udp_pcb *socket;
        ~Close() { udp_remove(socket); }
    } close{socket};

    // Use an independent random source port as well as a random transaction ID.
    err_t bound = ERR_USE;
    for (unsigned attempt = 0; attempt < 16 && bound == ERR_USE; ++attempt) {
        u16 port;
        ::randomGet(&port, sizeof(port));
        bound = udp_bind(socket, IP_ADDR_ANY, static_cast<u16>(0xc000 | (port & 0x3fff)));
    }
    if (bound != ERR_OK) return Status::TemporaryFailure;

    Query query;
    const u32 ip = std::byteswap(address);
    std::snprintf(query.question, sizeof(query.question), "%u.%u.%u.%u.in-addr.arpa",
                  ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    udp_recv(socket, Receive, &query);
    for (u8 index = 0; index < DNS_MAX_SERVERS; ++index) {
        query.server = *dns_getserver(index);
        if (ip_addr_isany(&query.server)) continue;
        query.complete = false;
        const auto status = QueryServer(socket, query, cancelled);
        if (status == Status::Success) {
            std::memcpy(name, query.answer, std::strlen(query.answer) + 1);
        }
        if (status != Status::TemporaryFailure) return status;
    }
    return Status::TemporaryFailure;
}

}
