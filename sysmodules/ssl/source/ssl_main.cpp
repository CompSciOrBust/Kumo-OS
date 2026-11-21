#include "ssl_tls.hpp"
#include <kumo/ipc/dispatch.hpp>
#include <kumo/ipc/object_server.hpp>
#include <mbedtls/net_sockets.h>
#include <poll.h>
extern "C" {
#include <switch/services/bsd.h>
}

namespace horizon::ssl {
namespace {
using namespace kumo::ipc;

constexpr CommandOptions Mapped{.buffers = BufferMode::Mapped};
constexpr CommandOptions WithPid{.pid = true};
constexpr u32 DefaultVerify = SslVerifyOption_PeerCa | SslVerifyOption_HostName;
constexpr u32 DefaultTimeoutMs = 300000;
constexpr u32 TlsVersionFlags = SslVersion_Auto | SslVersion_TlsV10 | SslVersion_TlsV11 |
                                SslVersion_TlsV12 | SslVersion_TlsV13;
constexpr size_t MaxRecord = 16384;
constexpr size_t MaxImportedCertBytes = 256 * 1024;
constexpr size_t MaxServerPki = 71;
constexpr u32 MaxContexts = 64;
constexpr u32 MaxConnections = 64;
std::atomic<u32> g_context_count{};
std::atomic<u32> g_connection_count{};

struct CreateContextInput {
    static constexpr size_t WireBytes = 16;
    u32 version;
    u32 padding;
    u64 pid_placeholder;
};
struct OptionInput {
    static constexpr size_t WireBytes = 8;
    u8 value;
    u8 padding[3];
    u32 option;
};
struct PollInput { static constexpr size_t WireBytes = 8; u32 events, timeout; };
struct Pair { static constexpr size_t WireBytes = 8; u32 first, second; };

void Log(const char *message, Result rc = 0) {
    char line[160];
    const auto size = std::snprintf(line, sizeof(line), "[ssl] %s (0x%x)\n", message, rc);
    if (size > 0) svcOutputDebugString(line, std::min(static_cast<size_t>(size), sizeof(line) - 1));
}

class Context;

class Connection final : public ServiceObject {
public:
    explicit Connection(Context &context);
    ~Connection() override;
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override;

private:
    Context &context_;
    ObjectRef context_ref_;
    kumo::Mutex mutex_;
    Certificates trust_;
    mbedtls_ssl_context tls_{};
    mbedtls_ssl_config config_{};
    s32 source_fd_ = -1;
    s32 fd_ = -1;
    std::array<char, 256> host_{};
    std::array<char, 256> alpn_bytes_{};
    std::array<const char *, 129> alpn_{};
    u32 io_mode_ = SslIoMode_Blocking;
    u32 verify_ = DefaultVerify;
    u32 io_timeout_ms_ = DefaultTimeoutMs;
    u32 options_ = 0;
    Result verify_error_ = 0;
    Result handshake_error_ = 0;
    bool configured_ = false;
    bool connected_ = false;
    std::array<u8, MaxRecord> write_buffer_{};
    size_t write_size_ = 0;
    std::array<u8, MaxRecord> peek_buffer_{};
    size_t peek_offset_ = 0;
    size_t peek_size_ = 0;

    Result Attach(s32 fd, s32 &out);
    Result SetHost(InBuffer buffer);
    Result SetAlpn(InBuffer buffer);
    Result Configure();
    Result Handshake();
    Result Read(OutBuffer buffer, u32 &out, bool peek);
    Result Write(InBuffer buffer, u32 &out);
    Result Poll(PollInput input, u32 &out);
    Result Wait(int tls_result, u64 start);
    Result CopyCertificates(void *buffer, size_t capacity, Pair &out);
    bool Option(u32 option) const { return (options_ & BIT(option)) != 0; }
};

class Context final : public ServiceObject {
public:
    Context(u64 pid, u32 version) : pid(pid), version(version) {}
    ~Context() override { --g_context_count; }
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override;

