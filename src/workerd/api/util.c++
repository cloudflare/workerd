// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "util.h"

#include "simdutf.h"

#include <workerd/util/mimetype.h>
#include <workerd/util/strings.h>

#include <kj/encoding.h>

namespace workerd::api {
namespace {

kj::ArrayPtr<const char> split(kj::ArrayPtr<const char>& text, char c) {
  // TODO(cleanup): Modified version of split() found in kj/compat/url.c++.

  for (auto i: kj::indices(text)) {
    if (text[i] == c) {
      kj::ArrayPtr<const char> result = text.first(i);
      text = text.slice(i + 1, text.size());
      return result;
    }
  }
  auto result = text;
  text = {};
  return result;
}

}  // namespace

void parseQueryString(kj::Vector<kj::Url::QueryParam>& query,
    kj::ArrayPtr<const char> text,
    bool skipLeadingQuestionMark) {
  if (skipLeadingQuestionMark && text.size() > 0 && text[0] == '?') {
    text = text.slice(1, text.size());
  }

  while (text.size() > 0) {
    auto value = split(text, '&');
    if (value.size() == 0) continue;
    auto name = split(value, '=');
    query.add(kj::Url::QueryParam{kj::decodeWwwForm(name), kj::decodeWwwForm(value)});
  }
}

kj::Maybe<kj::String> readContentTypeParameter(kj::StringPtr contentType, kj::StringPtr param) {
  KJ_IF_SOME(parsed, MimeType::tryParse(contentType)) {
    return parsed.params().find(toLower(param)).map([](auto& value) { return kj::str(value); });
  }
  return kj::none;
}

kj::Maybe<kj::Exception> translateKjException(
    const kj::Exception& exception, std::initializer_list<ErrorTranslation> translations) {
  for (auto& t: translations) {
    if (exception.getDescription().contains(t.kjDescription)) {
      return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
          kj::str(JSG_EXCEPTION(TypeError) ": ", t.jsDescription));
    }
  }

  return kj::none;
}

namespace {

template <typename Func>
auto translateTeeErrors(Func&& f) -> decltype(kj::fwd<Func>(f)()) {
  try {
    co_return co_await f();
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_IF_SOME(e,
        translateKjException(exception,
            {
              {"tee buffer size limit exceeded"_kj,
                "ReadableStream.tee() buffer limit exceeded. This error usually occurs when a Request or "
                "Response with a large body is cloned, then only one of the clones is read, forcing "
                "the Workers runtime to buffer the entire body in memory. To fix this issue, remove "
                "unnecessary calls to Request/Response.clone() and ReadableStream.tee(), and always read "
                "clones/tees in parallel."_kj},
            })) {
      kj::throwFatalException(kj::mv(e));
    }
    kj::throwFatalException(kj::mv(exception));
  }
}

}  // namespace

kj::Own<kj::AsyncInputStream> newTeeErrorAdapter(kj::Own<kj::AsyncInputStream> inner) {
  class Adapter final: public kj::AsyncInputStream {
   public:
    explicit Adapter(kj::Own<AsyncInputStream> inner): inner(kj::mv(inner)) {}

    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return translateTeeErrors([&] { return inner->tryRead(buffer, minBytes, maxBytes); });
    }

    kj::Maybe<uint64_t> tryGetLength() override {
      return inner->tryGetLength();
    };

    kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
      return translateTeeErrors([&] { return inner->pumpTo(output, amount); });
    }

    kj::Maybe<kj::Own<kj::AsyncInputStream>> tryTee(uint64_t limit) override {
      return inner->tryTee(limit);
    }

   private:
    kj::Own<AsyncInputStream> inner;
  };

  if (dynamic_cast<Adapter*>(inner.get()) != nullptr) {
    // HACK: Don't double-wrap. This can otherwise happen if we tee a tee.
    return kj::mv(inner);
  } else {
    return kj::heap<Adapter>(kj::mv(inner));
  }
}

