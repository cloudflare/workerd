// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "form-data.h"
#include "util.h"
#include <kj/vector.h>
#include <kj/encoding.h>
#include <algorithm>
#include <functional>
#include <regex>
#include <strings.h>
#include <kj/parse/char.h>
#include <kj/compat/http.h>

namespace workerd::api {

namespace {

kj::ArrayPtr<const char> splitAtSubString(
    kj::ArrayPtr<const char>& text, kj::StringPtr subString) {
  // Like split() in kj/compat/url.c++, but splits at a substring rather than a character.

  // TODO(perf): Use a Boyer-Moore search?
  auto iter = std::search(text.begin(), text.end(), subString.begin(), subString.end());
  auto result = kj::arrayPtr(text.begin(), iter - text.begin());
  text = text.slice(kj::min(text.size(), result.end() - text.begin() + subString.size()),
                    text.size());
  return result;
}

bool startsWith(kj::ArrayPtr<const char> bytes, kj::StringPtr prefix) {
  return bytes.size() >= prefix.size() && bytes.slice(0, prefix.size()) == prefix;
}

struct FormDataHeaderTable {
  kj::HttpHeaderId contentDispositionId;
  kj::Own<kj::HttpHeaderTable> table;

  FormDataHeaderTable(kj::HttpHeaderTable::Builder builder)
      : contentDispositionId(builder.add("Content-Disposition")),
        table(builder.build()) {}
};

const FormDataHeaderTable& getFormDataHeaderTable() {
  static FormDataHeaderTable table({});
  return table;
}

namespace p = kj::parse;
constexpr auto httpIdentifier = p::oneOrMore(p::nameChar.orChar('-'));
constexpr auto quotedChar = p::oneOf(
    p::anyOfChars("\"\n\\").invert(),
    // Chrome interprets "\<c>" as reducing to <c> for any character <c>, including double quote.
    // (So "\n" = "n", etc.)
    p::sequence(p::exactChar<'\\'>(), p::anyOfChars("\n").invert()));
constexpr auto contentDispositionParam =
    p::sequence(p::exactChar<';'>(), p::discardWhitespace,
                httpIdentifier, p::discardWhitespace,
                p::exactChar<'='>(), p::discardWhitespace,
                p::exactChar<'"'>(),
                p::oneOrMore(quotedChar),
                p::exactChar<'"'>(), p::discardWhitespace);
constexpr auto contentDisposition =
    p::sequence(p::discardWhitespace, httpIdentifier,
                p::discardWhitespace, p::many(contentDispositionParam));

void parseFormData(kj::Vector<FormData::Entry>& data, kj::ArrayPtr<const char> boundary,
                   kj::ArrayPtr<const char> body, bool convertFilesToStrings) {
  // multipart/form-data messages are delimited by <CRLF>--<boundary>. We want to be able to handle
  // omitted carriage returns, though, so our delimiter only matches against a preceding line feed.
  const auto delimiter = kj::str("\n--", boundary);

  // We want to slice off the delimiter's preceding newline for the initial search, because the very
  // first instance does not require one. In every subsequent multipart message, the preceding
  // newline is required.
  auto message = splitAtSubString(body, delimiter.slice(1));

  JSG_REQUIRE(body.size() > 0, TypeError,
      "No initial boundary string (or you have a truncated message).");

  const auto done = [](kj::ArrayPtr<const char>& body) {
    // Consume any (CR)LF characters that trailed the boundary and indicate continuation, or consume
    // the terminal "--" characters and indicate termination, or throw an error.
    if (startsWith(body, "\n")) {
      body = body.slice(1, body.size());
    } else if (startsWith(body, "\r\n")) {
      body = body.slice(2, body.size());
    } else if (startsWith(body, "--")) {
      // We're done!
      return true;
    } else {
      JSG_FAIL_REQUIRE(TypeError, "Boundary string was not succeeded by CRLF, LF, or '--'.");
    }
    return false;
  };

  constexpr auto staticRegexFlags = std::regex_constants::ECMAScript
                                  | std::regex_constants::optimize;

  static const auto headerTerminationRegex = std::regex("\r?\n\r?\n", staticRegexFlags);

  std::cmatch match;

  auto& formDataHeaderTable = getFormDataHeaderTable();

  while (!done(body)) {
    JSG_REQUIRE(std::regex_search(body.begin(), body.end(), match, headerTerminationRegex),
                 TypeError, "No multipart message header termination found.");

    // TODO(cleanup): Use kj-http to parse multipart headers. Right now that API isn't public, so
    //   I'm just using a regex. For reference, multipart/form-data supports the following three
    //   headers (https://tools.ietf.org/html/rfc7578#section-4.8):
    //
    //   Content-Disposition        (required)
    //   Content-Type               (optional, recommended for files)
    //   Content-Transfer-Encoding  (for 7-bit encoding, deprecated in HTTP contexts)
    //
    // TODO(soon): Read the Content-Type to support files.

    auto headersText = kj::str(body.slice(0, match[0].second - body.begin()));
    body = body.slice(match[0].second - body.begin(), body.size());

    kj::HttpHeaders headers(*formDataHeaderTable.table);
    JSG_REQUIRE(headers.tryParse(headersText), TypeError, "FormData part had invalid headers.");

    kj::StringPtr disposition = JSG_REQUIRE_NONNULL(
        headers.get(formDataHeaderTable.contentDispositionId),
        TypeError, "No valid Content-Disposition header found in FormData part.");

    kj::Maybe<kj::String> maybeName;
    kj::Maybe<kj::String> filename;
    {
      p::IteratorInput<char, const char*> input(disposition.begin(), disposition.end());
      auto result = JSG_REQUIRE_NONNULL(contentDisposition(input),
          TypeError, "Invalid Content-Disposition header found in FormData part.");
      JSG_REQUIRE(kj::get<0>(result) == "form-data"_kj.asArray(), TypeError,
          "Content-Disposition header for FormData part must have the value \"form-data\", "
          "possibly followed by parameters. Got: \"", kj::get<0>(result), "\"");

      for (auto& param: kj::get<1>(result)) {
        if (kj::get<0>(param) == "name"_kj.asArray()) {
          maybeName = kj::str(kj::get<1>(param));
        } else if (kj::get<0>(param) == "filename"_kj.asArray()) {
          filename = kj::str(kj::get<1>(param));
        }
      }
    }

    kj::String name = JSG_REQUIRE_NONNULL(kj::mv(maybeName),
        TypeError, "Content-Disposition header in FormData part is missing a name.");

    kj::Maybe<kj::StringPtr> type = headers.get(kj::HttpHeaderId::CONTENT_TYPE);

    message = splitAtSubString(body, delimiter);
    JSG_REQUIRE(body.size() > 0, TypeError,
        "No subsequent boundary string after multipart message.");

    if (message.size() > 0) {
      // If we skipped a CR, we must avoid including it in the message data.
      message = message.slice(0, message.size() - uint(message.back() == '\r'));
    }

    if (filename == nullptr || convertFilesToStrings) {
      data.add(FormData::Entry { kj::mv(name), kj::str(message) });
    } else {
      data.add(FormData::Entry {
        kj::mv(name),
        jsg::alloc<File>(kj::heapArray(message.asBytes()), KJ_ASSERT_NONNULL(kj::mv(filename)),
                          kj::str(type.orDefault(nullptr)), dateNow())
      });
    }
  }
}

kj::OneOf<jsg::Ref<File>, kj::String>
blobToFile(kj::StringPtr name, kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
           jsg::Optional<kj::String> filename) {
  auto fromBlob = [&](jsg::Ref<Blob> blob) {
    kj::String fn;
    KJ_IF_MAYBE(f, filename) {
      fn = kj::mv(*f);
    } else {
      fn = kj::str(name);
    }
    return jsg::alloc<File>(kj::heapArray(blob->getData()), kj::mv(fn),
                             kj::str(blob->getType()), dateNow());
  };

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(file, jsg::Ref<File>) {
      if (filename == nullptr) {
        return kj::mv(file);
      } else {
        // Need to substitute filename.
        return fromBlob(kj::mv(file));
      }
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return fromBlob(kj::mv(blob));
    }
    KJ_CASE_ONEOF(string, kj::String) {
      return kj::mv(string);
    }
  }
  KJ_UNREACHABLE;
}

void addEscapingQuotes(kj::Vector<char>& builder, kj::StringPtr value) {
  // Add the chars from `value` into `builder` escaping the characters '"' and '\n' using %
  // encoding, exactly as Chrome does for Content-Disposition values.

  // Chrome throws "Failed to fetch" if the name ends with a backslash. Otherwise it worries that
  // the backslash may be interpreted as escaping the final quote.
  JSG_REQUIRE(!value.endsWith("\\"), TypeError, "Name or filename can't end with backslash");

  for (char c: value) {
    switch (c) {
      case '\"':
        // Firefox supposedly escapes this as '\"', but Chrome chooses to use percent escapes,
        // probably for fear of a buggy receiver who interprets the '"' as being the end of the
        // string. There is no standard.
        builder.addAll("%22"_kj);
        break;
      case '\n':
        builder.addAll("%0A"_kj);
        break;
      case '\\':
        // Chrome doesn't escape '\', but this awkwardly means that the '\' will be evaluated as
        // an escape sequence on the other end. That seems like a bug. Let's not copy bugs.
        builder.addAll("\\\\"_kj);
        break;
      default:
        builder.add(c);
        break;
    }
  }
}

}  // namespace

