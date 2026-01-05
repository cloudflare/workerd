// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "simdutf.h"
#include "util.h"

#include <workerd/io/features.h>
#include <workerd/jsg/jsg.h>
#include <workerd/util/autogate.h>
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

constexpr int MAX_SIZE_FOR_STACK_ALLOC = 4096;

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

  return IcuDecoder(encoding, inner, fatal, ignoreBom);
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

        auto slice = data.slice(omitInitialBom ? 1 : 0, data.size());

        // If pedanticWpt flag is enabled, then we follow the spec and fix invalid
        // surrogates on the UTF-16 input.
        if (slice.size() == 0 || !FeatureFlags::get(js).getPedanticWpt()) {
          return js.str(slice);
        }

        if (simdutf::validate_utf16(slice.begin(), slice.size())) {
          return js.str(slice);
        }

        if (fatal) {
          // In fatal mode, return error for invalid surrogates
          return kj::none;
        }

        // In non-fatal mode, replace invalid surrogates with U+FFFD.
        // Output size equals input size because each invalid surrogate (1 code unit)
        // is replaced with U+FFFD (also 1 code unit).
        // Use stack allocation for small strings (up to 256 code units) to avoid
        // heap allocation overhead.
        kj::SmallArray<char16_t, 256> fixed(slice.size());
        simdutf::to_well_formed_utf16(slice.begin(), slice.size(), fixed.begin());
        return js.str(fixed.asPtr());
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

jsg::Ref<TextEncoder> TextEncoder::constructor(jsg::Lock& js) {
  return js.alloc<TextEncoder>();
}