    Result ConfigureTrust(Certificates &copy, mbedtls_ssl_config &config);
    const u64 pid;
    const u32 version;
    std::atomic<u32> connections{};

private:
    struct Pki {
        u64 id{};
        std::unique_ptr<Certificates> cert;
    };
    kumo::Mutex mutex_;
    std::array<Pki, MaxServerPki> pki_{};
    u64 next_id_ = 1;
    s32 crl_date_check_ = 1;

    Result Import(u32 format, InBuffer buffer, u64 &out);
    Result Remove(u64 id);
};

Connection::Connection(Context &context) : context_(context) {
    context.Retain();
    context_ref_ = ObjectRef::Adopt(&context);
    ++context.connections;
    mbedtls_ssl_init(&tls_);
    mbedtls_ssl_config_init(&config_);
}

Connection::~Connection() {
    mbedtls_ssl_free(&tls_);
    mbedtls_ssl_config_free(&config_);
    if (fd_ >= 0) bsdClose(fd_);
    --context_.connections;
    --g_connection_count;
}

Result Connection::Attach(s32 fd, s32 &out) {
    KUMO_UNLESS(fd_ < 0 && fd >= 0 && context_.pid != 0, InvalidSocket);
    KUMO_TRY(DuplicateSocket(fd_, fd, context_.pid));
    source_fd_ = fd;
    out = Option(SslOptionType_DoNotCloseSocket) ? -1 : source_fd_;
    return 0;
}

Result Connection::SetHost(InBuffer buffer) {
    const auto *text = reinterpret_cast<const char *>(buffer.GetPointer());
    const size_t size = buffer.GetSize();
    KUMO_UNLESS(!configured_ && size > 0 && size <= 255, InvalidArgument);
    const size_t length = strnlen(text, size);
    KUMO_UNLESS(length > 0 && (length == size || length + 1 == size), InvalidArgument);
    std::memcpy(host_.data(), text, length);
    host_[length] = 0;
    return 0;
}

Result Connection::SetAlpn(InBuffer buffer) {
    const auto *data = reinterpret_cast<const u8 *>(buffer.GetPointer());
    const size_t size = buffer.GetSize();
    KUMO_UNLESS(fd_ >= 0 && !configured_ && size >= 2 && size <= alpn_bytes_.size(), InvalidArgument);
    for (size_t offset = 0; offset < size;) {
        const size_t length = data[offset++];
        KUMO_UNLESS(length && length <= size - offset && !std::memchr(data + offset, 0, length), InvalidArgument);
        offset += length;
    }
    size_t index = 0;
    for (size_t offset = 0; offset < size; ++index) {
        const size_t length = data[offset];
        alpn_[index] = alpn_bytes_.data() + offset;
        std::memcpy(alpn_bytes_.data() + offset, data + offset + 1, length);
        alpn_bytes_[offset + length] = 0;
        offset += length + 1;
    }
    alpn_[index] = nullptr;
    return 0;
}

Result Connection::Configure() {
    const u32 versions = context_.version;
    const bool automatic = (versions & SslVersion_Auto) || versions == SslVersion_Auto24;
    const bool tls12 = automatic || (versions & SslVersion_TlsV12);
    const bool tls13 = (versions & SslVersion_TlsV13) || (automatic && (versions >> 24) >= 3);
    KUMO_UNLESS(tls12 || tls13, Unsupported);

    KUMO_UNLESS(mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT) == 0, OutOfMemory);
    mbedtls_ssl_conf_min_tls_version(&config_, tls12 ? MBEDTLS_SSL_VERSION_TLS1_2 : MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&config_, tls13 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&config_, MBEDTLS_SSL_VERIFY_REQUIRED);
    KUMO_TRY(context_.ConfigureTrust(trust_, config_));
    if (Option(SslOptionType_EnableAlpn) && alpn_[0]) {
        KUMO_UNLESS(mbedtls_ssl_conf_alpn_protocols(&config_, alpn_.data()) == 0, InvalidArgument);
    }
    KUMO_UNLESS(mbedtls_ssl_setup(&tls_, &config_) == 0, OutOfMemory);
    KUMO_UNLESS(mbedtls_ssl_set_hostname(&tls_, host_.data()) == 0, OutOfMemory);
    mbedtls_ssl_set_bio(&tls_, &fd_, Send, Receive, nullptr);
    configured_ = true;
    return 0;
}

Result Connection::Wait(int tls_result, u64 start) {
    if (tls_result == MBEDTLS_ERR_NET_CONN_RESET) return SslResult(209);
    if (tls_result == MBEDTLS_ERR_NET_RECV_FAILED || tls_result == MBEDTLS_ERR_NET_SEND_FAILED) return NoConnection;
    if (tls_result != MBEDTLS_ERR_SSL_WANT_READ && tls_result != MBEDTLS_ERR_SSL_WANT_WRITE) {
        return SslResult(311);
    }
    if (io_mode_ == SslIoMode_NonBlocking) return WouldBlock;
    const u64 elapsed = armTicksToNs(armGetSystemTick() - start) / 1000000;
    if (elapsed >= io_timeout_ms_) return Timeout;
    short ready{};
    const short events = tls_result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
    KUMO_TRY(WaitSocket(fd_, events, static_cast<int>(io_timeout_ms_ - elapsed), ready));
    return ready ? 0 : Timeout;
}

Result Connection::Handshake() {
    if (connected_) return 0;
    if (handshake_error_) return handshake_error_;
    KUMO_UNLESS(fd_ >= 0, InvalidSocket);
    KUMO_UNLESS(host_[0] != 0, InvalidArgument);
    KUMO_TRY(ReadCertificateTime());
    if (!configured_) {
        handshake_error_ = Configure();
        KUMO_TRY(handshake_error_);
    }

    const u64 start = armGetSystemTick();
    for (;;) {
        const int rc = mbedtls_ssl_handshake(&tls_);
        if (rc == 0) {
            connected_ = true;
            return 0;
        }
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            verify_error_ = CertificateError(mbedtls_ssl_get_verify_result(&tls_));
            handshake_error_ = verify_error_ ? verify_error_ : SslResult(307);
            return handshake_error_;
        }
        const Result result = Wait(rc, start);
        if (result) {
            if (result != WouldBlock && result != Timeout) handshake_error_ = result;
            return result;
        }
    }
}

