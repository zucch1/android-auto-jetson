// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/ipc/Contract.hpp>

#include <aa/ipc/dbus_contract.generated.hpp>

namespace aa::ipc {

std::string_view introspection_xml() noexcept {
    return dbus_contract::kIntrospectionXml;
}

std::string_view contract_semver() noexcept {
    return dbus_contract::kContractSemVer;
}

} // namespace aa::ipc
