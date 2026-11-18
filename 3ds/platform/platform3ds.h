// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: functions shared by the 3DS platform files (3ds/platform).
#ifndef PLATFORM3DS_H
#define PLATFORM3DS_H

#include <string>

struct RGSSThreadData;

namespace platform3ds {
// Text of the last message box (RGSS print/msgbox, uncaught Ruby errors); main() shows it on the
// error screen when the game ends. Thread-safe.
std::string lastMessage();
// Pauses the RGSS thread at its next Graphics.update while the HOME menu or sleep mode owns the
// console, and resumes it afterwards (APT hook, registered by EventThread::process).
void installAptHook(RGSSThreadData &rtData);
void removeAptHook();
} // namespace platform3ds

#endif
