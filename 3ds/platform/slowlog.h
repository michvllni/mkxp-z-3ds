// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: notes slow operations on stderr (sdmc:/rmxp3ds/mkxp-stderr.txt), to find what
// stalls frames on the console. A SlowLog in a scope writes one line when the scope took
// SLOW_LOG_MS or longer: "slow: <what> <name> <ms> ms".
#pragma once

#include <3ds.h>
#include <cstdio>
#include <string>

const u64 SLOW_LOG_MS = 100;

struct SlowLog {
	const char *what;
	std::string name;
	u64 t0;
	SlowLog(const char *what, const std::string &name) : what(what), name(name), t0(osGetTime()) {}
	~SlowLog() {
		u64 ms = osGetTime() - t0;
		if (ms >= SLOW_LOG_MS) fprintf(stderr, "slow: %s %s %llu ms\n", what, name.c_str(), (unsigned long long)ms);
	}
};
