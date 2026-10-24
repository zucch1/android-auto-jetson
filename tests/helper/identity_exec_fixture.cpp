// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const char ready = 'r';
    if (::write(STDOUT_FILENO, &ready, 1) != 1) return 3;
    char trigger{};
    if (::read(STDIN_FILENO, &trigger, 1) != 1) return 4;
    switch (argv[3][0]) {
    case '0': ::execl(argv[2], argv[2], argv[1], argv[2], "1", static_cast<char*>(nullptr)); return 5;
    case '1': ::execl(argv[1], argv[1], argv[1], argv[2], "2", static_cast<char*>(nullptr)); return 6;
    case '2': return 0;
    default: return 7;
    }
}
