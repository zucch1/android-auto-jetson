// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/tls/Credentials.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
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
    std::string content;
    {
        char buffer[4096];
        for (;;) {
            const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file.get());
            if (count == 0) { break; }
            content.append(buffer, count);
            if (content.size() > 65536) { throw Error(Failure::malformed); }
        }
        if (std::ferror(file.get()) != 0) { throw Error(Failure::unreadable); }
    }
    struct Block { std::string name; std::string text; };
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
        const std::string name = content.substr(name_start, name_end - name_start);
        if (name.find('-') != std::string::npos) { throw Error(Failure::malformed); }
        std::size_t cursor = name_end + 5;
        if (cursor < content.size() && content[cursor] == '\r') { ++cursor; }
        if (cursor >= content.size() || content[cursor] != '\n') {
            throw Error(Failure::malformed);
        }
        const std::string end_tag = "-----END " + name + "-----";
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
    const std::string &key_name = blocks.back().name;
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
    return Credentials{std::move(certificate), std::move(key), std::move(chain)};
}
}