// =======================================================================================
// FormData implementation

kj::Array<kj::byte> FormData::serialize(kj::ArrayPtr<const char> boundary) {
  // Boundary string requirement per RFC7578
  JSG_REQUIRE(boundary.size() > 0 && boundary.size() <= 70, TypeError,
      "Length of multipart/form-data boundary string must be in the range [1, 70].");

  // TODO(perf): We should be able to trivially calculate the length of the serialized form data
  //   beforehand. I tried, but apparently my math REALLY sucks and I hate memory overruns, so ...
  auto builder = kj::Vector<char>{};

  for (auto& kv: data) {
    builder.addAll("--"_kj);
    builder.addAll(boundary);
    builder.addAll("\r\n"_kj);
    builder.addAll("Content-Disposition: form-data; name=\""_kj);
    addEscapingQuotes(builder, kv.name);
    KJ_SWITCH_ONEOF(kv.value) {
      KJ_CASE_ONEOF(text, kj::String) {
        builder.addAll("\"\r\n\r\n"_kj);
        builder.addAll(text);
      }
      KJ_CASE_ONEOF(file, jsg::Ref<File>) {
        builder.addAll("\"; filename=\""_kj);
        addEscapingQuotes(builder, file->getName());
        builder.addAll("\"\r\nContent-Type: "_kj);
        auto type = file->getType();
        if (type == nullptr) {
          type = "application/octet-stream"_kj;
        }
        builder.addAll(type);
        builder.addAll("\r\n\r\n"_kj);
        builder.addAll(file->getData().asChars());
      }
    }
    builder.addAll("\r\n"_kj);
  }
  builder.addAll("--"_kj);
  builder.addAll(boundary);
  builder.addAll("--"_kj);

  return builder.releaseAsArray().releaseAsBytes();
}

