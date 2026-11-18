// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: filesystemImpl for the Nintendo 3DS (replaces src/filesystem/filesystemImpl.cpp).
// ghc::filesystem does not know the 3DS, and paths there carry a device prefix ("sdmc:/",
// "romfs:/"), so this implements the few helpers mkxp-z needs with POSIX calls from newlib.
#include "filesystemImpl.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "util/exception.h"

namespace {
// Length of a device prefix such as "sdmc:" (up to and including ':'), or 0.
size_t deviceLength(const std::string &p) {
	size_t colon = p.find(':');
	if (colon == std::string::npos) return 0;
	size_t slash = p.find('/');
	return (slash == std::string::npos || colon < slash) ? colon + 1 : 0;
}
} // namespace

bool filesystemImpl::fileExists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

std::string filesystemImpl::contentsOfFileAsString(const char *path) {
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) throw Exception(Exception::NoFileError, "Failed to read file at %s", path);
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool filesystemImpl::setCurrentDirectory(const char *path) { return chdir(path) == 0; }

std::string filesystemImpl::getCurrentDirectory() {
	char buf[1024];
	if (!getcwd(buf, sizeof buf)) throw Exception(Exception::MKXPError, "Failed to retrieve current path");
	return std::string(buf);
}

// Lexically normalizes path ('\\' -> '/', removes "." and resolves ".." without going above the
// root), keeping a device prefix. With absolute, a relative path is joined to the current
// directory first. preferred has no meaning here ('/' is the only separator).
std::string filesystemImpl::normalizePath(const char *path, bool, bool absolute) {
	std::string in(path);
	for (char &c : in)
		if (c == '\\') c = '/';
	size_t dev = deviceLength(in);
	bool rooted = dev > 0 || (!in.empty() && in[0] == '/');
	if (!rooted && absolute) {
		std::string cwd = getCurrentDirectory();
		if (!cwd.empty() && cwd.back() != '/') cwd += '/';
		in = cwd + in;
		dev = deviceLength(in);
		rooted = true;
	}
	std::string prefix = in.substr(0, dev);
	std::vector<std::string> parts;
	size_t i = dev;
	while (i <= in.size()) {
		size_t j = in.find('/', i);
		if (j == std::string::npos) j = in.size();
		std::string part = in.substr(i, j - i);
		if (part == "..") {
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else if (!rooted)
				parts.push_back(part);
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		i = j + 1;
	}
	std::string out = prefix + (rooted ? "/" : "");
	for (size_t k = 0; k < parts.size(); k++) {
		if (k) out += '/';
		out += parts[k];
	}
	if (out.empty()) out = ".";
	return out;
}

std::string filesystemImpl::getDefaultGameRoot() { return getCurrentDirectory(); }