kj::String redactUrl(kj::StringPtr url) {
  kj::Vector<char> redacted(url.size() + 1);
  const char* spanStart = url.begin();
  bool sawNonHexChar = false;
  uint digitCount = 0;
  uint upperCount = 0;
  uint lowerCount = 0;
  uint hexDigitCount = 0;

  auto maybeRedactSpan = [&](kj::ArrayPtr<const char> span) {
    bool isHexId = (hexDigitCount >= 32 && !sawNonHexChar);
    bool probablyBase64Id =
        (span.size() >= 21 && digitCount >= 2 && upperCount >= 2 && lowerCount >= 2);

    if (isHexId || probablyBase64Id) {
      redacted.addAll("REDACTED"_kj);
    } else {
      redacted.addAll(span);
    }
  };

  for (const char& c: url) {
    uint8_t lookup = kCharLookupTable[static_cast<const kj::byte>(c)];
    bool isSep = lookup & CharAttributeFlag::SEPARATOR;
    bool isAlphaUpper = lookup & CharAttributeFlag::UPPER_CASE;
    bool isAlphaLower = lookup & CharAttributeFlag::LOWER_CASE;
    bool isDigit = lookup & CharAttributeFlag::DIGIT;
    bool isHex = lookup & CharAttributeFlag::HEX;

    // These extra characters are used in the regular and url-safe versions of
    // base64, but might also be used for GUID-style separators in hex ids.
    // Regular base64 also includes '/', which we don't try to match here due
    // to its prevalence in URLs.  Likewise, we ignore the base64 "=" padding
    // character.

    if (isAlphaUpper || isAlphaLower || isDigit || isSep) {
      if (isHex) {
        hexDigitCount++;
      }
      if (!isHex && !isSep) {
        sawNonHexChar = true;
      }
      if (isAlphaUpper) {
        upperCount++;
      }
      if (isAlphaLower) {
        lowerCount++;
      }
      if (isDigit) {
        digitCount++;
      }
    } else {
      maybeRedactSpan(kj::ArrayPtr<const char>(spanStart, &c));
      redacted.add(c);
      spanStart = &c + 1;
      hexDigitCount = 0;
      digitCount = 0;
      upperCount = 0;
      lowerCount = 0;
      sawNonHexChar = false;
    }
  }
  maybeRedactSpan(kj::ArrayPtr<const char>(spanStart, url.end()));
  redacted.add('\0');

  return kj::String(redacted.releaseAsArray());
}

kj::Maybe<jsg::V8Ref<v8::Object>> cloneRequestCf(
    jsg::Lock& js, kj::Maybe<jsg::V8Ref<v8::Object>> maybeCf) {
  KJ_IF_SOME(cf, maybeCf) {
    return cf.deepClone(js);
  }
  return kj::none;
}

void maybeWarnIfNotText(jsg::Lock& js, kj::StringPtr str) {
  KJ_IF_SOME(parsed, MimeType::tryParse(str)) {
    if (MimeType::isText(parsed)) return;
  }
  // A common mistake is to call .text() on non-text content, e.g. because you're implementing a
  // search-and-replace across your whole site and you forgot that it'll apply to images too.
  // When running in the fiddle, let's warn the developer if they do this.
  js.logWarning(
      kj::str("Called .text() on an HTTP body which does not appear to be text. The body's "
              "Content-Type is \"",
          str,
          "\". The result will probably be corrupted. Consider "
          "checking the Content-Type header before interpreting entities as text."));
}

kj::String fastEncodeBase64Url(kj::ArrayPtr<const byte> bytes) {
  if (KJ_UNLIKELY(bytes.size() == 0)) {
    return {};
  }
  auto expected_length = simdutf::base64_length_from_binary(bytes.size(), simdutf::base64_url);
  auto output = kj::heapArray<char>(expected_length + 1);
  auto actual_length = simdutf::binary_to_base64(
      bytes.asChars().begin(), bytes.size(), output.asChars().begin(), simdutf::base64_url);
  output[actual_length] = '\0';
  return kj::String(kj::mv(output));
}

kj::Array<char16_t> fastEncodeUtf16(kj::ArrayPtr<const char> bytes) {
  if (KJ_UNLIKELY(bytes.size() == 0)) {
    return {};
  }
  auto expected_length = simdutf::utf16_length_from_utf8(bytes.asChars().begin(), bytes.size());
  auto output = kj::heapArray<char16_t>(expected_length);
  auto actual_length =
      simdutf::convert_utf8_to_utf16(bytes.asChars().begin(), bytes.size(), output.begin());
  return output.first(actual_length).attach(kj::mv(output));
}

}  // namespace workerd::api
