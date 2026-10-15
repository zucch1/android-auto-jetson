// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/tls/Credentials.hpp>
#include <iostream>

int main() {
    try {
        const auto credentials = aa::tls::load_credentials();
        std::cout << "tls-credential-loaded";
        if (!credentials.chain.empty()) {
            std::cout << " chain=" << credentials.chain.size();
        }
        std::cout << '\n';
        return credentials.certificate && credentials.private_key ? 0 : 2;
    } catch (const aa::tls::Error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
