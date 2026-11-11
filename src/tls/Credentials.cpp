// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/tls/Credentials.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/pem.h>

namespace aa::tls {
const char* Error::what() const noexcept {
    switch (failure_) {
    case Failure::unset: return "tls-credential-unset: set HU_KEY_PATH to an owner-only PEM bundle";
    case Failure::unreadable: return "tls-credential-unreadable";
    case Failure::insecure: return "tls-credential-insecure";
    case Failure::malformed: return "tls-credential-malformed";
    case Failure::mismatch: return "tls-credential-mismatch";
    case Failure::unknown_phone: return "tls-phone-not-approved";
    case Failure::peer_rejected: return "tls-peer-verification-rejected";
    case Failure::inactive: return "tls-session-inactive";
    case Failure::configuration: return "tls-configuration-rejected";
    }
    return "tls-configuration-rejected";
}

namespace {
struct Descriptor {
    int value;
    ~Descriptor() { if (value >= 0) { ::close(value); } }
};
int reject_password(char*, int, int, void*) { return 0; }
struct ClearErrors {
    ~ClearErrors() { ERR_clear_error(); }
};
struct PemBuffer {
    // One fixed allocation avoids secret copies from string growth/reallocation.
    // The extra byte detects growth beyond the file-size limit during reading.
    std::unique_ptr<char[]> bytes{new char[65537]{}};
    ~PemBuffer() { OPENSSL_cleanse(bytes.get(), 65537); }
};
}

Credentials load_credentials() {
    const ClearErrors clear_errors;
    const char* path = std::getenv("HU_KEY_PATH");
    if (path == nullptr || *path == '\0') { throw Error(Failure::unset); }
    Descriptor descriptor{::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (descriptor.value < 0) {
        throw Error(errno == ELOOP ? Failure::insecure : Failure::unreadable);
    }
    struct stat metadata {};
    if (::fstat(descriptor.value, &metadata) != 0) { throw Error(Failure::unreadable); }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        (metadata.st_mode & 07777) != 0600 || metadata.st_nlink != 1) {
        throw Error(Failure::insecure);
    }
    if (metadata.st_size <= 0 || metadata.st_size > 65536) { throw Error(Failure::malformed); }
    PemBuffer pem;
    std::size_t size = 0;
    for (;;) {
        const ssize_t count = ::read(descriptor.value, pem.bytes.get() + size, 65537 - size);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw Error(Failure::unreadable);
        }
        if (count == 0) break;
        size += static_cast<std::size_t>(count);
        if (size > 65536) throw Error(Failure::malformed);
    }
    const std::string_view content(pem.bytes.get(), size);
    // Non-owning views keep private-key text in the one cleansing allocation.
    struct Block { std::string_view name; std::string_view text; };
    std::vector<Block> blocks;
    std::size_t position = 0;
    while (position < content.size()) {
        while (position < content.size() &&
               std::isspace(static_cast<unsigned char>(content[position]))) {
            ++position;
        }
        if (position >= content.size()) { break; }
        static const std::string kBegin = "-----BEGIN ";
        if (content.compare(position, kBegin.size(), kBegin) != 0) {
            throw Error(Failure::malformed);
        }
        const std::size_t name_start = position + kBegin.size();
        const std::size_t name_end = content.find("-----", name_start);
        if (name_end == std::string::npos || name_end == name_start) {
            throw Error(Failure::malformed);
        }
        const std::string_view name = content.substr(name_start, name_end - name_start);
        if (name != "CERTIFICATE" && name != "X509 CERTIFICATE" &&
            name != "PRIVATE KEY" && name != "RSA PRIVATE KEY" && name != "EC PRIVATE KEY") {
            throw Error(Failure::malformed);
        }
        std::size_t cursor = name_end + 5;
        if (cursor < content.size() && content[cursor] == '\r') { ++cursor; }
        if (cursor >= content.size() || content[cursor] != '\n') {
            throw Error(Failure::malformed);
        }
        const std::string end_tag = "-----END " + std::string(name) + "-----";
        const std::size_t end_line = content.find(end_tag, cursor);
        if (end_line == std::string::npos) { throw Error(Failure::malformed); }
        std::size_t after = end_line + end_tag.size();
        if (after < content.size() && content[after] == '\r') { ++after; }
        if (after < content.size() && content[after] == '\n') { ++after; }
        blocks.push_back({name, content.substr(position, after - position)});
        position = after;
    }
    if (blocks.size() < 2) { throw Error(Failure::malformed); }
    for (std::size_t index = 0; index + 1 < blocks.size(); ++index) {
        if (blocks[index].name != "CERTIFICATE" && blocks[index].name != "X509 CERTIFICATE") {
            throw Error(Failure::malformed);
        }
    }
    const std::string_view &key_name = blocks.back().name;
    if (key_name != "PRIVATE KEY" && key_name != "RSA PRIVATE KEY" && key_name != "EC PRIVATE KEY") {
        throw Error(Failure::malformed);
    }
    std::vector<Certificate> chain;
    Certificate certificate(nullptr, &X509_free);
    PrivateKey key(nullptr, &EVP_PKEY_free);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        std::unique_ptr<BIO, decltype(&BIO_free)> block_bio(
            BIO_new_mem_buf(blocks[index].text.data(), static_cast<int>(blocks[index].text.size())),
            &BIO_free);
        if (!block_bio) { throw Error(Failure::configuration); }
        if (index + 1 < blocks.size()) {
            Certificate parsed(PEM_read_bio_X509(block_bio.get(), nullptr, nullptr, nullptr), &X509_free);
            if (!parsed) { throw Error(Failure::malformed); }
            if (index == 0) { certificate = std::move(parsed); }
            else { chain.push_back(std::move(parsed)); }
        } else {
            key.reset(PEM_read_bio_PrivateKey(block_bio.get(), nullptr, reject_password, nullptr));
            if (!key) { throw Error(Failure::malformed); }
        }
    }
    if (X509_check_private_key(certificate.get(), key.get()) != 1) {
        throw Error(Failure::mismatch);
    }
    // Security bits compare different key families fairly: RSA-2048 and
    // P-256 both meet this floor; RSA-1024 does not.
    if (EVP_PKEY_security_bits(key.get()) < 112) { throw Error(Failure::insecure); }
    const auto check_certificate = [](X509* cert) {
        int digest = 0, signature_key = 0, security_bits = 0;
        std::uint32_t flags = 0;
        if (X509_get_signature_info(cert, &digest, &signature_key, &security_bits, &flags) != 1 ||
            security_bits < 112 || digest == NID_sha1 || digest == NID_md5) {
            throw Error(Failure::insecure);
        }
        const int expiry = X509_cmp_current_time(X509_get0_notAfter(cert));
        if (expiry == 0) { throw Error(Failure::malformed); }
        // Android Auto HU identity is not a Web PKI server identity. Rejecting
        // an expired compatibility credential can disable otherwise working
        // phones; emit a stable, redacted warning on every load instead.
        if (expiry < 0) std::fputs("tls-credential-warning: certificate-expired\n", stderr);
    };
    check_certificate(certificate.get());
    for (const auto& intermediate : chain) check_certificate(intermediate.get());
    return Credentials{std::move(certificate), std::move(key), std::move(chain)};
}
}
