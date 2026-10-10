// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/protocol/Messages.hpp>
#include <aap_protobuf/service/Service.pb.h>

namespace aa::protocol::aasdk_adapter {
[[nodiscard]] core::Result<ServiceConfiguration>
read_configuration(const aap_protobuf::service::Service& entry, ChannelRole role);
[[nodiscard]] core::Result<void>
put_configuration(aap_protobuf::service::Service& entry, const ServiceDescriptor& descriptor);
} // namespace aa::protocol::aasdk_adapter
