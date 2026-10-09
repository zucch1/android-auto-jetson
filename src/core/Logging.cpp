// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/core/Logging.hpp>

namespace aa::core {

void Logger::log(LogLevel level, std::string_view message,
                 std::vector<LogField> fields) const {
    if (static_cast<int>(level) < static_cast<int>(minimum_)) {
        return;
    }
    sink_->write(LogEvent{level, message, std::move(fields)});
}

} // namespace aa::core