jsg::JsUint8Array TextEncoder::encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input) {
  if (!workerd::util::Autogate::isEnabled(workerd::util::AutogateKey::ENABLE_FAST_TEXTENCODER)) {
    auto str = input.orDefault(js.str());
    auto view = JSG_REQUIRE_NONNULL(jsg::BufferSource::tryAlloc(js, str.utf8Length(js)), RangeError,
        "Cannot allocate space for TextEncoder.encode");
    [[maybe_unused]] auto result = str.writeInto(
        js, view.asArrayPtr().asChars(), jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
    KJ_DASSERT(result.written == view.size());
    return jsg::JsUint8Array(view.getHandle(js).As<v8::Uint8Array>());
  }

  jsg::JsString str = input.orDefault(js.str());

  size_t utf8_length = 0;
  auto length = str.length(js);

#ifdef KJ_DEBUG
  bool wasAlreadyFlat = str.isFlat();
  KJ_DEFER({ KJ_ASSERT(wasAlreadyFlat || !str.isFlat()); });
#endif

  // Note: writeInto() doesn't flatten the string - it calls writeTo() which chains through
  // Write2 -> WriteV2 -> WriteHelperV2 -> String::WriteToFlat.
  // This means we may read from multiple string segments, but that's fine for our use case.

  if (str.isOneByte(js)) {
    // Use off-heap allocation for intermediate Latin-1 buffer to avoid wasting V8 heap space
    // and potentially triggering GC. Stack allocation for small strings, heap for large.
    kj::SmallArray<kj::byte, MAX_SIZE_FOR_STACK_ALLOC> latin1Buffer(length);

    [[maybe_unused]] auto writeResult = str.writeInto(js, latin1Buffer.asPtr());
    KJ_DASSERT(
        writeResult.written == length, "writeInto must completely overwrite the backing buffer");

    utf8_length = simdutf::utf8_length_from_latin1(
        reinterpret_cast<const char*>(latin1Buffer.begin()), length);

    auto backingStore = js.allocBackingStore(utf8_length, jsg::Lock::AllocOption::UNINITIALIZED);
    if (utf8_length == length) {
      // ASCII fast path: no conversion needed, Latin-1 is same as UTF-8 for ASCII
      kj::arrayPtr(static_cast<kj::byte*>(backingStore->Data()), length).copyFrom(latin1Buffer);
    } else {
      [[maybe_unused]] auto written =
          simdutf::convert_latin1_to_utf8(reinterpret_cast<const char*>(latin1Buffer.begin()),
              length, reinterpret_cast<char*>(backingStore->Data()));
      KJ_DASSERT(utf8_length == written);
    }
    return jsg::JsUint8Array::create(js, kj::mv(backingStore), 0, utf8_length);
  }

  // Use off-heap allocation for intermediate UTF-16 buffer to avoid wasting V8 heap space
  // and potentially triggering GC. Stack allocation for small strings, heap for large.
  // Stack allocation for small strings, heap for large.
  kj::SmallArray<uint16_t, MAX_SIZE_FOR_STACK_ALLOC> utf16Buffer(length);

  [[maybe_unused]] auto writeResult = str.writeInto(js, utf16Buffer.asPtr());
  KJ_DASSERT(
      writeResult.written == length, "writeInto must completely overwrite the backing buffer");

  auto data = reinterpret_cast<char16_t*>(utf16Buffer.begin());
  auto lengthResult = simdutf::utf8_length_from_utf16_with_replacement(data, length);
  utf8_length = lengthResult.count;

  if (lengthResult.error == simdutf::SURROGATE) {
    // If there are surrogates there may be unpaired surrogates. Fix them.
    simdutf::to_well_formed_utf16(data, length, data);
  } else {
    KJ_DASSERT(lengthResult.error == simdutf::SUCCESS);
  }

  auto backingStore = js.allocBackingStore(utf8_length, jsg::Lock::AllocOption::UNINITIALIZED);
  [[maybe_unused]] auto written =
      simdutf::convert_utf16_to_utf8(data, length, reinterpret_cast<char*>(backingStore->Data()));
  KJ_DASSERT(written == utf8_length, "Conversion yielded wrong number of UTF-8 bytes");

  return jsg::JsUint8Array::create(js, kj::mv(backingStore), 0, utf8_length);
}

namespace {

constexpr bool isSurrogatePair(uint16_t lead, uint16_t trail) {
  // We would like to use simdutf::trim_partial_utf16, but it's not guaranteed
  // to work right on invalid UTF-16. Hence, we need this method to check for
  // surrogate pairs and correctly trim utf16 chunks.
  return (lead & 0xfc00) == 0xd800 && (trail & 0xfc00) == 0xdc00;
}

// Ignores surrogates conservatively.
constexpr size_t simpleUtfEncodingLength(uint16_t c) {
  return 1 + (c >= 0x80) + (c >= 0x400);
}

// Find how many UTF-16 or Latin1 code units fit when converted to UTF-8.
// May conservatively underestimate the largest number of code units we can fit
// because of undetected surrogate pairs on boundaries.
// Works even on malformed UTF-16.
template <typename Char>
size_t findBestFit(const Char* data, size_t length, size_t bufferSize) {
  size_t pos = 0;
  size_t utf8Accumulated = 0;
  // The SIMD is more efficient with a size that's a little over a multiple of 16.
  constexpr size_t CHUNK = 257;
  // The max number of UTF-8 output bytes per input code unit.
  constexpr bool UTF16 = sizeof(Char) == 2;
  constexpr size_t MAX_FACTOR = UTF16 ? 3 : 2;

  // Our initial guess at how much the number of elements expands in the
  // conversion to UTF-8.
  double expansion = 1.15;

  while (pos < length && utf8Accumulated < bufferSize) {
    size_t remainingInput = length - pos;
    size_t spaceRemaining = bufferSize - utf8Accumulated;
    KJ_DASSERT(expansion >= 1.15);

    // We estimate how many characters are likely to fit in the buffer, but
    // only try for CHUNK characters at a time to minimize the worst case
    // waste of time if we guessed too high.
    size_t guaranteedToFit = spaceRemaining / MAX_FACTOR;
    if (guaranteedToFit >= remainingInput) {
      // Don't even bother checking any more, it's all going to fit.  Hitting
      // this halfway through is also a good reason to limit the CHUNK size.
      return length;
    }
    size_t likelyToFit = kj::min(static_cast<size_t>(spaceRemaining / expansion), CHUNK);
    size_t fitEstimate = kj::max(1, kj::max(guaranteedToFit, likelyToFit));
    size_t chunkSize = kj::min(remainingInput, fitEstimate);
    if (chunkSize == 1) break;  // Not worth running this complicated stuff one char at a time.
    // No div-by-zero because remainingInput and fitEstimate are at least 1.
    KJ_DASSERT(chunkSize >= 1);

    size_t chunkUtf8Len;
    if constexpr (UTF16) {
      chunkUtf8Len = simdutf::utf8_length_from_utf16_with_replacement(data + pos, chunkSize).count;
    } else {
      chunkUtf8Len = simdutf::utf8_length_from_latin1(data + pos, chunkSize);
    }

    if (utf8Accumulated + chunkUtf8Len > bufferSize) {
      // Our chosen chunk didn't fit in the rest of the output buffer.
      KJ_DASSERT(chunkSize > guaranteedToFit);
      // Since it didn't fit we adjust our expansion guess upwards.
      expansion = kj::max(expansion * 1.1, (chunkUtf8Len * 1.1) / chunkSize);
    } else {
      // Use successful length calculation to adjust our expansion estimate.
      expansion = kj::max(1.15, (chunkUtf8Len * 1.1) / chunkSize);
      pos += chunkSize;
      utf8Accumulated += chunkUtf8Len;
    }
  }
  // Do the last few code units in a simpler way.
  while (pos < length && utf8Accumulated < bufferSize) {
    size_t extra = simpleUtfEncodingLength(data[pos]);
    if (utf8Accumulated + extra > bufferSize) break;
    pos++;
    utf8Accumulated += extra;
  }
  if (UTF16 && pos != 0 && pos != length && isSurrogatePair(data[pos - 1], data[pos])) {
    // We ended on a leading surrogate which has a matching trailing surrogate in the next
    // position.  In order to make progress when the bufferSize is tiny we try to include it.
    if (utf8Accumulated < bufferSize) {
      pos++;  // We had one more byte, so we can include the pair, UTF-8 encoding 3->4.
    } else {
      pos--;  // Don't chop the pair in half.
    }
  }
  return pos;
}

}  // namespace

// Test helpers used by encoding-test.c++ to verify findBestFit behavior.
namespace test {

size_t bestFit(const char* str, size_t bufferSize) {
  return findBestFit(str, strlen(str), bufferSize);
}

size_t bestFit(const char16_t* str, size_t bufferSize) {
  size_t length = 0;
  while (str[length] != 0) length++;
  return findBestFit(str, length, bufferSize);
}

}  // namespace test

TextEncoder::EncodeIntoResult TextEncoder::encodeInto(
    jsg::Lock& js, jsg::JsString input, jsg::JsUint8Array buffer) {
  if (!workerd::util::Autogate::isEnabled(workerd::util::AutogateKey::ENABLE_FAST_TEXTENCODER)) {
    auto result = input.writeInto(
        js, buffer.asArrayPtr<char>(), jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
    return TextEncoder::EncodeIntoResult{
      .read = static_cast<int>(result.read),
      .written = static_cast<int>(result.written),
    };
  }

  auto outputBuf = buffer.asArrayPtr<char>();
  size_t bufferSize = outputBuf.size();

  size_t read = 0;
  size_t written = 0;
  {
    // Scope for the view - we can't do anything that might cause a V8 GC!
    v8::String::ValueView view(js.v8Isolate, input);
    size_t length = view.length();

    if (view.is_one_byte()) {
      auto data = reinterpret_cast<const char*>(view.data8());
      simdutf::result result =
          simdutf::validate_ascii_with_errors(data, kj::min(length, bufferSize));
      written = read = result.count;
      auto outAddr = outputBuf.begin();
      kj::arrayPtr(outAddr, read).copyFrom(kj::arrayPtr(data, read));
      outAddr += read;
      data += read;
      length -= read;
      bufferSize -= read;
      if (length != 0 && bufferSize != 0) {
        size_t rest = findBestFit(data, length, bufferSize);
        if (rest != 0) {
          KJ_DASSERT(simdutf::utf8_length_from_latin1(data, rest) <= bufferSize);
          written += simdutf::convert_latin1_to_utf8(data, rest, outAddr);
          read += rest;
        }
      }
    } else {
      auto data = reinterpret_cast<const char16_t*>(view.data16());
      read = findBestFit(data, length, bufferSize);
      if (read != 0) {
        KJ_DASSERT(
            simdutf::utf8_length_from_utf16_with_replacement(data, read).count <= bufferSize);
        simdutf::result result =
            simdutf::convert_utf16_to_utf8_with_errors(data, read, outputBuf.begin());
        if (result.error == simdutf::SUCCESS) {
          written = result.count;
        } else {
          // Oh, no, there are unpaired surrogates.  This is hopefully rare.
          kj::SmallArray<char16_t, MAX_SIZE_FOR_STACK_ALLOC> conversionBuffer(read);
          simdutf::to_well_formed_utf16(data, read, conversionBuffer.begin());
          written =
              simdutf::convert_utf16_to_utf8(conversionBuffer.begin(), read, outputBuf.begin());
        }
      }
    }
  }
  KJ_DASSERT(written <= bufferSize);
  // V8's String::kMaxLenth is a lot less than a maximal int so this is fine.
  using RInt = decltype(TextEncoder::EncodeIntoResult::read);
  using WInt = decltype(TextEncoder::EncodeIntoResult::written);
  KJ_DASSERT(0 <= read && read <= std::numeric_limits<RInt>::max());
  KJ_DASSERT(0 <= written && written <= std::numeric_limits<WInt>::max());
  return TextEncoder::EncodeIntoResult{
    .read = static_cast<RInt>(read),
    .written = static_cast<WInt>(written),
  };
}

}  // namespace workerd::api
