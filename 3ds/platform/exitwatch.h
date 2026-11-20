// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: shutdown steps and the exit watchdog (main3ds.cpp).
#pragma once

// Writes "exit: <step>" to stderr (sdmc:/rmxp3ds/mkxp-stderr.txt), flushed, and remembers the
// step for the watchdog.
void rmxp3dsExitStep(const char *step);

// The system asked the app to close (HOME Menu, power button): logs it and starts the watchdog,
// which ends the process if the shutdown has not finished EXIT_WATCHDOG_S seconds later, so
// "Closing software" never hangs. Safe to call more than once.
void rmxp3dsCloseRequested();
