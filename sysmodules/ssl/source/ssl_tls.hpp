#pragma once
#include <kumo/prelude.hpp>
#include <mbedtls/ssl.h>

namespace horizon::ssl {

constexpr Result SslResult(u32 description) { return MAKERESULT(123, description); }
constexpr Result Unsupported = SslResult(13);
constexpr Result InvalidArgument = SslResult(101);
constexpr Result OutOfMemory = SslResult(102);
constexpr Result InvalidSocket = SslResult(116);
constexpr Result WouldBlock = SslResult(204);
constexpr Result Timeout = SslResult(205);
constexpr Result NoConnection = SslResult(208);

struct Certificates {
    mbedtls_x509_crt chain{};
    Certificates() { mbedtls_x509_crt_init(&chain); }
    ~Certificates() { mbedtls_x509_crt_free(&chain); }
    Certificates(const Certificates &) = delete;
    Certificates &operator=(const Certificates &) = delete;

    int Parse(std::span<const u8> bytes, bool pem);
    int Append(const Certificates &source);
};

Result InitializeTls();
Result ReadCertificateTime();
Certificates &DefaultTrust();
Result DuplicateSocket(s32 &out, s32 fd, u64 source_pid);
int Send(void *context, const unsigned char *data, size_t size);
int Receive(void *context, unsigned char *data, size_t size);
Result WaitSocket(s32 fd, short events, int timeout, short &ready);
Result CertificateError(u32 flags);

} // namespace horizon::ssl
