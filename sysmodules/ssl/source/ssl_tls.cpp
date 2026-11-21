#include "ssl_tls.hpp"
#include <kumo/mutex.hpp>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_time.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>
#include <poll.h>
#include <sys/socket.h>
#include <ctime>
extern "C" {
#include <switch/services/bsd.h>
}

namespace horizon::ssl {
namespace {

constexpr size_t MaxBundleSize = 1024 * 1024;
// Raw BSD IPC uses Linux errno values.
constexpr s32 BsdInterrupted = 4;
constexpr s32 BsdTryAgain = 11;
constexpr s32 BsdConnectionReset = 104;
Certificates g_default_trust;
kumo::Mutex g_rng_mutex;
kumo::Mutex g_time_mutex;
kumo::Mutex g_duplicate_mutex;
thread_local time_t g_certificate_time;

int InitMutex(Mutex *mutex) { mutexInit(mutex); return 0; }
void FreeMutex(Mutex *) {}
int LockMutex(Mutex *mutex) { mutexLock(mutex); return 0; }
int UnlockMutex(Mutex *mutex) { mutexUnlock(mutex); return 0; }
int InitCondition(CondVar *condition) { condvarInit(condition); return 0; }
void FreeCondition(CondVar *) {}
int SignalCondition(CondVar *condition) { return condvarWakeOne(condition); }
int BroadcastCondition(CondVar *condition) { return condvarWakeAll(condition); }
int WaitCondition(CondVar *condition, Mutex *mutex) { return condvarWait(condition, mutex); }

time_t CertificateTime(time_t *out) {
    if (out) *out = g_certificate_time;
    return g_certificate_time;
}

Result LoadDefaultTrust() {
    KUMO_TRY(romfsMountSelf("romfs"));
    FILE *file = std::fopen("romfs:/certs/ca-bundle.pem", "rb");
    Result rc = InvalidArgument;
    if (file) {
        if (std::fseek(file, 0, SEEK_END) == 0) {
            const long size = std::ftell(file);
            if (size > 0 && static_cast<unsigned long>(size) <= MaxBundleSize &&
                std::fseek(file, 0, SEEK_SET) == 0) {
                auto bytes = std::unique_ptr<u8[]>(new (std::nothrow) u8[size]);
                if (!bytes) {
                    rc = OutOfMemory;
                } else if (std::fread(bytes.get(), 1, size, file) == static_cast<size_t>(size)
                           && g_default_trust.Parse({bytes.get(), static_cast<size_t>(size)}, true) == 0) {
                    rc = 0;
                }
            }
        }
        std::fclose(file);
    }
    romfsUnmount("romfs");
    return rc;
}

} // namespace

int Certificates::Parse(std::span<const u8> bytes, bool pem) {
    if (bytes.empty()) return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    if (!pem) return mbedtls_x509_crt_parse_der(&chain, bytes.data(), bytes.size());
    auto text = std::unique_ptr<u8[]>(new (std::nothrow) u8[bytes.size() + 1]);
    if (!text) return MBEDTLS_ERR_X509_ALLOC_FAILED;
    std::memcpy(text.get(), bytes.data(), bytes.size());
    text[bytes.size()] = 0;
    return mbedtls_x509_crt_parse(&chain, text.get(), bytes.size() + 1);
}

int Certificates::Append(const Certificates &source) {
    for (auto *cert = &source.chain; cert && cert->raw.p; cert = cert->next) {
        const int rc = mbedtls_x509_crt_parse_der(&chain, cert->raw.p, cert->raw.len);
        if (rc) return rc;
    }
    return 0;
}

Certificates &DefaultTrust() { return g_default_trust; }

Result ReadCertificateTime() {
    std::scoped_lock lock(g_time_mutex);
    u64 seconds{};
    // The network clock starts unset. The user clock can initially hold a fixed placeholder.
    if (R_FAILED(timeGetCurrentTime(TimeType_NetworkSystemClock, &seconds)) ||
        seconds > static_cast<u64>(std::numeric_limits<time_t>::max())) {
        return SslResult(303);
    }
    g_certificate_time = static_cast<time_t>(seconds);
    return 0;
}

Result InitializeTls() {
    mbedtls_threading_set_alt(InitMutex, FreeMutex, LockMutex, UnlockMutex,
                              InitCondition, FreeCondition, SignalCondition, BroadcastCondition, WaitCondition);
    mbedtls_platform_set_time(CertificateTime);
    KUMO_TRY(csrngInitialize());
    KUMO_TRY(timeInitialize());
    KUMO_TRY(bsdInitialize(bsdGetDefaultInitConfig(), 8, BIT(1)));
    if (psa_crypto_init() != PSA_SUCCESS) return Unsupported;
    return LoadDefaultTrust();
}

Result DuplicateSocket(s32 &out, s32 fd, u64 source_pid) {
    const struct { s32 fd; u32 padding; u64 pid; } in{fd, 0, source_pid};
    struct { s32 fd; s32 error; } reply{};
    {
        std::scoped_lock lock(g_duplicate_mutex);
        KUMO_TRY(serviceDispatchInOut(bsdGetServiceSession(), 27, in, reply));
    }
    if (reply.fd < 0) return InvalidSocket;

    sockaddr_storage peer{};
    socklen_t size = sizeof(peer);
    int type = 0;
    socklen_t type_size = sizeof(type);
    if (bsdGetPeerName(reply.fd, reinterpret_cast<sockaddr *>(&peer), &size) < 0 ||
        bsdGetSockOpt(reply.fd, SOL_SOCKET, SO_TYPE, &type, &type_size) < 0 || type != SOCK_STREAM) {
        bsdClose(reply.fd);
        return InvalidSocket;
    }
    out = reply.fd;
    return 0;
}

int Send(void *context, const unsigned char *data, size_t size) {
    const auto result = bsdSend(*static_cast<s32 *>(context), data, size, MSG_DONTWAIT);
    if (result >= 0) return result;
    if (g_bsdErrno == BsdTryAgain || g_bsdErrno == BsdInterrupted) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return g_bsdErrno == BsdConnectionReset ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
}

int Receive(void *context, unsigned char *data, size_t size) {
    const auto result = bsdRecv(*static_cast<s32 *>(context), data, size, MSG_DONTWAIT);
    if (result >= 0) return result;
    if (g_bsdErrno == BsdTryAgain || g_bsdErrno == BsdInterrupted) return MBEDTLS_ERR_SSL_WANT_READ;
    return g_bsdErrno == BsdConnectionReset ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

Result WaitSocket(s32 fd, short events, int timeout, short &ready) {
    pollfd descriptor{fd, events, 0};
    if (bsdPoll(&descriptor, 1, timeout) < 0) return NoConnection;
    ready = descriptor.revents;
    if (ready & POLLNVAL) return InvalidSocket;
    return 0;
}

Result CertificateError(u32 flags) {
    if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) return SslResult(301);
    if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) return SslResult(303);
    if (flags & MBEDTLS_X509_BADCERT_REVOKED) return SslResult(304);
    if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) return SslResult(308);
    if (flags == MBEDTLS_X509_BADCERT_MISSING) return SslResult(302);
    return flags ? SslResult(307) : 0;
}

} // namespace horizon::ssl

extern "C" int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags, size_t *estimate_bits,
                                            unsigned char *output, size_t size) {
    *estimate_bits = 0;
    if (flags != PSA_DRIVER_GET_ENTROPY_FLAGS_NONE) return PSA_ERROR_NOT_SUPPORTED;
    std::scoped_lock lock(horizon::ssl::g_rng_mutex);
    if (size && R_FAILED(csrngGetRandomBytes(output, size))) return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *estimate_bits = size * 8;
    return 0;
}

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000;
}