Result Connection::Read(OutBuffer buffer, u32 &out, bool peek) {
    KUMO_UNLESS(connected_, NoConnection);
    KUMO_UNLESS(buffer.GetSize() > 0, InvalidArgument);
    KUMO_UNLESS(!write_size_, WouldBlock);
    const u64 start = armGetSystemTick();
    if (!peek_size_) {
        if (peek) peek_offset_ = 0;
        auto *destination = peek ? peek_buffer_.data() : reinterpret_cast<u8 *>(buffer.GetPointer());
        const size_t size = std::min(buffer.GetSize(), MaxRecord);
        for (;;) {
            const int rc = mbedtls_ssl_read(&tls_, destination, size);
            // Session resumption is disabled at the IPC layer. Consume tickets
            // as protocol messages and keep reading for application data.
            if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                out = 0;
                return 0;
            }
            if (rc > 0) {
                if (!peek) {
                    out = rc;
                    return 0;
                }
                peek_size_ = rc;
                break;
            }
            const Result result = Wait(rc, start);
            if (result) {
                if (result != WouldBlock && result != Timeout) {
                    connected_ = false;
                    handshake_error_ = result;
                }
                return result;
            }
        }
    }

    const size_t size = std::min(buffer.GetSize(), peek_size_);
    std::memcpy(buffer.GetPointer(), peek_buffer_.data() + peek_offset_, size);
    if (!peek) {
        peek_offset_ += size;
        peek_size_ -= size;
    }
    out = size;
    return 0;
}

