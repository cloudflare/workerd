// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "form-data.h"

#include "util.h"

#include <workerd/io/io-util.h>
#include <workerd/util/mimetype.h>

#include <kj/compat/http.h>
#include <kj/parse/char.h>
#include <kj/vector.h>

#include <algorithm>
#include <regex>

#if !_MSC_VER
#include <strings.h>
#endif

namespace workerd::api {

namespace {
// Like split() in kj/compat/url.c++, but splits at a substring rather than a character.
kj::ArrayPtr<const char> splitAtSubString(kj::ArrayPtr<const char>& text, kj::StringPtr subString) {
  // TODO(perf): Use a Boyer-Moore search?
  auto iter = std::search(text.begin(), text.end(), subString.begin(), subString.end());
  auto result = kj::arrayPtr(text.begin(), iter - text.begin());
  text =
      text.slice(kj::min(text.size(), result.end() - text.begin() + subString.size()), text.size());
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
  static const FormDataHeaderTable table({});
  return table;
}

namespace p = kj::parse;
constexpr auto httpIdentifier = p::oneOrMore(p::nameChar.orChar('-'));
constexpr auto quotedChar = p::oneOf(p::anyOfChars("\"\n\\").invert(),
    // Chrome interprets "\<c>" as reducing to <c> for any character <c>, including double quote.
    // (So "\n" = "n", etc.)
    p::sequence(p::exactChar<'\\'>(), p::anyOfChars("\n").invert()));
constexpr auto contentDispositionParam = p::sequence(p::exactChar<';'>(),
    p::discardWhitespace,
    httpIdentifier,
    p::discardWhitespace,
    p::exactChar<'='>(),
    p::discardWhitespace,
    p::exactChar<'"'>(),
    p::oneOrMore(quotedChar),
    p::exactChar<'"'>(),
    p::discardWhitespace);
constexpr auto contentDisposition = p::sequence(
    p::discardWhitespace, httpIdentifier, p::discardWhitespace, p::many(contentDispositionParam));

void parseFormData(kj::Maybe<jsg::Lock&> js,
    kj::Vector<FormData::Entry>& data,
    kj::StringPtr boundary,
    kj::ArrayPtr<const char> body,
    bool convertFilesToStrings) {
  // multipart/form-data messages are delimited by <CRLF>--<boundary>. We want to be able to handle
  // omitted carriage returns, though, so our delimiter only matches against a preceding line feed.
  const auto delimiter = kj::str("\n--", boundary);

  // We want to slice off the delimiter's preceding newline for the initial search, because the very
  // first instance does not require one. In every subsequent multipart message, the preceding
  // newline is required.
  auto message = splitAtSubString(body, delimiter.slice(1));

  JSG_REQUIRE(
      body.size() > 0, TypeError, "No initial boundary string (or you have a truncated message).");

  const auto done = [](kj::ArrayPtr<const char>& body) {
    // Consume any (CR)LF characters that trailed the boundary and indicate continuation, or consume
    // the terminal "--" characters and indicate termination, or throw an error.
    if (startsWith(body, "\n"_kj)) {
      body = body.slice(1, body.size());
    } else if (startsWith(body, "\r\n"_kj)) {
      body = body.slice(2, body.size());
    } else if (startsWith(body, "--"_kj)) {
      // We're done!
      return true;
    } else {
      JSG_FAIL_REQUIRE(TypeError, "Boundary string was not succeeded by CRLF, LF, or '--'.");
    }
    return false;
  };

  constexpr auto staticRegexFlags =
      std::regex_constants::ECMAScript | std::regex_constants::optimize;

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

    kj::StringPtr disposition =
        JSG_REQUIRE_NONNULL(headers.get(formDataHeaderTable.contentDispositionId), TypeError,
            "No valid Content-Disposition header found in FormData part.");

    kj::Maybe<kj::String> maybeName;
    kj::Maybe<kj::String> filename;
    {
      p::IteratorInput<char, const char*> input(disposition.begin(), disposition.end());
      auto result = JSG_REQUIRE_NONNULL(contentDisposition(input), TypeError,
          "Invalid Content-Disposition header found in FormData part.");
      JSG_REQUIRE(kj::get<0>(result) == "form-data"_kj.asArray(), TypeError,
          "Content-Disposition header for FormData part must have the value \"form-data\", "
          "possibly followed by parameters. Got: \"",
          kj::get<0>(result), "\"");

      for (auto& param: kj::get<1>(result)) {
        if (kj::get<0>(param) == "name"_kj.asArray()) {
          maybeName = kj::str(kj::get<1>(param));
        } else if (kj::get<0>(param) == "filename"_kj.asArray()) {
          filename = kj::str(kj::get<1>(param));
        }
      }
    }

    kj::String name = JSG_REQUIRE_NONNULL(kj::mv(maybeName), TypeError,
        "Content-Disposition header in FormData part is missing a name.");

    kj::Maybe<kj::StringPtr> type = headers.get(kj::HttpHeaderId::CONTENT_TYPE);

    message = splitAtSubString(body, delimiter);
    JSG_REQUIRE(
        body.size() > 0, TypeError, "No subsequent boundary string after multipart message.");

    if (message.size() > 0) {
      // If we skipped a CR, we must avoid including it in the message data.
      message = message.slice(0, message.size() - uint(message.back() == '\r'));
    }

    if (filename == kj::none || convertFilesToStrings) {
      data.add(FormData::Entry{kj::mv(name), kj::str(message)});
    } else {
      auto bytes = kj::heapArray(message.asBytes());
      KJ_IF_SOME(lock, js) {
        data.add(FormData::Entry{kj::mv(name),
          jsg::alloc<File>(lock, kj::mv(bytes), KJ_ASSERT_NONNULL(kj::mv(filename)),
              kj::str(type.orDefault(nullptr)), dateNow())});
      } else {
        // This variation is used when we do not have an isolate lock. In this
        // case, the external memory held by the File is not tracked towards
        // the isolate's external memory.
        data.add(FormData::Entry{kj::mv(name),
          jsg::alloc<File>(kj::mv(bytes), KJ_ASSERT_NONNULL(kj::mv(filename)),
              kj::str(type.orDefault(nullptr)), dateNow())});
      }
    }
  }
}

kj::OneOf<jsg::Ref<File>, kj::String> blobToFile(jsg::Lock& js,
    kj::StringPtr name,
    kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
    jsg::Optional<kj::String> filename) {
  auto fromBlob = [&](jsg::Ref<Blob> blob) {
    kj::String fn;
    KJ_IF_SOME(f, filename) {
      fn = kj::mv(f);
    } else {
      fn = kj::str(name);
    }
    // The file is created with the same data as the blob (essentially as just
    // a view of the same blob) to avoid copying the data.
    return jsg::alloc<File>(
        blob.addRef(), blob->getData(), kj::mv(fn), kj::str(blob->getType()), dateNow());
  };

  KJ_SWITCH_ONEOF(value) {
    KJ_CASE_ONEOF(file, jsg::Ref<File>) {
      if (filename == kj::none) {
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

// Add the chars from `value` into `builder` escaping the characters '"' and '\n' using %
// encoding, exactly as Chrome does for Content-Disposition values.
void addEscapingQuotes(kj::Vector<char>& builder, kj::StringPtr value) {
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
          builder.addAll(MimeType::OCTET_STREAM.toString());
        } else {
          builder.addAll(type);
        }
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

FormData::EntryType FormData::clone(FormData::EntryType& value) {
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

void FormData::parse(kj::Maybe<jsg::Lock&> js,
    kj::ArrayPtr<const char> rawText,
    kj::StringPtr contentType,
    bool convertFilesToStrings) {
  KJ_IF_SOME(parsed, MimeType::tryParse(contentType)) {
    auto& params = parsed.params();
    if (MimeType::FORM_DATA == parsed) {
      auto& boundary = JSG_REQUIRE_NONNULL(params.find("boundary"_kj), TypeError,
          "No boundary string in Content-Type header. The multipart/form-data MIME "
          "type requires a boundary parameter, e.g. 'Content-Type: multipart/form-data; "
          "boundary=\"abcd\"'. See RFC 7578, section 4.");
      parseFormData(js, data, boundary, rawText, convertFilesToStrings);
      return;
    } else if (MimeType::FORM_URLENCODED == parsed) {
      // Let's read the charset so we can barf if the body isn't UTF-8.
      //
      // TODO(conform): Transcode to UTF-8, like the spec tells us to.
      KJ_IF_SOME(charsetParam, params.find("charset"_kj)) {
        auto charset = kj::str(charsetParam);
        JSG_REQUIRE(strcasecmp(charset.cStr(), "utf-8") == 0 ||
                strcasecmp(charset.cStr(), "utf8") == 0 ||
                strcasecmp(charset.cStr(), "unicode-1-1-utf-8") == 0,
            TypeError, "Non-utf-8 application/x-www-form-urlencoded body.");
      }
      kj::Vector<kj::Url::QueryParam> query;
      parseQueryString(query, kj::mv(rawText));
      data.reserve(query.size());
      for (auto& param: query) {
        data.add(Entry{kj::mv(param.name), kj::mv(param.value)});
      }
      return;
    }
  }
  JSG_FAIL_REQUIRE(TypeError,
      kj::str("Unrecognized Content-Type header value. FormData can only "
              "parse the following MIME types: ",
          MimeType::FORM_DATA.toString(), ", ", MimeType::FORM_URLENCODED.toString()));
}

jsg::Ref<FormData> FormData::constructor() {
  return jsg::alloc<FormData>();
}

void FormData::append(jsg::Lock& js,
    kj::String name,
    kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
    jsg::Optional<kj::String> filename) {
  auto filifiedValue = blobToFile(js, name, kj::mv(value), kj::mv(filename));
  data.add(Entry{kj::mv(name), kj::mv(filifiedValue)});
}

void FormData::delete_(kj::String name) {
  auto pivot =
      std::remove_if(data.begin(), data.end(), [&name](const auto& kv) { return kv.name == name; });
  data.truncate(pivot - data.begin());
}

kj::Maybe<kj::OneOf<jsg::Ref<File>, kj::String>> FormData::get(kj::String name) {
  for (auto& [k, v]: data) {
    if (k == name) {
      return clone(v);
    }
  }
  return kj::none;
}

kj::Array<kj::OneOf<jsg::Ref<File>, kj::String>> FormData::getAll(kj::String name) {
  kj::Vector<kj::OneOf<jsg::Ref<File>, kj::String>> result;
  for (auto& [k, v]: data) {
    if (k == name) {
      result.add(clone(v));
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

// Set the first element named `name` to `value`, then remove all the rest matching that name.
void FormData::set(jsg::Lock& js,
    kj::String name,
    kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>, kj::String> value,
    jsg::Optional<kj::String> filename) {
  const auto predicate = [name = name.slice(0)](const auto& kv) { return kv.name == name; };
  auto firstFound = std::find_if(data.begin(), data.end(), predicate);
  if (firstFound != data.end()) {
    firstFound->value = blobToFile(js, name, kj::mv(value), kj::mv(filename));
    auto pivot = std::remove_if(++firstFound, data.end(), predicate);
    data.truncate(pivot - data.begin());
  } else {
    append(js, kj::mv(name), kj::mv(value), kj::mv(filename));
  }
}

jsg::Ref<FormData::EntryIterator> FormData::entries(jsg::Lock&) {
  return jsg::alloc<EntryIterator>(IteratorState{JSG_THIS});
}

jsg::Ref<FormData::KeyIterator> FormData::keys(jsg::Lock&) {
  return jsg::alloc<KeyIterator>(IteratorState{JSG_THIS});
}

jsg::Ref<FormData::ValueIterator> FormData::values(jsg::Lock&) {
  return jsg::alloc<ValueIterator>(IteratorState{JSG_THIS});
}

void FormData::forEach(jsg::Lock& js,
    jsg::Function<void(EntryType, kj::StringPtr, jsg::Ref<FormData>)> callback,
    jsg::Optional<jsg::Value> thisArg) {
  // Here, if the thisArg is not passed, or is passed explicitly as a null or
  // undefined, then undefined is used as the thisArg.
  auto receiver = js.v8Undefined();
  KJ_IF_SOME(arg, thisArg) {
    auto handle = arg.getHandle(js);
    if (!handle->IsNullOrUndefined()) {
      receiver = handle;
    }
  }
  callback.setReceiver(js.v8Ref(receiver));

  // On each iteration of the for loop, a JavaScript callback is invokved. If a new
  // item is appended to the URLSearchParams within that function, the loop must pick
  // it up. Using the classic for (;;) syntax here allows for that. However, this does
  // mean that it's possible for a user to trigger an infinite loop here if new items
  // are added to the search params unconditionally on each iteration.
  for (size_t i = 0; i < this->data.size(); i++) {
    auto& [key, value] = this->data[i];
    callback(js, clone(value), key, JSG_THIS);
  }
}

}  // namespace workerd::api
