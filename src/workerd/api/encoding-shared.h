// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Shared types used by encoding.h and encoding-legacy.h.
// Extracted to break circular dependencies between the two headers.

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

WD_STRONG_BOOL(DecoderFatal);
WD_STRONG_BOOL(DecoderIgnoreBom);

// The encodings listed here are defined as required by the Encoding spec.
// The first label is enum we use to identify the encoding in code, while
// the second label is the public identifier.
#define EW_ENCODINGS(V)                                                                            \
  V(Utf8, "utf-8")                                                                                 \
  V(Ibm866, "ibm866")                                                                              \
  V(Iso8859_2, "iso-8859-2")                                                                       \
  V(Iso8859_3, "iso-8859-3")                                                                       \
  V(Iso8859_4, "iso-8859-4")                                                                       \
  V(Iso8859_5, "iso-8859-5")                                                                       \
  V(Iso8859_6, "iso-8859-6")                                                                       \
  V(Iso8859_7, "iso-8859-7")                                                                       \
  V(Iso8859_8, "iso-8859-8")                                                                       \
  V(Iso8859_8i, "iso-8859-8-i")                                                                    \
  V(Iso8859_10, "iso-8859-10")                                                                     \
  V(Iso8859_13, "iso-8859-13")                                                                     \
  V(Iso8859_14, "iso-8859-14")                                                                     \
  V(Iso8859_15, "iso-8859-15")                                                                     \
  V(Iso8859_16, "iso-8859-16")                                                                     \
  V(Ko18_r, "koi8-r")                                                                              \
  V(Koi8_u, "koi8-u")                                                                              \
  V(Macintosh, "macintosh")                                                                        \
  V(Windows_874, "windows-874")                                                                    \
  V(Windows_1250, "windows-1250")                                                                  \
  V(Windows_1251, "windows-1251")                                                                  \
  V(Windows_1252, "windows-1252")                                                                  \
  V(Windows_1253, "windows-1253")                                                                  \
  V(Windows_1254, "windows-1254")                                                                  \
  V(Windows_1255, "windows-1255")                                                                  \
  V(Windows_1256, "windows-1256")                                                                  \
  V(Windows_1257, "windows-1257")                                                                  \
  V(Windows_1258, "windows-1258")                                                                  \
  V(X_Mac_Cyrillic, "x-mac-cyrillic")                                                              \
  V(Gbk, "gbk")                                                                                    \
  V(Gb18030, "gb18030")                                                                            \
  V(Big5, "big5")                                                                                  \
  V(Euc_Jp, "euc-jp")                                                                              \
  V(Iso2022_Jp, "iso-2022-jp")                                                                     \
  V(Shift_Jis, "shift_jis")                                                                        \
  V(Euc_Kr, "euc-kr")                                                                              \
  V(Replacement, "replacement")                                                                    \
  V(Utf16be, "utf-16be")                                                                           \
  V(Utf16le, "utf-16le")                                                                           \
  V(X_User_Defined, "x-user-defined")

enum class Encoding {
  INVALID,
#define V(name, _) name,
  EW_ENCODINGS(V)
#undef V
};

// A Decoder provides the underlying implementation of a TextDecoder.
class Decoder {
 public:
  virtual ~Decoder() noexcept(true) {}
  virtual Encoding getEncoding() = 0;
  virtual kj::Maybe<jsg::JsString> decode(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush = false) = 0;

  virtual void reset() {}
};

}  // namespace workerd::api