Result Connection::Write(InBuffer buffer, u32 &out) {
    KUMO_UNLESS(connected_, NoConnection);
    const size_t size = std::min(buffer.GetSize(), write_buffer_.size());
    KUMO_UNLESS(size > 0, InvalidArgument);
    // Mbed TLS requires the same buffer and data on retry after WANT_READ or WANT_WRITE.
    if (write_size_) {
        KUMO_UNLESS(size == write_size_ && !std::memcmp(write_buffer_.data(), buffer.GetPointer(), size), InvalidArgument);
    } else {
        std::memcpy(write_buffer_.data(), buffer.GetPointer(), size);
        write_size_ = size;
    }
    const u64 start = armGetSystemTick();
    for (;;) {
        const int rc = mbedtls_ssl_write(&tls_, write_buffer_.data(), size);
        if (rc >= 0) {
            out = rc;
            write_size_ = 0;
            return 0;
        }
        const Result result = Wait(rc, start);
        if (result) {
            if (result != WouldBlock && result != Timeout) {
                write_size_ = 0;
                connected_ = false;
                handshake_error_ = result;
            }
            return result;
        }
    }
}

Result Connection::Poll(PollInput input, u32 &out) {
    KUMO_UNLESS(fd_ >= 0, InvalidSocket);
    KUMO_UNLESS(input.events && !(input.events & ~7u), InvalidArgument);
    KUMO_UNLESS(input.timeout <= INT32_MAX, InvalidArgument);
    short events = 0;
    if (input.events & 1u) events |= POLLIN;
    if (input.events & 2u) events |= POLLOUT;
    const bool can_read = (input.events & 1u) && connected_ &&
                          (peek_size_ || mbedtls_ssl_get_bytes_avail(&tls_));
    short ready = 0;
    KUMO_TRY(WaitSocket(fd_, events, can_read ? 0 : static_cast<int>(input.timeout), ready));
    out = ((ready & (POLLIN | POLLHUP)) || can_read ? 1u : 0u)
        | (ready & POLLOUT ? 2u : 0u)
        | (ready & (POLLERR | POLLHUP) ? 4u : 0u);
    out &= input.events;
    return out ? 0 : Timeout;
}

Result Connection::CopyCertificates(void *buffer, size_t capacity, Pair &out) {
    KUMO_UNLESS(connected_, NoConnection);
    const auto *leaf = mbedtls_ssl_get_peer_cert(&tls_);
    KUMO_UNLESS(leaf, SslResult(302));
    const bool chain = Option(SslOptionType_GetServerCertChain);
    size_t total = chain ? sizeof(SslServerCertDetailHeader) : 0;
    u32 count = 0;
    for (auto *cert = leaf; cert; cert = chain ? cert->next : nullptr) {
        total += cert->raw.len + (chain ? sizeof(SslServerCertDetailEntry) : 0);
        ++count;
    }
    out = {static_cast<u32>(total), count};
    if (!buffer) return 0;
    KUMO_UNLESS(capacity >= total, InvalidArgument);
    auto *data = static_cast<u8 *>(buffer);
    if (!chain) {
        std::memcpy(data, leaf->raw.p, leaf->raw.len);
        return 0;
    }

    const SslServerCertDetailHeader header{0x4E4D684374726543ULL, count, 0};
    std::memcpy(data, &header, sizeof(header));
    size_t offset = sizeof(header) + sizeof(SslServerCertDetailEntry) * count;
    auto *entry_bytes = data + sizeof(header);
    for (auto *cert = leaf; cert; cert = cert->next) {
        const SslServerCertDetailEntry entry{static_cast<u32>(cert->raw.len), static_cast<u32>(offset)};
        std::memcpy(entry_bytes, &entry, sizeof(entry));
        std::memcpy(data + offset, cert->raw.p, cert->raw.len);
        entry_bytes += sizeof(entry);
        offset += cert->raw.len;
    }
    return 0;
}

