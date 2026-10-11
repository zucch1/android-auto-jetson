# SPDX-License-Identifier: GPL-3.0-or-later
# Sentinel for the mandatory input-sensor memcheck qualification (gate r1).
# Registered as `input_sensors_memory` whenever valgrind is absent (or the
# configured AA_VALGRIND_EXECUTABLE path does not exist): CMake configure may
# succeed without the tool, but qualification must FAIL VISIBLY under the
# same test name - never skip, never warn-and-pass. A PR whose history
# contained a memcheck-proven use-after-free behind a green suite must not be
# able to merge on a silently unqualified run.
message(FATAL_ERROR
    "AA_INPUT_SENSORS_MEMCHECK_UNAVAILABLE: valgrind is required for input-sensor qualification (input_sensors_memory)")