FormData::EntryType FormData::clone(v8::Isolate* isolate, FormData::EntryType& value) {
  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(file, jsg::Ref<File>) {
      return file.addRef();
    }
    KJ_CASE_ONEOF(string, kj::String) {
      return kj::str(string);
    }
  }
  KJ_UNREACHABLE;
}

void FormData::parse(kj::ArrayPtr<const char> rawText, kj::StringPtr contentType,
                     bool convertFilesToStrings) {
  if (contentType.startsWith("multipart/form-data")) {
    auto boundary = JSG_REQUIRE_NONNULL(readContentTypeParameter(contentType, "boundary"),
        TypeError, "No boundary string in Content-Type header. The multipart/form-data MIME "
        "type requires a boundary parameter, e.g. 'Content-Type: multipart/form-data; "
        "boundary=\"abcd\"'. See RFC 7578, section 4.");
    parseFormData(data, boundary, rawText, convertFilesToStrings);
  } else if (contentType.startsWith("application/x-www-form-urlencoded")) {
    // Let's read the charset so we can barf if the body isn't UTF-8.
    //
    // TODO(conform): Transcode to UTF-8, like the spec tells us to.
    KJ_IF_MAYBE(charsetParam, readContentTypeParameter(contentType, "charset")) {
      auto charset = kj::str(*charsetParam);
      JSG_REQUIRE(strcasecmp(charset.cStr(), "utf-8") == 0 ||
                 strcasecmp(charset.cStr(), "utf8") == 0 ||
                 strcasecmp(charset.cStr(), "unicode-1-1-utf-8") == 0,
          TypeError, "Non-utf-8 application/x-www-form-urlencoded body.");
    }
    kj::Vector<kj::Url::QueryParam> query;
    parseQueryString(query, kj::mv(rawText));
    data.reserve(query.size());
    for (auto& param: query) {
      data.add(Entry { kj::mv(param.name), kj::mv(param.value) });
    }
  } else {
    JSG_FAIL_REQUIRE(TypeError, "Unrecognized Content-Type header value. FormData can only "
        "parse the following MIME types: multipart/form-data, application/x-www-form-urlencoded.");
  }
}