bool Connection::Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) {
    std::scoped_lock lock(mutex_);
    static constexpr CommandTable commands({
        MakeCommand<Connection, s32, s32, 0, 0,
            [](Connection &self, s32 fd, s32 &out, auto &) { return self.Attach(fd, out); }>(0),
        MakeCommand<Connection, NoData, NoData, 1, 0,
            [](Connection &self, NoData, NoData &, auto &args) { return self.SetHost(args.In(0)); }>(1, Mapped),
        MakeCommand<Connection, u32, NoData, 0, 0,
            [](Connection &self, u32 flags, NoData &, auto &) -> Result {
                KUMO_UNLESS(!self.configured_ && (flags & DefaultVerify) == DefaultVerify, InvalidArgument);
                KUMO_UNLESS(!(flags & ~(DefaultVerify | SslVerifyOption_DateCheck)), Unsupported);
                self.verify_ = flags;
                return 0;
            }>(2),
        MakeCommand<Connection, u32, NoData, 0, 0,
            [](Connection &self, u32 mode, NoData &, auto &) -> Result {
                KUMO_UNLESS(self.fd_ >= 0, InvalidSocket);
                KUMO_UNLESS(mode == SslIoMode_Blocking || mode == SslIoMode_NonBlocking, InvalidArgument);
                self.io_mode_ = mode;
                return 0;
            }>(3),
        MakeCommand<Connection, NoData, s32, 0, 0,
            [](Connection &self, NoData, s32 &out, auto &) -> Result {
                KUMO_UNLESS(self.fd_ >= 0, InvalidSocket);
                out = self.source_fd_;
                return 0;
            }>(4),
        MakeCommand<Connection, NoData, u32, 0, 1,
            [](Connection &self, NoData, u32 &out, auto &args) -> Result {
                const auto buffer = args.Out(0);
                out = std::strlen(self.host_.data());
                KUMO_UNLESS(buffer.GetSize() > out, InvalidArgument);
                std::memcpy(buffer.GetPointer(), self.host_.data(), out + 1);
                return 0;
            }>(5, Mapped),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &self, NoData, u32 &out, auto &) -> Result { out = self.verify_; return 0; }>(6),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &self, NoData, u32 &out, auto &) -> Result { out = self.io_mode_; return 0; }>(7),
        MakeCommand<Connection, NoData, NoData, 0, 0,
            [](Connection &self, NoData, NoData &, auto &) { return self.Handshake(); }>(8),
        MakeCommand<Connection, NoData, Pair, 0, 1,
            [](Connection &self, NoData, Pair &out, auto &args) -> Result {
                KUMO_TRY(self.Handshake());
                const auto buffer = args.Out(0);
                return self.CopyCertificates(buffer.GetPointer(), buffer.GetSize(), out);
            }>(9, Mapped),
        MakeCommand<Connection, NoData, u32, 0, 1,
            [](Connection &self, NoData, u32 &out, auto &args) { return self.Read(args.Out(0), out, false); }>(10, Mapped),
        MakeCommand<Connection, NoData, u32, 1, 0,
            [](Connection &self, NoData, u32 &out, auto &args) { return self.Write(args.In(0), out); }>(11, Mapped),
        MakeCommand<Connection, NoData, s32, 0, 0,
            [](Connection &self, NoData, s32 &out, auto &) -> Result {
                KUMO_UNLESS(self.connected_, NoConnection);
                out = self.peek_size_ + mbedtls_ssl_get_bytes_avail(&self.tls_);
                return 0;
            }>(12),
        MakeCommand<Connection, NoData, u32, 0, 1,
            [](Connection &self, NoData, u32 &out, auto &args) { return self.Read(args.Out(0), out, true); }>(13, Mapped),
        MakeCommand<Connection, PollInput, u32, 0, 0,
            [](Connection &self, PollInput input, u32 &out, auto &) { return self.Poll(input, out); }>(14),
        MakeCommand<Connection, NoData, NoData, 0, 0,
            [](Connection &self, NoData, NoData &, auto &) { return self.verify_error_; }>(15),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &self, NoData, u32 &out, auto &) -> Result {
                Pair size{};
                KUMO_TRY(self.CopyCertificates(nullptr, 0, size));
                out = size.first;
                return 0;
            }>(16),
        MakeCommand<Connection, u32, NoData, 0, 0,
            [](Connection &, u32 mode, NoData &, auto &) -> Result {
                return mode == SslSessionCacheMode_None ? 0 : Unsupported;
            }>(17),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &, NoData, u32 &out, auto &) -> Result { out = SslSessionCacheMode_None; return 0; }>(18),
        MakeCommand<Connection, NoData, NoData, 0, 0,
            [](Connection &, NoData, NoData &, auto &) -> Result { return 0; }>(19),
        MakeCommand<Connection, u32, NoData, 0, 0,
            [](Connection &, u32 mode, NoData &, auto &) -> Result {
                return mode == SslRenegotiationMode_None ? 0 : Unsupported;
            }>(20),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &, NoData, u32 &out, auto &) -> Result { out = SslRenegotiationMode_None; return 0; }>(21),
        MakeCommand<Connection, OptionInput, NoData, 0, 0,
            [](Connection &self, const OptionInput &input, NoData &, auto &) -> Result {
                KUMO_UNLESS(input.option <= SslOptionType_EnableAlpn && input.value <= 1, InvalidArgument);
                KUMO_UNLESS(input.option != SslOptionType_SkipDefaultVerify || !input.value, Unsupported);
                KUMO_UNLESS(input.option != SslOptionType_DoNotCloseSocket || self.fd_ < 0, InvalidArgument);
                KUMO_UNLESS(input.option != SslOptionType_EnableAlpn || !self.configured_, InvalidArgument);
                if (input.value) self.options_ |= BIT(input.option);
                else self.options_ &= ~BIT(input.option);
                return 0;
            }>(22),
        MakeCommand<Connection, u32, u8, 0, 0,
            [](Connection &self, u32 option, u8 &out, auto &) -> Result {
                KUMO_UNLESS(option <= SslOptionType_EnableAlpn, InvalidArgument);
                out = self.Option(option);
                return 0;
            }>(23),
        MakeCommand<Connection, NoData, Pair, 0, 1,
            [](Connection &self, NoData, Pair &out, auto &args) -> Result {
                if (self.verify_error_) {
                    const auto buffer = args.Out(0);
                    KUMO_UNLESS(buffer.GetSize() >= sizeof(Result), InvalidArgument);
                    std::memcpy(buffer.GetPointer(), &self.verify_error_, sizeof(Result));
                    out = {1, 1};
                } else {
                    out = {};
                }
                return 0;
            }>(24, Mapped),
        MakeCommand<Connection, u32, NoData, 0, 1,
            [](Connection &self, u32 version, NoData &, auto &args) -> Result {
                const auto buffer = args.Out(0);
                KUMO_UNLESS(version == 1 && buffer.GetSize() == sizeof(SslCipherInfo), InvalidArgument);
                KUMO_UNLESS(self.connected_, NoConnection);
                SslCipherInfo info{};
                std::snprintf(info.cipher, sizeof(info.cipher), "%s", mbedtls_ssl_get_ciphersuite(&self.tls_));
                std::replace(std::begin(info.cipher), std::end(info.cipher), '-', '_');
                std::snprintf(info.protocol_version, sizeof(info.protocol_version), "%s",
                              mbedtls_ssl_get_version(&self.tls_));
                std::memcpy(buffer.GetPointer(), &info, sizeof(info));
                return 0;
            }>(25, Mapped),
        MakeCommand<Connection, NoData, NoData, 1, 0,
            [](Connection &self, NoData, NoData &, auto &args) { return self.SetAlpn(args.In(0)); }>(26, Mapped),
        MakeCommand<Connection, NoData, Pair, 0, 1,
            [](Connection &self, NoData, Pair &out, auto &args) -> Result {
                KUMO_UNLESS(self.fd_ >= 0, InvalidSocket);
                out = {};
                const char *protocol = self.connected_ ? mbedtls_ssl_get_alpn_protocol(&self.tls_) : nullptr;
                if (protocol) {
                    const auto buffer = args.Out(0);
                    const size_t size = std::strlen(protocol);
                    KUMO_UNLESS(size <= buffer.GetSize(), InvalidArgument);
                    std::memcpy(buffer.GetPointer(), protocol, size);
                    out = {SslAlpnProtoState_Negotiated, static_cast<u32>(size)};
                }
                return 0;
            }>(27, Mapped),
        MakeCommand<Connection, u32, NoData, 0, 0,
            [](Connection &self, u32 milliseconds, NoData &, auto &) -> Result {
                KUMO_UNLESS(milliseconds <= INT32_MAX, InvalidArgument);
                self.io_timeout_ms_ = milliseconds;
                return 0;
            }>(34),
        MakeCommand<Connection, NoData, u32, 0, 0,
            [](Connection &self, NoData, u32 &out, auto &) -> Result { out = self.io_timeout_ms_; return 0; }>(35),
    });
    if (commands.Dispatch(this, request, reply)) return true;
    const u32 command = request.input.header.command_id;
    if (command >= 28 && command <= 37) {
        reply.result = Unsupported;
        return true;
    }
    return false;
}

