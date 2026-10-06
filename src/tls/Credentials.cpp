#include <aa/tls/Credentials.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cctype>
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
struct CloseFile {
    void operator()(FILE* file) const noexcept { std::fclose(file); }
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
    std::unique_ptr<FILE, CloseFile> file(::fdopen(descriptor.value, "r"));
    if (!file) { throw Error(Failure::unreadable); }
    descriptor.value = -1;
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_fp(file.get(), BIO_NOCLOSE), &BIO_free);
    if (!bio) { throw Error(Failure::configuration); }
    Certificate certificate(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), &X509_free);
    PrivateKey key(PEM_read_bio_PrivateKey(bio.get(), nullptr, reject_password, nullptr), &EVP_PKEY_free);
    if (!certificate || !key) { throw Error(Failure::malformed); }
    if (X509_check_private_key(certificate.get(), key.get()) != 1) { throw Error(Failure::mismatch); }
    char remainder[256];
    int count;
    while ((count = BIO_read(bio.get(), remainder, sizeof(remainder))) > 0) {
        for (int index = 0; index < count; ++index) {
            if (!std::isspace(static_cast<unsigned char>(remainder[index]))) {
                throw Error(Failure::malformed);
            }
        }
    }
    if (std::ferror(file.get()) != 0) { throw Error(Failure::unreadable); }
    return Credentials{std::move(certificate), std::move(key)};
}
}
