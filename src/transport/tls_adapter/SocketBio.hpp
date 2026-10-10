// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <memory>
#include <new>
#include <openssl/ssl.h>
#include <sys/socket.h>

namespace aa::transport::tls_adapter {

// SSL owns the BIO, but never the fd. Every SSL-generated write (including
// handshake/alerts) is nonblocking and cannot generate SIGPIPE.
class SocketBio final {
public:
    [[nodiscard]] static BIO* create(int fd) {
        static const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
            make_method(), BIO_meth_free);
        if (!method) {
            return nullptr;
        }
        BIO* bio = BIO_new(method.get());
        if (bio == nullptr) {
            return nullptr;
        }
        auto* socket = new (std::nothrow) int(fd);
        if (socket == nullptr) {
            BIO_free(bio);
            return nullptr;
        }
        BIO_set_data(bio, socket);
        BIO_set_init(bio, 1);
        return bio;
    }

private:
    static int read(BIO* bio, char* data, int size) {
        BIO_clear_retry_flags(bio);
        const auto count = ::recv(*static_cast<int*>(BIO_get_data(bio)), data,
                                  static_cast<std::size_t>(size), MSG_DONTWAIT);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            BIO_set_retry_read(bio);
        }
        return static_cast<int>(count);
    }

    static int write(BIO* bio, const char* data, int size) {
        BIO_clear_retry_flags(bio);
        const auto count = ::send(*static_cast<int*>(BIO_get_data(bio)), data,
                                  static_cast<std::size_t>(size), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            BIO_set_retry_write(bio);
        }
        return static_cast<int>(count);
    }

    static int initialize(BIO* bio) {
        BIO_set_init(bio, 0);
        BIO_set_data(bio, nullptr);
        return 1;
    }

    static int destroy(BIO* bio) {
        delete static_cast<int*>(BIO_get_data(bio));
        return 1;
    }

    static long control(BIO*, int command, long, void*) {
        return command == BIO_CTRL_FLUSH ? 1 : 0;
    }

    static BIO_METHOD* make_method() {
        BIO_METHOD* method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "nonblocking-nosignal-socket");
        if (method == nullptr) {
            return nullptr;
        }
        if (BIO_meth_set_create(method, initialize) != 1 ||
            BIO_meth_set_destroy(method, destroy) != 1 ||
            BIO_meth_set_read(method, read) != 1 ||
            BIO_meth_set_write(method, write) != 1 ||
            BIO_meth_set_ctrl(method, control) != 1) {
            BIO_meth_free(method);
            return nullptr;
        }
        return method;
    }
};

} // namespace aa::transport::tls_adapter