Result Context::Import(u32 format, InBuffer buffer, u64 &out) {
    KUMO_UNLESS(format == SslCertificateFormat_Pem || format == SslCertificateFormat_Der, InvalidArgument);
    KUMO_UNLESS(buffer.GetSize() && buffer.GetSize() <= MaxImportedCertBytes, InvalidArgument);
    auto cert = std::unique_ptr<Certificates>(new (std::nothrow) Certificates);
    KUMO_UNLESS(cert, OutOfMemory);
    const auto bytes = std::span(reinterpret_cast<const u8 *>(buffer.GetPointer()), buffer.GetSize());
    KUMO_UNLESS(cert->Parse(bytes, format == SslCertificateFormat_Pem) == 0, SslResult(307));
    for (auto &entry : pki_) {
        if (!entry.cert) {
            entry.id = next_id_++;
            entry.cert = std::move(cert);
            out = entry.id;
            return 0;
        }
    }
    return SslResult(218);
}

Result Context::Remove(u64 id) {
    for (auto &entry : pki_) {
        if (entry.cert && entry.id == id) {
            entry.cert.reset();
            return 0;
        }
    }
    return InvalidArgument;
}

Result Context::ConfigureTrust(Certificates &copy, mbedtls_ssl_config &config) {
    std::scoped_lock lock(mutex_);
    const bool has_imports = std::any_of(pki_.begin(), pki_.end(), [](const Pki &entry) {
        return entry.cert != nullptr;
    });
    if (!has_imports) {
        mbedtls_ssl_conf_ca_chain(&config, &DefaultTrust().chain, nullptr);
        return 0;
    }
    KUMO_UNLESS(copy.Append(DefaultTrust()) == 0, OutOfMemory);
    for (auto &entry : pki_) {
        if (entry.cert) KUMO_UNLESS(copy.Append(*entry.cert) == 0, OutOfMemory);
    }
    mbedtls_ssl_conf_ca_chain(&config, &copy.chain, nullptr);
    return 0;
}

