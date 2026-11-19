//
//  encoding.h
//  mkxp-z
//
//  Created by ゾロアーク on 6/22/21.
//

#ifndef encoding_h
#define encoding_h

#include <string>
#include <string.h>

#include "util/encoding.h"
#include <iconv.h>
#include <uchardet.h>
#include <errno.h>

namespace Encoding {

static std::string getCharset(std::string &str) {
    uchardet_t ud = uchardet_new();
    uchardet_handle_data(ud, str.c_str(), str.length());
    uchardet_data_end(ud);
    
    std::string ret(uchardet_get_charset(ud));
    uchardet_delete(ud);
    
    if (ret.empty())
        throw Exception(Exception::MKXPError, "Could not detect string encoding", str.c_str());
    return ret;
}

static std::string convertString(std::string &str, const char *charset) {
    // Conversion doesn't need to happen if it's already UTF-8
    if (!strcmp(charset, "UTF-8") || !strcmp(charset, "ASCII")) {
        return std::string(str);
    }
    
    iconv_t cd = iconv_open("UTF-8", charset);
#ifdef __3DS__
    // mkxp-z-3ds: iconv_open fails for charset names libiconv doesn't know (uchardet 0.0.8 can
    // guess "MAC-CENTRALEUROPE"); iconv() on (iconv_t)-1 writes through it and crashes.
    if (cd == (iconv_t)-1)
        throw Exception(Exception::MKXPError, "Unsupported string encoding (Guessed encoding: %s)", charset);
#endif
    
    size_t inLen = str.size();
    size_t outLen = inLen * 4;
    std::string buf(outLen, '\0');
    char *inPtr = const_cast<char*>(str.c_str());
    char *outPtr = const_cast<char*>(buf.c_str());
    
    errno = 0;
    size_t result = iconv(cd, &inPtr, &inLen, &outPtr, &outLen);
    
    iconv_close(cd);
    
    if (result != (size_t)-1 && errno == 0)
    {
        buf.resize(buf.size()-outLen);
    }
    else {
        throw Exception(Exception::MKXPError, "Unable to convert string (Guessed encoding: %s)", charset);
    }
    
    return buf;
}

#ifdef __3DS__
// mkxp-z-3ds: true if str is well-formed UTF-8 (no overlong forms, surrogates or code points
// above U+10FFFF) without NUL bytes (UTF-16/32 text without a BOM still goes to uchardet).
static bool isUtf8(const std::string &str) {
    const unsigned char *p = (const unsigned char *)str.data(), *end = p + str.size();
    while (p < end) {
        unsigned char c = *p++;
        if (c == 0) return false;
        if (c < 0x80) continue;
        int n; unsigned int cp, min;
        if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (end - p < n) return false;
        for (int i = 0; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    }
    return true;
}
#endif

static std::string convertString(std::string &str) {
#ifdef __3DS__
    // mkxp-z-3ds: well-formed UTF-8 stays as is. uchardet guesses on mostly ASCII text with a
    // few UTF-8 characters are unreliable (Essentials' mkxp.json, "Pokémon": MAC-CENTRALEUROPE).
    if (isUtf8(str))
        return std::string(str);
#endif
    return convertString(str, getCharset(str).c_str());
}
}

#endif /* encoding_h */
