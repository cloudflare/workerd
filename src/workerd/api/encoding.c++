// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "simdutf.h"
#include "util.h"

#include <workerd/jsg/jsg.h>
#include <workerd/util/strings.h>

#include <unicode/ucnv.h>
#include <unicode/utf8.h>
#include <v8.h>

#include <kj/string.h>

#include <algorithm>

namespace workerd::api {

// =======================================================================================
// TextDecoder implementation

namespace {
#define EW_ENCODING_LABELS(V)                                                                      \
  V("unicode-1-1-utf-8", Utf8)                                                                     \
  V("unicode11utf8", Utf8)                                                                         \
  V("unicode20utf8", Utf8)                                                                         \
  V("utf-8", Utf8)                                                                                 \
  V("utf8", Utf8)                                                                                  \
  V("x-unicode20utf8", Utf8)                                                                       \
  V("866", Ibm866)                                                                                 \
  V("cp866", Ibm866)                                                                               \
  V("csibm866", Ibm866)                                                                            \
  V("ibm866", Ibm866)                                                                              \
  V("csisolatin2", Iso8859_2)                                                                      \
  V("iso-8859-2", Iso8859_2)                                                                       \
  V("iso-ir-101", Iso8859_2)                                                                       \
  V("iso8859-2", Iso8859_2)                                                                        \
  V("iso88592", Iso8859_2)                                                                         \
  V("iso_8859-2", Iso8859_2)                                                                       \
  V("iso_8859-2:1987", Iso8859_2)                                                                  \
  V("l2", Iso8859_2)                                                                               \
  V("latin2", Iso8859_2)                                                                           \
  V("csisolatin3", Iso8859_3)                                                                      \
  V("iso-8859-3", Iso8859_3)                                                                       \
  V("iso-ir-109", Iso8859_3)                                                                       \
  V("iso8859-3", Iso8859_3)                                                                        \
  V("iso88593", Iso8859_3)                                                                         \
  V("iso_8859-3", Iso8859_3)                                                                       \
  V("iso_8859-3:1988", Iso8859_3)                                                                  \
  V("l3", Iso8859_3)                                                                               \
  V("latin3", Iso8859_3)                                                                           \
  V("csisolatin4", Iso8859_4)                                                                      \
  V("iso-8859-4", Iso8859_4)                                                                       \
  V("iso-ir-110", Iso8859_4)                                                                       \
  V("iso8859-4", Iso8859_4)                                                                        \
  V("iso88594", Iso8859_4)                                                                         \
  V("iso_8859-4", Iso8859_4)                                                                       \
  V("iso_8859-4:1988", Iso8859_4)                                                                  \
  V("l4", Iso8859_4)                                                                               \
  V("latin4", Iso8859_4)                                                                           \
  V("csisolatincyrillic", Iso8859_5)                                                               \
  V("cyrillic", Iso8859_5)                                                                         \
  V("iso-8859-5", Iso8859_5)                                                                       \
  V("iso-ir-144", Iso8859_5)                                                                       \
  V("iso8859-5", Iso8859_5)                                                                        \
  V("iso88595", Iso8859_5)                                                                         \
  V("iso_8859-5", Iso8859_5)                                                                       \
  V("iso_8859-5:1988", Iso8859_5)                                                                  \
  V("arabic", Iso8859_6)                                                                           \
  V("asmo-708", Iso8859_6)                                                                         \
  V("csiso88596e", Iso8859_6)                                                                      \
  V("csiso88596i", Iso8859_6)                                                                      \
  V("csisolatinarabic", Iso8859_6)                                                                 \
  V("ecma-114", Iso8859_6)                                                                         \
  V("iso-8859-6", Iso8859_6)                                                                       \
  V("iso-8859-6-e", Iso8859_6)                                                                     \
  V("iso-8859-6-i", Iso8859_6)                                                                     \
  V("iso-ir-127", Iso8859_6)                                                                       \
  V("iso8859-6", Iso8859_6)                                                                        \
  V("iso88596", Iso8859_6)                                                                         \
  V("iso_8859-6", Iso8859_6)                                                                       \
  V("iso_8859-6:1987", Iso8859_6)                                                                  \
  V("csisolatingreek", Iso8859_7)                                                                  \
  V("ecma-118", Iso8859_7)                                                                         \
  V("elot_928", Iso8859_7)                                                                         \
  V("greek", Iso8859_7)                                                                            \
  V("greek8", Iso8859_7)                                                                           \
  V("iso-8859-7", Iso8859_7)                                                                       \
  V("iso-ir-126", Iso8859_7)                                                                       \
  V("iso8859-7", Iso8859_7)                                                                        \
  V("iso88597", Iso8859_7)                                                                         \
  V("iso_8859-7", Iso8859_7)                                                                       \
  V("iso_8859-7:1987", Iso8859_7)                                                                  \
  V("sun_eu_greek", Iso8859_7)                                                                     \
  V("csiso88598e", Iso8859_8)                                                                      \
  V("csisolatinhebrew", Iso8859_8)                                                                 \
  V("hebrew", Iso8859_8)                                                                           \
  V("iso-8859-8", Iso8859_8)                                                                       \
  V("iso-8859-8-e", Iso8859_8)                                                                     \
  V("iso-ir-138", Iso8859_8)                                                                       \
  V("iso8859-8", Iso8859_8)                                                                        \
  V("iso88598", Iso8859_8)                                                                         \
  V("iso_8859-8", Iso8859_8)                                                                       \
  V("iso_8859-8:1988", Iso8859_8)                                                                  \
  V("visual", Iso8859_8)                                                                           \
  V("csiso88598i", Iso8859_8i)                                                                     \
  V("iso-8859-8-i", Iso8859_8i)                                                                    \
  V("logical", Iso8859_8i)                                                                         \
  V("csisolatin6", Iso8859_10)                                                                     \
  V("iso-8859-10", Iso8859_10)                                                                     \
  V("iso-ir-157", Iso8859_10)                                                                      \
  V("iso8859-10", Iso8859_10)                                                                      \
  V("iso885910", Iso8859_10)                                                                       \
  V("l6", Iso8859_10)                                                                              \
  V("latin6", Iso8859_10)                                                                          \
  V("iso-8859-13", Iso8859_13)                                                                     \
  V("iso8859-13", Iso8859_13)                                                                      \
  V("iso885913", Iso8859_13)                                                                       \
  V("iso-8859-14", Iso8859_14)                                                                     \
  V("iso8859-14", Iso8859_14)                                                                      \
  V("iso885914", Iso8859_14)                                                                       \
  V("csisolatin9", Iso8859_15)                                                                     \
  V("iso-8859-15", Iso8859_15)                                                                     \
  V("iso8859-15", Iso8859_15)                                                                      \
  V("iso885915", Iso8859_15)                                                                       \
  V("iso_8859-15", Iso8859_15)                                                                     \
  V("l9", Iso8859_15)                                                                              \
  V("iso-8859-16", Iso8859_16)                                                                     \
  V("cskoi8r", Ko18_r)                                                                             \
  V("koi", Ko18_r)                                                                                 \
  V("koi8", Ko18_r)                                                                                \
  V("koi8-r", Ko18_r)                                                                              \
  V("koi8_r", Ko18_r)                                                                              \
  V("koi8-ru", Koi8_u)                                                                             \
  V("koi8-u", Koi8_u)                                                                              \
  V("csmacintosh", Macintosh)                                                                      \
  V("mac", Macintosh)                                                                              \
  V("macintosh", Macintosh)                                                                        \
  V("x-mac-roman", Macintosh)                                                                      \
  V("dos-874", Windows_874)                                                                        \
  V("iso-8859-11", Windows_874)                                                                    \
  V("iso8859-11", Windows_874)                                                                     \
  V("iso885911", Windows_874)                                                                      \
  V("tis-620", Windows_874)                                                                        \
  V("windows-874", Windows_874)                                                                    \
  V("cp1250", Windows_1250)                                                                        \
  V("windows-1250", Windows_1250)                                                                  \
  V("x-cp1250", Windows_1250)                                                                      \
  V("cp1251", Windows_1251)                                                                        \
  V("windows-1251", Windows_1251)                                                                  \
  V("x-cp1251", Windows_1251)                                                                      \
  V("ansi_x3.4-1968", Windows_1252)                                                                \
  V("ascii", Windows_1252)                                                                         \
  V("cp1252", Windows_1252)                                                                        \
  V("cp819", Windows_1252)                                                                         \
  V("csisolatin1", Windows_1252)                                                                   \
  V("ibm819", Windows_1252)                                                                        \
  V("iso-8859-1", Windows_1252)                                                                    \
  V("iso-ir-100", Windows_1252)                                                                    \
  V("iso8859-1", Windows_1252)                                                                     \
  V("iso88591", Windows_1252)                                                                      \
  V("iso_8859-1", Windows_1252)                                                                    \
  V("iso_8859-1:1987", Windows_1252)                                                               \
  V("l1", Windows_1252)                                                                            \
  V("latin1", Windows_1252)                                                                        \
  V("us-ascii", Windows_1252)                                                                      \
  V("windows-1252", Windows_1252)                                                                  \
  V("x-cp1252", Windows_1252)                                                                      \
  V("cp1253", Windows_1253)                                                                        \
  V("windows-1253", Windows_1253)                                                                  \
  V("x-cp1253", Windows_1253)                                                                      \
  V("cp1254", Windows_1254)                                                                        \
  V("csisolatin5", Windows_1254)                                                                   \
  V("iso-8859-9", Windows_1254)                                                                    \
  V("iso-ir-148", Windows_1254)                                                                    \
  V("iso8859-9", Windows_1254)                                                                     \
  V("iso88599", Windows_1254)                                                                      \
  V("iso_8859-9", Windows_1254)                                                                    \
  V("iso_8859-9:1989", Windows_1254)                                                               \
  V("l5", Windows_1254)                                                                            \
  V("latin5", Windows_1254)                                                                        \
  V("windows-1254", Windows_1254)                                                                  \
  V("x-cp1254", Windows_1254)                                                                      \
  V("cp1255", Windows_1255)                                                                        \
  V("windows-1255", Windows_1255)                                                                  \
  V("x-cp1255", Windows_1255)                                                                      \
  V("cp1256", Windows_1256)                                                                        \
  V("windows-1256", Windows_1256)                                                                  \
  V("x-cp1256", Windows_1256)                                                                      \
  V("cp1257", Windows_1257)                                                                        \
  V("windows-1257", Windows_1257)                                                                  \
  V("x-cp1257", Windows_1257)                                                                      \
  V("cp1258", Windows_1258)                                                                        \
  V("windows-1258", Windows_1258)                                                                  \
  V("x-cp1258", Windows_1258)                                                                      \
  V("x-mac-cyrillic", X_Mac_Cyrillic)                                                              \
  V("x-mac-ukrainian", X_Mac_Cyrillic)                                                             \
  V("chinese", Gbk)                                                                                \
  V("csgb2312", Gbk)                                                                               \
  V("csiso58gb231280", Gbk)                                                                        \
  V("gb2312", Gbk)                                                                                 \
  V("gb_2312", Gbk)                                                                                \
  V("gb_2312-80", Gbk)                                                                             \
  V("gbk", Gbk)                                                                                    \
  V("iso-ir-58", Gbk)                                                                              \
  V("x-gbk", Gbk)                                                                                  \
  V("gb18030", Gb18030)                                                                            \
  V("big5", Big5)                                                                                  \
  V("big5-hkscs", Big5)                                                                            \
  V("cn-big5", Big5)                                                                               \
  V("csbig5", Big5)                                                                                \
  V("x-x-big5", Big5)                                                                              \
  V("cseucpkdfmtjapanese", Euc_Jp)                                                                 \
  V("euc-jp", Euc_Jp)                                                                              \
  V("x-euc-jp", Euc_Jp)                                                                            \
  V("csiso2022jp", Iso2022_Jp)                                                                     \
  V("iso-2022-jp", Iso2022_Jp)                                                                     \
  V("csshiftjis", Shift_Jis)                                                                       \
  V("ms932", Shift_Jis)                                                                            \
  V("ms_kanji", Shift_Jis)                                                                         \
  V("shift-jis", Shift_Jis)                                                                        \
  V("shift_jis", Shift_Jis)                                                                        \
  V("sjis", Shift_Jis)                                                                             \
  V("windows-31j", Shift_Jis)                                                                      \
  V("x-sjis", Shift_Jis)                                                                           \
  V("cseuckr", Euc_Kr)                                                                             \
  V("csksc56011987", Euc_Kr)                                                                       \
  V("euc-kr", Euc_Kr)                                                                              \
  V("iso-ir-149", Euc_Kr)                                                                          \
  V("korean", Euc_Kr)                                                                              \
  V("ks_c_5601-1987", Euc_Kr)                                                                      \
  V("ks_c_5601-1989", Euc_Kr)                                                                      \
  V("ksc5601", Euc_Kr)                                                                             \
  V("ksc_5601", Euc_Kr)                                                                            \
  V("windows-949", Euc_Kr)                                                                         \
  V("csiso2022kr", Replacement)                                                                    \
  V("hz-gb-2312", Replacement)                                                                     \
  V("iso-2022-cn", Replacement)                                                                    \
  V("iso-2022-cn-ext", Replacement)                                                                \
  V("iso-2022-kr", Replacement)                                                                    \
  V("replacement", Replacement)                                                                    \
  V("unicodefffe", Utf16be)                                                                        \
  V("utf-16be", Utf16be)                                                                           \
  V("csunicode", Utf16le)                                                                          \
  V("iso-10646-ucs-2", Utf16le)                                                                    \
  V("ucs-2", Utf16le)                                                                              \
  V("unicode", Utf16le)                                                                            \
  V("unicodefeff", Utf16le)                                                                        \
  V("utf-16", Utf16le)                                                                             \
  V("utf-16le", Utf16le)                                                                           \
  V("x-user-defined", X_User_Defined)

kj::StringPtr getEncodingId(Encoding encoding) {
  switch (encoding) {
    case Encoding::INVALID:
      return "invalid"_kj;
#define V(name, id)                                                                                \
  case Encoding::name:                                                                             \
    return id##_kj;
      EW_ENCODINGS(V)
#undef V
  }
  KJ_UNREACHABLE;
}

Encoding getEncodingForLabel(kj::StringPtr label) {
  auto lower = toLower(label);
  auto trimmed = trimLeadingAndTrailingWhitespace(lower);
#define V(label, key)                                                                              \
  if (trimmed == label##_kjb) return Encoding::key;
  EW_ENCODING_LABELS(V)
#undef V
  return Encoding::INVALID;
}
}  // namespace

const kj::Array<const kj::byte> TextDecoder::EMPTY =
    kj::Array<const kj::byte>(&DUMMY, 0, kj::NullArrayDisposer::instance);
const TextDecoder::DecodeOptions TextDecoder::DEFAULT_OPTIONS = TextDecoder::DecodeOptions();

kj::Maybe<IcuDecoder> IcuDecoder::create(Encoding encoding, bool fatal, bool ignoreBom) {
  UErrorCode status = U_ZERO_ERROR;
  UConverter* inner = ucnv_open(getEncodingId(encoding).cStr(), &status);
  JSG_REQUIRE(U_SUCCESS(status), RangeError, "Invalid or unsupported encoding");

  if (fatal) {
    status = U_ZERO_ERROR;
    ucnv_setToUCallBack(inner, UCNV_TO_U_CALLBACK_STOP, nullptr, nullptr, nullptr, &status);
    if (U_FAILURE(status)) return kj::none;
  }

  return IcuDecoder(encoding, inner, ignoreBom);
}

kj::Maybe<jsg::JsString> IcuDecoder::decode(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush) {
  UErrorCode status = U_ZERO_ERROR;
  const auto maxCharSize = [this]() { return ucnv_getMaxCharSize(inner.get()); };

  const auto isUnicode = [this]() {
    switch (ucnv_getType(inner.get())) {
      case UCNV_UTF8:
      case UCNV_UTF16:
      case UCNV_UTF16_BigEndian:
      case UCNV_UTF16_LittleEndian:
        return true;
      default:
        return false;
    }
    KJ_UNREACHABLE;
  };

  const auto isUsAscii = [](const auto& b) { return b <= 0x7f; };

  KJ_DEFER({
    if (flush) reset();
  });

  // Evaluate fast-path options. These provide shortcuts for common cases with the caveat
  // that error handling for invalid sequences might be a bit different (because the
  // conversions are being handled by v8 directly rather than by the ICU converter).
  if (buffer.size() > 0 && ucnv_toUCountPending(inner.get(), &status) == 0) {
    KJ_ASSERT(U_SUCCESS(status));
    if (encoding == Encoding::Utf8 && std::all_of(buffer.begin(), buffer.end(), isUsAscii)) {
      // This is a fast-path option for UTF-8 that can be taken when there
      // are no buffered inputs and the non-empty input buffer contains only
      // codepoints <= 0x7f. This path is safe because with ASCII range codepoints
      // we know we won't accidentally split a multi-byte encoding. We also don't
      // have to worry about the BOM here since the BOM bytes are > 0x7f.
      // Note also that in this case we'll interpret as Latin1 since UTF-8 bytes
      // within this range are identical to Latin1 and v8 allocates these more
      // efficiently.
      return js.str(buffer);
    }

    if (encoding == Encoding::Utf16le && buffer.size() % sizeof(char16_t) == 0) {
      // This is a fast-path option for UTF-16le that can be taken when:
      // there are no buffered inputs, the non-empty input buffer length is an
      // even multiple of 2, and either flush is true or the last code unit
      // is not a Unicode lead surrogate. This is safe because when flush
      // is true the converter state will be cleared, and if the last code
      // unit is not a lead surrogate, we won't have to worry about possibly
      // splitting a valid surrogate pair.
      auto ptr = reinterpret_cast<const char16_t*>(buffer.begin());
      auto data = kj::ArrayPtr<const char16_t>(ptr, buffer.size() / 2);

      if (flush || !U_IS_SURROGATE_LEAD(data[data.size() - 1])) {
        bool omitInitialBom = false;
        if (!ignoreBom && !bomSeen) {
          omitInitialBom = data[0] == 0xfeff;
          bomSeen = true;
        }
        return js.str(data.slice(omitInitialBom ? 1 : 0, data.size()));
      }
    }
  }

  status = U_ZERO_ERROR;
  auto limit = 2 * maxCharSize() *
      (!flush ? buffer.size()
              : std::max(buffer.size(),
                    static_cast<size_t>(ucnv_toUCountPending(inner.get(), &status))));

  KJ_STACK_ARRAY(UChar, result, limit, 512, 4096);

  auto dest = result.begin();
  auto source = reinterpret_cast<const char*>(buffer.begin());

  ucnv_toUnicode(
      inner.get(), &dest, dest + limit, &source, source + buffer.size(), nullptr, flush, &status);

  if (U_FAILURE(status)) return kj::none;

  auto omitInitialBom = false;
  auto length = std::distance(result.begin(), dest);
  if (length > 0 && isUnicode() && !ignoreBom && !bomSeen) {
    omitInitialBom = result[0] == 0xfeff;
    bomSeen = true;
  }

  return js.str(result.slice(omitInitialBom ? 1 : 0, length));
}

kj::Maybe<jsg::JsString> AsciiDecoder::decode(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush) {
  return js.str(buffer);
}

void IcuDecoder::reset() {
  bomSeen = false;
  return ucnv_reset(inner.get());
}

Decoder& TextDecoder::getImpl() {
  KJ_SWITCH_ONEOF(decoder) {
    KJ_CASE_ONEOF(dec, AsciiDecoder) {
      return dec;
    }
    KJ_CASE_ONEOF(dec, IcuDecoder) {
      return dec;
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<TextDecoder> TextDecoder::constructor(jsg::Lock& js,
    jsg::Optional<kj::String> maybeLabel,
    jsg::Optional<ConstructorOptions> maybeOptions) {
  static constexpr ConstructorOptions DEFAULT_OPTIONS;
  auto options = maybeOptions.orDefault(DEFAULT_OPTIONS);
  auto encoding = Encoding::Utf8;

  const auto errorMessage = [](kj::StringPtr label) {
    return kj::str("\"", label, "\" is not a valid encoding.");
  };

  KJ_IF_SOME(label, maybeLabel) {
    encoding = getEncodingForLabel(label);
    JSG_REQUIRE(encoding != Encoding::Replacement && encoding != Encoding::X_User_Defined &&
            encoding != Encoding::INVALID,
        RangeError, errorMessage(label));
  }

  if (encoding == Encoding::Windows_1252) {
    return js.alloc<TextDecoder>(AsciiDecoder(), options);
  }

  return js.alloc<TextDecoder>(
      JSG_REQUIRE_NONNULL(IcuDecoder::create(encoding, options.fatal, options.ignoreBOM),
          RangeError, errorMessage(getEncodingId(encoding))),
      options);
}

kj::StringPtr TextDecoder::getEncoding() {
  return getEncodingId(getImpl().getEncoding());
}

jsg::JsString TextDecoder::decode(jsg::Lock& js,
    jsg::Optional<kj::Array<const kj::byte>> maybeInput,
    jsg::Optional<DecodeOptions> maybeOptions) {
  auto options = maybeOptions.orDefault(DEFAULT_OPTIONS);
  auto& input = maybeInput.orDefault(EMPTY);
  return JSG_REQUIRE_NONNULL(
      getImpl().decode(js, input, !options.stream), TypeError, "Failed to decode input.");
}

kj::Maybe<jsg::JsString> TextDecoder::decodePtr(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush) {
  KJ_SWITCH_ONEOF(decoder) {
    KJ_CASE_ONEOF(dec, AsciiDecoder) {
      return dec.decode(js, buffer, flush);
    }
    KJ_CASE_ONEOF(dec, IcuDecoder) {
      return dec.decode(js, buffer, flush);
    }
  }
  KJ_UNREACHABLE;
}

// =======================================================================================
// TextEncoder implementation

namespace {

constexpr inline bool isLeadSurrogate(char16_t c) {
  return 0xD800 <= c && c < 0xDC00;
}

constexpr inline bool isTrailSurrogate(char16_t c) {
  return 0xDC00 <= c && c <= 0xDFFF;
}

// Calculate the number of UTF-8 bytes needed for a single UTF-16 code unit
constexpr inline size_t utf8BytesForCodeUnit(char16_t c) {
  if (c < 0x80) return 1;
  if (c < 0x800) return 2;
  return 3;
}

// Calculate UTF-8 length from UTF-16 with potentially invalid surrogates.
// Invalid surrogates are counted as U+FFFD (3 bytes in UTF-8).
size_t utf8LengthFromInvalidUtf16(kj::ArrayPtr<const char16_t> input) {
  size_t utf8Length = 0;
  bool pendingSurrogate = false;

  for (size_t i = 0; i < input.size(); i++) {
    char16_t c = input[i];

    if (pendingSurrogate) {
      if (isTrailSurrogate(c)) {
        // Valid surrogate pair = 4 bytes in UTF-8
        utf8Length += 4;
        pendingSurrogate = false;
      } else {
        // Unpaired lead surrogate = U+FFFD (3 bytes)
        utf8Length += 3;
        if (!isLeadSurrogate(c)) {
          utf8Length += utf8BytesForCodeUnit(c);
          pendingSurrogate = false;
        }
      }
    } else if (isLeadSurrogate(c)) {
      pendingSurrogate = true;
    } else {
      if (isTrailSurrogate(c)) {
        // Unpaired trail surrogate = U+FFFD (3 bytes)
        utf8Length += 3;
      } else {
        utf8Length += utf8BytesForCodeUnit(c);
      }
    }
  }

  if (pendingSurrogate) {
    utf8Length += 3;  // Trailing unpaired lead surrogate
  }

  return utf8Length;
}

// Encode a single UTF-16 code unit to UTF-8
inline size_t encodeUtf8CodeUnit(char16_t c, kj::ArrayPtr<char> out) {
  if (c < 0x80) {
    out[0] = static_cast<char>(c);
    return 1;
  } else if (c < 0x800) {
    out[0] = static_cast<char>(0xC0 | (c >> 6));
    out[1] = static_cast<char>(0x80 | (c & 0x3F));
    return 2;
  } else {
    out[0] = static_cast<char>(0xE0 | (c >> 12));
    out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (c & 0x3F));
    return 3;
  }
}

// Encode a valid surrogate pair to UTF-8
inline void encodeSurrogatePair(char16_t lead, char16_t trail, kj::ArrayPtr<char> out) {
  uint32_t codepoint = 0x10000 + (((lead & 0x3FF) << 10) | (trail & 0x3FF));
  out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
  out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
}

// Convert UTF-16 with potentially invalid surrogates to UTF-8.
// Invalid surrogates are replaced with U+FFFD.
// Returns the number of UTF-8 bytes written.
size_t convertInvalidUtf16ToUtf8(kj::ArrayPtr<const char16_t> input, kj::ArrayPtr<char> out) {
  size_t position = 0;
  bool pendingSurrogate = false;

  for (size_t i = 0; i < input.size(); i++) {
    char16_t c = input[i];

    if (pendingSurrogate) {
      if (isTrailSurrogate(c)) {
        encodeSurrogatePair(input[i - 1], c, out.slice(position, out.size()));
        position += 4;
        pendingSurrogate = false;
      } else {
        position += encodeUtf8CodeUnit(0xFFFD, out.slice(position, out.size()));
        if (!isLeadSurrogate(c)) {
          position += encodeUtf8CodeUnit(c, out.slice(position, out.size()));
          pendingSurrogate = false;
        }
      }
    } else if (isLeadSurrogate(c)) {
      pendingSurrogate = true;
    } else {
      if (isTrailSurrogate(c)) {
        position += encodeUtf8CodeUnit(0xFFFD, out.slice(position, out.size()));
      } else {
        position += encodeUtf8CodeUnit(c, out.slice(position, out.size()));
      }
    }
  }

  if (pendingSurrogate) {
    position += encodeUtf8CodeUnit(0xFFFD, out.slice(position, out.size()));
  }

  return position;
}

}  // namespace

jsg::Ref<TextEncoder> TextEncoder::constructor(jsg::Lock& js) {
  return js.alloc<TextEncoder>();
}

jsg::JsUint8Array TextEncoder::encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input) {
  jsg::JsString str = input.orDefault(js.str());
  std::shared_ptr<v8::BackingStore> backingStore;
  size_t utf8_length = 0;

  // Fast path: check if string is one-byte before creating ValueView
  if (str.isOneByte(js)) {
    auto length = str.length(js);
    // Allocate buffer for Latin-1. Use v8::ArrayBuffer::NewBackingStore to avoid creating
    // JS objects during conversion.
    backingStore = js.allocBackingStore(length, jsg::Lock::AllocOption::UNINITIALIZED);
    auto backingData = reinterpret_cast<kj::byte*>(backingStore->Data());

    [[maybe_unused]] auto writeResult = str.writeInto(js, kj::arrayPtr(backingData, length));
    KJ_DASSERT(
        writeResult.written == length, "writeInto must completely overwrite the backing buffer");

    utf8_length =
        simdutf::utf8_length_from_latin1(reinterpret_cast<const char*>(backingData), length);

    if (utf8_length == length) {
      // ASCII fast path: no conversion needed, Latin-1 is same as UTF-8 for ASCII
      auto array = v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, backingStore), 0, length);
      return jsg::JsUint8Array(array);
    }

    KJ_DASSERT(utf8_length > length);

    // Need to convert Latin-1 to UTF-8
    std::shared_ptr<v8::BackingStore> backingStore2 =
        js.allocBackingStore(utf8_length, jsg::Lock::AllocOption::UNINITIALIZED);
    [[maybe_unused]] auto written =
        simdutf::convert_latin1_to_utf8(reinterpret_cast<const char*>(backingData), length,
            reinterpret_cast<char*>(backingStore2->Data()));
    KJ_DASSERT(utf8_length == written);
    auto array =
        v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, backingStore2), 0, utf8_length);
    return jsg::JsUint8Array(array);
  }

  // Two-byte string path
  {
    // Note that ValueView flattens the string, if it's not already flattened
    v8::String::ValueView view(js.v8Isolate, str);
    // Two-byte string path. V8 uses UTF-16LE encoding internally for strings with code points
    // > U+00FF. Check if the UTF-16 is valid (no unpaired surrogates) to determine the path.
    auto data = reinterpret_cast<const char16_t*>(view.data16());

    if (simdutf::validate_utf16le(data, view.length())) {
      // Common case: valid UTF-16, convert directly to UTF-8
      utf8_length = simdutf::utf8_length_from_utf16le(data, view.length());
      backingStore = js.allocBackingStore(utf8_length, jsg::Lock::AllocOption::UNINITIALIZED);
      [[maybe_unused]] auto written = simdutf::convert_utf16le_to_utf8(
          data, view.length(), reinterpret_cast<char*>(backingStore->Data()));
      KJ_DASSERT(written == utf8_length);
    } else {
      // Invalid UTF-16 with unpaired surrogates. Per the Encoding Standard,
      // unpaired surrogates must be replaced with U+FFFD (replacement character).
      // Use custom conversion that handles invalid surrogates without creating an
      // intermediate well-formed UTF-16 buffer.
      auto inputArray = kj::ArrayPtr<const char16_t>(data, view.length());
      utf8_length = utf8LengthFromInvalidUtf16(inputArray);
      backingStore = js.allocBackingStore(utf8_length, jsg::Lock::AllocOption::UNINITIALIZED);
      auto outputArray =
          kj::ArrayPtr<char>(reinterpret_cast<char*>(backingStore->Data()), utf8_length);
      convertInvalidUtf16ToUtf8(inputArray, outputArray);
    }
  }  // ValueView destroyed here, releasing the heap lock

  // Now that ValueView is destroyed and the heap lock is released, it's safe to create V8 objects.
  // Create the Uint8Array from the raw v8::BackingStore.
  auto array =
      v8::Uint8Array::New(v8::ArrayBuffer::New(js.v8Isolate, backingStore), 0, utf8_length);
  return jsg::JsUint8Array(array);
}