bool Context::Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) {
    std::scoped_lock lock(mutex_);
    static constexpr CommandTable commands({
        MakeCommand<Context, Pair, NoData, 0, 0,
            [](Context &self, Pair in, NoData &, auto &) -> Result {
                KUMO_UNLESS(in.first == SslContextOption_CrlImportDateCheckEnable && in.second <= 1, InvalidArgument);
                self.crl_date_check_ = in.second;
                return 0;
            }>(0),
        MakeCommand<Context, u32, s32, 0, 0,
            [](Context &self, u32 option, s32 &out, auto &) -> Result {
                KUMO_UNLESS(option == SslContextOption_CrlImportDateCheckEnable, InvalidArgument);
                out = self.crl_date_check_;
                return 0;
            }>(1),
        MakeCommand<Context, NoData, NoData, 0, 0,
            [](Context &self, NoData, NoData &, auto &args) -> Result {
                if (g_connection_count.fetch_add(1) >= MaxConnections) {
                    --g_connection_count;
                    return OutOfMemory;
                }
                auto object = MakeObject<Connection>(self);
                if (!object) {
                    --g_connection_count;
                    return OutOfMemory;
                }
                args.ReturnObject(std::move(object));
                return 0;
            }>(2),
        MakeCommand<Context, NoData, u32, 0, 0,
            [](Context &self, NoData, u32 &out, auto &) -> Result { out = self.connections; return 0; }>(3),
        MakeCommand<Context, u32, u64, 1, 0,
            [](Context &self, u32 format, u64 &out, auto &args) { return self.Import(format, args.In(0), out); }>(4, Mapped),
        MakeCommand<Context, u64, NoData, 0, 0,
            [](Context &self, u64 id, NoData &, auto &) { return self.Remove(id); }>(6),
    });
    if (commands.Dispatch(this, request, reply)) return true;
    const u32 command = request.input.header.command_id;
    if (command >= 5 && command <= 13) {
        reply.result = Unsupported;
        return true;
    }
    return false;
}

