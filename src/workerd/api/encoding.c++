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

#include <kj/array.h>
#include <kj/string.h>

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
  // Per the WHATWG encoding spec (section 10.1.1), GBK's decoder is gb18030's decoder.
  // https://encoding.spec.whatwg.org/#gbk-decoder
  // We can't change getEncodingId() itself because it is also used for the TextDecoder.encoding
  // getter, which must still return "gbk" for GBK.
  auto icuEncoding =
      encoding == Encoding::Gbk ? getEncodingId(Encoding::Gb18030) : getEncodingId(encoding);
  UConverter* inner = ucnv_open(icuEncoding.cStr(), &status);
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

  KJ_DEFER({
    if (flush) reset();
  });

  // Evaluate fast-path options. These provide shortcuts for common cases with the caveat
  // that error handling for invalid sequences might be a bit different (because the
  // conversions are being handled by v8 directly rather than by the ICU converter).
  if (buffer.size() > 0 && ucnv_toUCountPending(inner.get(), &status) == 0) {
    KJ_ASSERT(U_SUCCESS(status));
    if (encoding == Encoding::Utf8 &&
        simdutf::validate_ascii(buffer.asChars().begin(), buffer.size())) {
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

      // The input buffer may be at an odd byte offset (e.g. a Uint8Array view
      // at offset 3 into an ArrayBuffer), which makes reinterpret_cast to
      // char16_t* undefined behavior due to alignment violation. Copy into an
      // aligned buffer to avoid this.
      auto bufSize = buffer.size() / 2;
      kj::SmallArray<char16_t, 256> aligned(bufSize);
      aligned.asBytes().copyFrom(buffer.first(bufSize * 2));
      auto data = aligned.asPtr();

      if (flush || !U_IS_SURROGATE_LEAD(data[data.size() - 1])) {
        bool omitInitialBom = false;
        if (!ignoreBom && !bomSeen) {
          omitInitialBom = data[0] == 0xfeff;
          bomSeen = true;
        }

        auto slice = data.slice(omitInitialBom ? 1 : 0, data.size());

        // If textDecoderReplaceSurrogates flag is enabled, then we follow the spec
        // and fix invalid surrogates on the UTF-16 input.
        if (slice.size() == 0 || !FeatureFlags::get(js).getTextDecoderReplaceSurrogates()) {
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
              : kj::max(buffer.size(),
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

void IcuDecoder::reset() {
  bomSeen = false;
  return ucnv_reset(inner.get());
}

Decoder& TextDecoder::getImpl() {
  KJ_SWITCH_ONEOF(decoder) {
    KJ_CASE_ONEOF(dec, LegacyDecoder) {
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
    JSG_REQUIRE(encoding != Encoding::Replacement && encoding != Encoding::INVALID, RangeError,
        errorMessage(label));
  }

  switch (encoding) {
    case Encoding::Big5:
    case Encoding::Euc_Jp:
    case Encoding::Euc_Kr:
    case Encoding::Gb18030:
    case Encoding::Gbk:
    case Encoding::Iso2022_Jp:
    case Encoding::Shift_Jis: {
      // If the feature flag is disabled, we use the ICU decoder.
      if (!FeatureFlags::get(js).getTextDecoderCjkDecoder()) {
        break;
      }

      // We fallthrough to LegacyDecoder in order to avoid breaking changes.
      [[fallthrough]];
    }
    case Encoding::X_User_Defined:
    case Encoding::Windows_1252:
      return js.alloc<TextDecoder>(LegacyDecoder(encoding, DecoderFatal(options.fatal)), options);
    default:
      break;
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
    KJ_CASE_ONEOF(dec, LegacyDecoder) {
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

namespace {

jsg::JsUint8Array encodeUtf8WithV8(jsg::Lock& js, jsg::JsString str) {
  auto view = jsg::JsUint8Array::create(js, str.utf8Length(js));
  [[maybe_unused]] auto result = str.writeInto(
      js, view.asArrayPtr().asChars(), jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
  KJ_DASSERT(result.written == view.size());
  return view;
}

jsg::JsUint8Array encodeUtf8WithSimdutf(jsg::Lock& js, jsg::JsString str) {
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

    auto view = jsg::JsUint8Array::create(js, utf8_length);
    if (utf8_length == length) {
      // ASCII fast path: no conversion needed, Latin-1 is same as UTF-8 for ASCII
      view.asArrayPtr().copyFrom(latin1Buffer);
    } else {
      [[maybe_unused]] auto written =
          simdutf::convert_latin1_to_utf8(reinterpret_cast<const char*>(latin1Buffer.begin()),
              length, view.asArrayPtr().asChars().begin());
      KJ_DASSERT(utf8_length == written);
    }
    return view;
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

  auto view = jsg::JsUint8Array::create(js, utf8_length);
  [[maybe_unused]] auto written =
      simdutf::convert_utf16_to_utf8(data, length, view.asArrayPtr().asChars().begin());
  KJ_DASSERT(written == utf8_length, "Conversion yielded wrong number of UTF-8 bytes");
  return view;
}

}  // namespace

jsg::JsUint8Array TextEncoder::encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input) {
  auto str = input.orDefault(js.str());
  if (!workerd::util::Autogate::isEnabled(workerd::util::AutogateKey::ENABLE_FAST_TEXTENCODER)) {
    return encodeUtf8WithV8(js, str);
  }
  return encodeUtf8WithSimdutf(js, str);
}

jsg::JsUint8Array encodeUtf8Chunk(jsg::Lock& js, jsg::JsString str) {
  // V8 writes a one-byte string straight into the result, where encodeUtf8WithSimdutf()
  // copies it through a temporary buffer first, so V8 is the faster of the two for one-byte
  // strings and simdutf for two-byte strings.
  if (str.isOneByte(js)) {
    return encodeUtf8WithV8(js, str);
  }
  return encodeUtf8WithSimdutf(js, str);
}

namespace {

struct EncodedPrefix {
  size_t read;
  size_t written;
};

EncodedPrefix encodeLatin1Prefix(kj::ArrayPtr<const char> input, kj::ArrayPtr<char> output) {
  size_t ascii =
      simdutf::validate_ascii_with_errors(input.begin(), kj::min(input.size(), output.size()))
          .count;
  auto rest = input.slice(ascii);
  output.write(input.first(ascii));
  if (rest.size() == 0 || output.size() == 0) {
    return {ascii, ascii};
  }
  if (output.size() / 2 >= rest.size()) {
    return {input.size(),
      ascii + simdutf::convert_latin1_to_utf8(rest.begin(), rest.size(), output.begin())};
  }
  auto result = simdutf::convert_latin1_to_utf8_safe_with_details(
      rest.begin(), rest.size(), output.begin(), output.size());
  return {ascii + result.input_count, ascii + result.output_count};
}

EncodedPrefix encodeUtf16Prefix(kj::ArrayPtr<const char16_t> input, kj::ArrayPtr<char> output) {
  size_t read;
  size_t written;
  if (output.size() / 3 >= input.size()) {
    auto result =
        simdutf::convert_utf16_to_utf8_with_errors(input.begin(), input.size(), output.begin());
    if (result.error == simdutf::SUCCESS) {
      return {input.size(), result.count};
    }
    // The output before the error is complete, which simdutf's bounded conversions also rely on.
    read = result.count;
    written = simdutf::utf8_length_from_utf16(input.begin(), read);
  } else {
    auto result = simdutf::convert_utf16_to_utf8_safe_with_details(
        input.begin(), input.size(), output.begin(), output.size());
    if (result.error == simdutf::OUTPUT_BUFFER_TOO_SMALL) {
      // simdutf reports a lone lead surrogate with three bytes left as too small rather than as
      // SURROGATE, which would drop a U+FFFD that fits.
      auto tail = simdutf::convert_utf16_to_utf8_with_replacement_safe(
          input.begin() + result.input_count, input.size() - result.input_count,
          output.begin() + result.output_count, output.size() - result.output_count);
      return {result.input_count + tail.input_count, result.output_count + tail.output_count};
    }
    if (result.error != simdutf::SURROGATE) {
      return {result.input_count, result.output_count};
    }
    read = result.input_count;
    written = result.output_count;
  }
  // A lone surrogate becomes U+FFFD in place, so a well-formed copy reads and writes the same
  // counts. simdutf's replacing conversions restart at every lone surrogate, which is far slower
  // when they are dense. Each code unit takes at least one byte, so at most restOut.size() of
  // them are read, and one more keeps a pair on that boundary whole.
  auto restIn = input.slice(read);
  auto restOut = output.slice(written);
  size_t length = kj::min(restIn.size(), restOut.size() + 1);
  kj::SmallArray<char16_t, MAX_SIZE_FOR_STACK_ALLOC> wellFormed(length);
  simdutf::to_well_formed_utf16(restIn.begin(), length, wellFormed.begin());
  if (restOut.size() / 3 >= length) {
    return {read + length,
      written + simdutf::convert_utf16_to_utf8(wellFormed.begin(), length, restOut.begin())};
  }
  auto result = simdutf::convert_utf16_to_utf8_safe_with_details(
      wellFormed.begin(), length, restOut.begin(), restOut.size());
  return {read + result.input_count, written + result.output_count};
}

}  // namespace

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
  EncodedPrefix encoded;
  {
    // Scope for the view - we can't do anything that might cause a V8 GC!
    v8::String::ValueView view(js.v8Isolate, input);
    if (view.is_one_byte()) {
      encoded = encodeLatin1Prefix(
          kj::arrayPtr(reinterpret_cast<const char*>(view.data8()), view.length()), outputBuf);
    } else {
      encoded = encodeUtf16Prefix(
          kj::arrayPtr(reinterpret_cast<const char16_t*>(view.data16()), view.length()), outputBuf);
    }
  }
  size_t read = encoded.read;
  size_t written = encoded.written;
  KJ_DASSERT(written <= outputBuf.size());
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