namespace {

// Binary search to find how many Latin-1 characters fit when converted to UTF-8.
// Latin-1 bytes 0x00-0x7F encode as 1 UTF-8 byte, 0x80-0xFF encode as 2 UTF-8 bytes.
size_t findBestFitLatin1(const char* data, size_t length, size_t bufferSize) {
  size_t left = 0;
  size_t right = length;
  size_t bestFit = 0;

  while (left <= right) {
    size_t mid = left + (right - left) / 2;
    if (mid == 0) break;

    size_t midUtf8Length = simdutf::utf8_length_from_latin1(data, mid);
    if (midUtf8Length <= bufferSize) {
      bestFit = mid;
      left = mid + 1;
    } else {
      right = mid - 1;
    }
  }

  return bestFit;
}

// Binary search to find how many UTF-16 code units fit when converted to UTF-8.
// Ensures surrogate pairs (0xD800-0xDFFF) are never split across the boundary.
size_t findBestFitUtf16(const char16_t* data, size_t length, size_t bufferSize) {
  size_t left = 0;
  size_t right = length;
  size_t bestFit = 0;

  while (left <= right) {
    size_t mid = left + (right - left) / 2;
    if (mid == 0) break;

    // Don't split surrogate pairs - adjust backwards if mid lands after a high surrogate
    size_t adjustedMid = mid;
    if (adjustedMid > 0 && adjustedMid < length) {
      char16_t prev = data[adjustedMid - 1];
      if (prev >= 0xD800 && prev < 0xDC00) {
        adjustedMid--;
      }
    }

    if (adjustedMid == 0) {
      right = 0;
      break;
    }

    size_t midUtf8Length = simdutf::utf8_length_from_utf16(data, adjustedMid);
    if (midUtf8Length <= bufferSize) {
      bestFit = adjustedMid;
      left = adjustedMid + 1;
    } else {
      right = adjustedMid - 1;
    }
  }

  return bestFit;
}

// Binary search to find how many UTF-16 code units with invalid surrogates fit when converted to UTF-8.
// Ensures surrogate pairs are never split, and unpaired surrogates are replaced with U+FFFD.
size_t findBestFitInvalidUtf16(const char16_t* data, size_t length, size_t bufferSize) {
  size_t left = 0;
  size_t right = length;
  size_t bestFit = 0;

  while (left <= right) {
    size_t mid = left + (right - left) / 2;
    if (mid == 0) break;

    // Don't split surrogate pairs - adjust backwards if mid lands after a high surrogate
    size_t adjustedMid = mid;
    if (adjustedMid > 0 && adjustedMid < length) {
      char16_t prev = data[adjustedMid - 1];
      if (prev >= 0xD800 && prev < 0xDC00) {
        adjustedMid--;
      }
    }

    if (adjustedMid == 0) {
      right = 0;
      break;
    }

    size_t midUtf8Length = utf8LengthFromInvalidUtf16(kj::arrayPtr(data, adjustedMid));
    if (midUtf8Length <= bufferSize) {
      bestFit = adjustedMid;
      left = adjustedMid + 1;
    } else {
      right = adjustedMid - 1;
    }
  }

  return bestFit;
}

}  // namespace