jsg::Ref<FormData> FormData::constructor() {
  return jsg::alloc<FormData>();
}

void FormData::append(kj::String name,
    kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
    jsg::Optional<kj::String> filename) {
  auto filifiedValue = blobToFile(name, kj::mv(value), kj::mv(filename));
  data.add(Entry { kj::mv(name), kj::mv(filifiedValue) });
}

void FormData::delete_(kj::String name) {
  auto pivot = std::remove_if(data.begin(), data.end(),
                              [&name](const auto& kv) { return kv.name == name; });
  data.truncate(pivot - data.begin());
}

kj::Maybe<kj::OneOf<jsg::Ref<File>, kj::String>> FormData::get(
    kj::String name, v8::Isolate* isolate) {
  for (auto& [k, v]: data) {
    if (k == name) {
      return clone(isolate, v);
    }
  }
  return nullptr;
}

kj::Array<kj::OneOf<jsg::Ref<File>, kj::String>> FormData::getAll(
    kj::String name, v8::Isolate* isolate) {
  kj::Vector<kj::OneOf<jsg::Ref<File>, kj::String>> result;
  for (auto& [k, v]: data) {
    if (k == name) {
      result.add(clone(isolate, v));
    }
  }
  return result.releaseAsArray();
}

bool FormData::has(kj::String name) {
  for (auto& [k, v]: data) {
    if (k == name) {
      return true;
    }
  }
  return false;
}

void FormData::set(kj::String name,
    kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
    jsg::Optional<kj::String> filename) {
  // Set the first element named `name` to `value`, then remove all the rest matching that name.
  const auto predicate = [name = name.slice(0)](const auto& kv) { return kv.name == name; };
  auto firstFound = std::find_if(data.begin(), data.end(), predicate);
  if (firstFound != data.end()) {
    firstFound->value = blobToFile(name, kj::mv(value), kj::mv(filename));
    auto pivot = std::remove_if(++firstFound, data.end(), predicate);
    data.truncate(pivot - data.begin());
  } else {
    append(kj::mv(name), kj::mv(value), kj::mv(filename));
  }
}

jsg::Ref<FormData::EntryIterator> FormData::entries(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<EntryIterator>(IteratorState { JSG_THIS });
}

jsg::Ref<FormData::KeyIterator> FormData::keys(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<KeyIterator>(IteratorState { JSG_THIS });
}

jsg::Ref<FormData::ValueIterator> FormData::values(
    jsg::Lock&,
    CompatibilityFlags::Reader featureFlags) {
  return jsg::alloc<ValueIterator>(IteratorState { JSG_THIS });
}

void FormData::forEach(
    jsg::Lock& js,
    jsg::V8Ref<v8::Function> callback,
    jsg::Optional<jsg::Value> thisArg,
    const jsg::TypeHandler<EntryType>& handler) {
  auto isolate = js.v8Isolate;
  auto localCallback = callback.getHandle(isolate);
  auto localThisArg = thisArg.map([&](jsg::Value& v) { return v.getHandle(isolate); })
      .orDefault(v8::Undefined(isolate));
  // JSG_THIS.tryGetHandle() is guaranteed safe because `forEach()` is only called
  // from JavaScript, which means a Headers JS wrapper object must already exist.
  auto localParams = KJ_ASSERT_NONNULL(JSG_THIS.tryGetHandle(isolate));

  auto context = isolate->GetCurrentContext();  // Needed later for Call().

  // On each iteration of the for loop, a JavaScript callback is invokved. If a new
  // item is appended to the URLSearchParams within that function, the loop must pick
  // it up. Using the classic for (;;) syntax here allows for that. However, this does
  // mean that it's possible for a user to trigger an infinite loop here if new items
  // are added to the search params unconditionally on each iteration.
  for (size_t i = 0; i < this->data.size(); i++) {
    auto& [key, value] = this->data[i];
    static constexpr auto ARG_COUNT = 3;

    v8::Local<v8::Value> args[ARG_COUNT] = {
      handler.wrap(js, clone(isolate, value)),
      jsg::v8Str(isolate, key),
      localParams,
    };
    // Call jsg::check() to propagate exceptions, but we don't expect any
    // particular return value.
    jsg::check(localCallback->Call(context, localThisArg, ARG_COUNT, args));
  }
}

}  // namespace workerd::api