class SslService final : public ServiceObject {
    std::atomic<u32> interface_version_{};
public:
    bool Dispatch(ConcurrentRequest &request, ConcurrentReply &reply) override {
        static constexpr CommandTable commands({
            MakeCommand<SslService, CreateContextInput, NoData, 0, 0,
                [](SslService &, const CreateContextInput &in, NoData &, auto &args) -> Result {
                    constexpr u32 supported = SslVersion_Auto | SslVersion_TlsV12 | SslVersion_TlsV13;
                    KUMO_UNLESS(!(in.version & ~(TlsVersionFlags | 0xff000000u)) && args.Pid().value != 0, InvalidArgument);
                    KUMO_UNLESS((in.version & supported) || in.version == SslVersion_Auto24, Unsupported);
                    if (g_context_count.fetch_add(1) >= MaxContexts) {
                        --g_context_count;
                        return OutOfMemory;
                    }
                    auto object = MakeObject<Context>(args.Pid().value, in.version);
                    if (!object) {
                        --g_context_count;
                        return OutOfMemory;
                    }
                    args.ReturnObject(std::move(object));
                    return 0;
                }>(0, WithPid),
            MakeCommand<SslService, NoData, u32, 0, 0,
                [](SslService &, NoData, u32 &out, auto &) -> Result { out = g_context_count; return 0; }>(1),
            MakeCommand<SslService, u32, NoData, 0, 0,
                [](SslService &self, u32 version, NoData &, auto &) -> Result {
                    KUMO_UNLESS(version >= 1 && version <= 5, InvalidArgument);
                    self.interface_version_ = version;
                    return 0;
                }>(5),
            MakeCommand<SslService, u32, u32, 1, 0,
                [](SslService &, u32 type, u32 &out, auto &) -> Result {
                    KUMO_UNLESS(type <= SslFlushSessionCacheOptionType_AllHosts, InvalidArgument);
                    out = 0;
                    return 0;
                }>(6, Mapped),
            MakeCommand<SslService, NoData, NoData, 0, 0,
                [](SslService &, NoData, NoData &, auto &) -> Result { return 0; }>(9),
        });
        if (commands.Dispatch(this, request, reply)) return true;
        if (request.input.header.command_id <= 11) {
            reply.result = Unsupported;
            return true;
        }
        return false;
    }
};

ObjectRef OpenSsl(void *) { return MakeObject<SslService>(); }
ObjectServer g_server({.sessions = 192, .domains = 128, .objects = 256, .converted_domains = 64});

} // namespace
} // namespace horizon::ssl

extern "C" {
u32 __nx_applet_type = AppletType_None;
void __libnx_initheap() {
    alignas(4096) static char heap[32 * 1024 * 1024];
    extern char *fake_heap_start, *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof(heap);
}
void __appInit() {
    SecmonArgs config{{0xC3000002, 65011}};
    svcCallSecureMonitor(&config);
    if (config.X[0] == 0 && config.X[1]) hosversionSet(BIT(31) | static_cast<u32>(config.X[1] >> 8));
    if (R_FAILED(smInitialize()) || R_FAILED(fsInitialize())) svcExitProcess();
}
void __appExit() { fsExit(); smExit(); }
void __libnx_exit(int) { svcExitProcess(); }
}

int main() {
    using namespace horizon::ssl;
    Result rc = InitializeTls();
    if (R_SUCCEEDED(rc)) rc = g_server.Register({"ssl", 64, nullptr, OpenSsl});
    if (R_FAILED(rc)) {
        Log("initialization failed", rc);
        return 1;
    }
    Log("serving ssl");
    rc = g_server.RunConcurrent({.count = 8, .stack_size = 128 * 1024, .priority = 48});
    Log("server stopped", rc);
    return 1;
}
