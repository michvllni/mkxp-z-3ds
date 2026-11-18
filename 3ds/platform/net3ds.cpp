// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: mkxp_net for the Nintendo 3DS (replaces src/net/net.cpp, which needs cpp-httplib
// and POSIX networking the 3DS does not have). HTTPLite requests raise an error.
#include "net/net.h"

#include "util/exception.h"

namespace mkxp_net {

namespace {
[[noreturn]] void unavailable() {
	throw Exception(Exception::MKXPError, "HTTP is not available on the Nintendo 3DS");
}
} // namespace

HTTPResponse::HTTPResponse() : _status(0) {}
HTTPResponse::~HTTPResponse() {}
int HTTPResponse::status() { return _status; }
std::string &HTTPResponse::body() { return _body; }
StringMap &HTTPResponse::headers() { return _headers; }

HTTPRequest::HTTPRequest(const char *dest, bool follow_redirects)
    : destination(dest), follow_location(follow_redirects) {}
HTTPRequest::~HTTPRequest() {}
StringMap &HTTPRequest::headers() { return _headers; }
HTTPResponse HTTPRequest::get() { unavailable(); }
HTTPResponse HTTPRequest::post(StringMap &) { unavailable(); }
HTTPResponse HTTPRequest::post(const char *, const char *) { unavailable(); }

} // namespace mkxp_net
