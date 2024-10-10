#include "data-url.h"

#include <workerd/api/encoding.h>
#include <workerd/util/strings.h>

#include <kj/encoding.h>

namespace workerd::api {

kj::Maybe<DataUrl> DataUrl::tryParse(kj::StringPtr url) {
  KJ_IF_SOME(url, jsg::Url::tryParse(url)) {
    return from(url);
  }
  return kj::none;
}

kj::Maybe<DataUrl> DataUrl::from(const jsg::Url& url) {
  if (url.getProtocol() != "data:"_kj) return kj::none;
  auto clone = url.clone(jsg::Url::EquivalenceOption::IGNORE_FRAGMENTS);

  // Remove the "data:" prefix.
  auto href = clone.getHref().slice(5);

  // We scan for the first comma, which separates the MIME type from the data.
  // Per the fetch spec, it doesn't matter if the comma is within a quoted
  // string value in the MIME type... which is fun.

  static const auto trim = [](auto label) {
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

  static const auto strip = [](auto label) {
    auto result = kj::heapArray<kj::byte>(label.size());
    size_t len = 0;
    for (const kj::byte c: label) {
      if (!isAsciiWhitespace(c)) {
        result[len++] = c;
      }
    }
    return result.first(len).attach(kj::mv(result));
  };

  static const auto isBase64 = [](kj::ArrayPtr<const char> label) -> bool {
    KJ_IF_SOME(pos, label.findLast(';')) {
      auto res = trim(label.slice(pos + 1));
      return res.size() == 6 && (res[0] | 0x20) == 'b' && (res[1] | 0x20) == 'a' &&
          (res[2] | 0x20) == 's' && (res[3] | 0x20) == 'e' && (res[4] == '6') && (res[5] == '4');
    }
    return false;
  };

  static const auto create = [](auto input, auto decoded) {
    KJ_IF_SOME(parsed, MimeType::tryParse(input)) {
      return DataUrl(kj::mv(parsed), kj::mv(decoded));
    } else {
      return DataUrl(MimeType::PLAINTEXT_ASCII.clone(), kj::mv(decoded));
    }
  };

  KJ_IF_SOME(pos, href.findFirst(',')) {
    auto unparsed = href.first(pos);
    auto data = href.slice(pos + 1);

    // We need to trim leading and trailing whitespace from the mimetype
    unparsed = trim(unparsed);

    // Determine if the data is base64 encoded
    kj::Array<kj::byte> decoded = nullptr;
    if (isBase64(unparsed)) {
      unparsed = unparsed.first(KJ_ASSERT_NONNULL(unparsed.findLast(';')));
      decoded = kj::decodeBase64(strip(jsg::Url::percentDecode(data.asBytes())).asChars());
    } else {
      decoded = jsg::Url::percentDecode(data.asBytes());
    }

    if (unparsed.startsWith(";"_kj)) {
      // If the mime type starts with ;, then the spec tells us to
      // prepend "text/plain" to the mime type.
      auto fixed = kj::str("text/plain", unparsed);
      return create(fixed.asPtr(), kj::mv(decoded));
    }

    return create(unparsed, kj::mv(decoded));
  }

  // If there are no commas, the data: url is invalid.
  return kj::none;
}

}  // namespace workerd::api
