// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "util.h"

#include <workerd/jsg/jsg.h>
#include <workerd/util/strings.h>

#include <unicode/ucnv.h>
#include <unicode/utf8.h>

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
  kj::String labelInsensitive = toLower(label);
  const auto trim = [](kj::StringPtr label) {
    size_t start = 0;
    auto end = label.size();
    while (start < end && isAsciiWhitespace(label[start])) {
      start++;
    }
    while (end > start && isAsciiWhitespace(label[end - 1])) {
      end--;
    }
    return label.slice(start, end).asChars();
  };

  auto trimmed = trim(labelInsensitive);
#define V(label, key)                                                                              \
  if (trimmed == label##_kj) return Encoding::key;
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

jsg::Ref<TextDecoder> TextDecoder::constructor(
    jsg::Optional<kj::String> maybeLabel, jsg::Optional<ConstructorOptions> maybeOptions) {
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
    return jsg::alloc<TextDecoder>(AsciiDecoder(), options);
  }

  return jsg::alloc<TextDecoder>(
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

jsg::Ref<TextEncoder> TextEncoder::constructor() {
  return jsg::alloc<TextEncoder>();
}

namespace {
TextEncoder::EncodeIntoResult encodeIntoImpl(
    jsg::Lock& js, jsg::JsString input, jsg::BufferSource& buffer) {
  auto result = input.writeInto(js, buffer.asArrayPtr().asChars(),
      static_cast<jsg::JsString::WriteOptions>(
          jsg::JsString::NO_NULL_TERMINATION | jsg::JsString::REPLACE_INVALID_UTF8));
  return TextEncoder::EncodeIntoResult{
    .read = result.read,
    .written = result.written,
  };
}
}  // namespace

jsg::BufferSource TextEncoder::encode(jsg::Lock& js, jsg::Optional<jsg::JsString> input) {
  auto str = input.orDefault(js.str());
  auto view = JSG_REQUIRE_NONNULL(jsg::BufferSource::tryAlloc(js, str.utf8Length(js)), RangeError,
      "Cannot allocate space for TextEncoder.encode");
  [[maybe_unused]] auto result = encodeIntoImpl(js, str, view);
  KJ_DASSERT(result.written == view.size());
  return kj::mv(view);
}

TextEncoder::EncodeIntoResult TextEncoder::encodeInto(
    jsg::Lock& js, jsg::JsString input, jsg::BufferSource buffer) {
  auto handle = buffer.getHandle(js);
  JSG_REQUIRE(handle->IsUint8Array(), TypeError, "buffer must be a Uint8Array");
  return encodeIntoImpl(js, input, buffer);
}

}  // namespace workerd::api