TextEncoder::EncodeIntoResult TextEncoder::encodeInto(
    jsg::Lock& js, jsg::JsString input, jsg::JsUint8Array buffer) {
  auto outputBuf = buffer.asArrayPtr<char>();
  size_t bufferSize = outputBuf.size();

  v8::String::ValueView view(js.v8Isolate, input);
  uint32_t length = view.length();

  if (view.is_one_byte()) {
    // Latin-1 path: characters 0x00-0x7F encode as 1 UTF-8 byte, 0x80-0xFF as 2 bytes
    auto data = reinterpret_cast<const char*>(view.data8());

    // Fast path: avoid length calculation when we can prove the string fits.
    // Check worst-case (2x), ASCII (1:1), or calculate exact length as fallback.
    size_t read = length;
    if (!(length * 2 <= bufferSize ||
            (length <= bufferSize && simdutf::validate_ascii(data, length)) ||
            simdutf::utf8_length_from_latin1(data, length) <= bufferSize)) {
      // Binary search to find how many characters fit
      read = findBestFitLatin1(data, length, bufferSize);
    }

    size_t written = simdutf::convert_latin1_to_utf8(data, read, outputBuf.begin());
    return TextEncoder::EncodeIntoResult{
      .read = static_cast<int>(read),
      .written = static_cast<int>(written),
    };
  }

  // UTF-16 path: validate to ensure spec compliance (replace invalid surrogates with U+FFFD)
  auto data = reinterpret_cast<const char16_t*>(view.data16());

  if (simdutf::validate_utf16(data, length)) {
    // Valid UTF-16: use fast SIMD conversion
    if (simdutf::utf8_length_from_utf16(data, length) <= bufferSize) {
      size_t written = simdutf::convert_utf16_to_utf8(data, length, outputBuf.begin());
      return TextEncoder::EncodeIntoResult{
        .read = static_cast<int>(length),
        .written = static_cast<int>(written),
      };
    }

    size_t bestFit = findBestFitUtf16(data, length, bufferSize);
    size_t written = simdutf::convert_utf16_to_utf8(data, bestFit, outputBuf.begin());
    return TextEncoder::EncodeIntoResult{
      .read = static_cast<int>(bestFit),
      .written = static_cast<int>(written),
    };
  }

  // Invalid UTF-16: convert directly to UTF-8, replacing unpaired surrogates with U+FFFD
  if (utf8LengthFromInvalidUtf16(kj::arrayPtr(data, length)) <= bufferSize) {
    size_t written = convertInvalidUtf16ToUtf8(kj::arrayPtr(data, length), outputBuf);
    return TextEncoder::EncodeIntoResult{
      .read = static_cast<int>(length),
      .written = static_cast<int>(written),
    };
  }

  size_t bestFit = findBestFitInvalidUtf16(data, length, bufferSize);
  size_t written = convertInvalidUtf16ToUtf8(kj::arrayPtr(data, bestFit), outputBuf);
  return TextEncoder::EncodeIntoResult{
    .read = static_cast<int>(bestFit),
    .written = static_cast<int>(written),
  };
}

}  // namespace workerd::api
