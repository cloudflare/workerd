// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "encoding.h"

#include "simdutf.h"
#include "util.h"

#include <workerd/io/features.h>
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

// Full 256-entry windows-1252 byte-to-Unicode lookup table.
// For most entries table[i] == i (identity mapping). Bytes 0x80-0x9F
// differ from Latin-1 and map to their correct windows-1252 code points.
// Undefined bytes (0x81, 0x8D, 0x8F, 0x90, 0x9D) map to 0x0000 as a sentinel.
// See: https://encoding.spec.whatwg.org/index-windows-1252.txt
// clang-format off
static constexpr uint16_t WIN1252_TABLE[256] = {
    // 0x00-0x0F
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    // 0x10-0x1F
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    // 0x20-0x2F
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    // 0x30-0x3F
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    // 0x40-0x4F
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    // 0x50-0x5F
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    // 0x60-0x6F
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    // 0x70-0x7F
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    // 0x80-0x8F — windows-1252 diverges from Latin-1 here
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    // 0x90-0x9F
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    // 0xA0-0xAF
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    // 0xB0-0xBF
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    // 0xC0-0xCF
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    // 0xD0-0xDF
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    // 0xE0-0xEF
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    // 0xF0-0xFF
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};
// clang-format on

kj::Maybe<jsg::JsString> AsciiDecoder::decode(
    jsg::Lock& js, kj::ArrayPtr<const kj::byte> buffer, bool flush) {
  // Single branchless scan: accumulate whether any byte maps to a
  // different code point than its raw value. For bytes outside 0x80-0x9F
  // the table is identity so the XOR is zero and contributes nothing.
  uint16_t diff = 0;
  for (auto byte: buffer) {
    diff |= WIN1252_TABLE[byte] ^ byte;
  }

  if (diff == 0) {
    // Fast path: all bytes mapped to their own value (pure ASCII or
    // 0xA0-0xFF range), so Latin-1 identity decoding is correct.
    return js.str(buffer);
  }

  // Slow path: at least one byte in 0x80-0x9F needs remapping.
  // Since some windows-1252 code points are > 0xFF we use uint16_t.
  auto result = kj::heapArray<uint16_t>(buffer.size());
  for (size_t i = 0; i < buffer.size(); i++) {
    result[i] = WIN1252_TABLE[buffer[i]];
  }

  return js.str(result.asPtr());
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

namespace {
TextEncoder::EncodeIntoResult encodeIntoImpl(
    jsg::Lock& js, jsg::JsString input, jsg::BufferSource& buffer) {
  auto result = input.writeInto(
      js, buffer.asArrayPtr().asChars(), jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
  return TextEncoder::EncodeIntoResult{
    .read = static_cast<int>(result.read),
    .written = static_cast<int>(result.written),
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
    jsg::Lock& js, jsg::JsString input, jsg::JsUint8Array buffer) {
  auto result = input.writeInto(
      js, buffer.asArrayPtr<char>(), jsg::JsString::WriteFlags::REPLACE_INVALID_UTF8);
  return TextEncoder::EncodeIntoResult{
    .read = static_cast<int>(result.read),
    .written = static_cast<int>(result.written),
  };
}

}  // namespace workerd::api
