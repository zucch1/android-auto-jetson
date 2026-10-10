// SPDX-License-Identifier: GPL-3.0-or-later
#include <aasdk/Messenger/Cryptor.hpp>
#include <aasdk/Transport/SSLWrapper.hpp>
#include <aap_protobuf/service/control/message/VersionResponseOptions.pb.h>

#include <concepts>
#include <iostream>
#include <memory>
#include <span>
#include <string>

static_assert(std::same_as<std::span<const char>::element_type, const char>);

int main() {
    // Given: real production objects; no handshake or credential-policy change.
    const auto wrapper = std::make_shared<aasdk::transport::SSLWrapper>();
    aasdk::messenger::Cryptor cryptor(wrapper);
    aap_protobuf::service::control::message::VersionResponseOptions response;
    response.mutable_connection_configuration()->mutable_ping_configuration()->set_timeout_ms(1234);
    // When: call out-of-line SDK methods and generated protobuf serialization.
    const bool active = cryptor.aasdk::messenger::Cryptor::isActive();
    const auto* method = wrapper->aasdk::transport::SSLWrapper::getMethod();
    const std::string bytes = response.SerializeAsString();
    aap_protobuf::service::control::message::VersionResponseOptions decoded;
    const bool parsed = decoded.ParseFromString(bytes);
    cryptor.deinit();
    // Then: a fresh cryptor is inactive, OpenSSL supplies a method, round-trip succeeds.
    if (active || method == nullptr || !parsed ||
        decoded.connection_configuration().ping_configuration().timeout_ms() != 1234) {
        return 1;
    }
    std::cout << "C++20 AASDK SSLWrapper/Cryptor and protobuf consumer passed\n";
    return 0;
}
