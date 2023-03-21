// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "util.h"
#include <kj/encoding.h>
#include <workerd/io/io-context.h>
#include <workerd/util/thread-scopes.h>

namespace workerd::api {

jsg::ByteString toLower(kj::StringPtr str) {
  auto buf = kj::heapArray<char>(str.size() + 1);
  for (auto i: kj::indices(str)) {
    char c = str[i];
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
    buf[i] = c;
  }
  buf.back() = '\0';
  return jsg::ByteString(kj::mv(buf));
}

namespace {

kj::ArrayPtr<const char> split(kj::ArrayPtr<const char>& text, char c) {
  // TODO(cleanup): Modified version of split() found in kj/compat/url.c++.

  for (auto i: kj::indices(text)) {
    if (text[i] == c) {
      kj::ArrayPtr<const char> result = text.slice(0, i);
      text = text.slice(i + 1, text.size());
      return result;
    }
  }
  auto result = text;
  text = {};
  return result;
}

}  // namespace

kj::String toLower(kj::String&& str) {
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

kj::String toUpper(kj::String&& str) {
  for (char& c: str) {
    if ('a' <= c && c <= 'z') c -= 'a' - 'A';
  }
  return kj::mv(str);
}

void parseQueryString(kj::Vector<kj::Url::QueryParam>& query, kj::ArrayPtr<const char> text,
                      bool skipLeadingQuestionMark) {
  if (skipLeadingQuestionMark && text.size() > 0 && text[0] == '?') {
    text = text.slice(1, text.size());
  }

  while (text.size() > 0) {
    auto value = split(text, '&');
    if (value.size() == 0) continue;
    auto name = split(value, '=');
    query.add(kj::Url::QueryParam { kj::decodeWwwForm(name), kj::decodeWwwForm(value) });
  }
}

kj::Maybe<kj::ArrayPtr<const char>> readContentTypeParameter(kj::StringPtr contentType,
                                                             kj::StringPtr param) {
  KJ_IF_MAYBE(semiColon, contentType.findFirst(';')) {
    // Get to the parameters
    contentType = contentType.slice(*semiColon + 1);

    // The attribute name of a MIME type parameter is always case-insensitive. See definition of
    // the attribute production rule in https://tools.ietf.org/html/rfc2045#page-29
    auto lowerParam = toLower(kj::str(param));

    kj::StringPtr leftover = contentType;
    while(true) {
      while (leftover.startsWith(" ") || leftover.startsWith(";")) {
        leftover = leftover.slice(1);
      }

      KJ_IF_MAYBE(equal, leftover.findFirst('=')) {
        // Handle parameter
        auto name = toLower(kj::str(leftover.slice(0, *equal)));
        auto valueStart = *equal + 1;
        kj::ArrayPtr<const char> value = nullptr;

        if (leftover[valueStart] == '"') {
          // parameter value surrounded by quotes
          auto pos = 0;
          auto valueStr = leftover.slice(valueStart + 1);

          while(pos < valueStr.size()) {
            if (valueStr[pos] == '\\') {
              pos++;
            } else if (valueStr[pos] == '"') {
              break;
            }
            pos++;
          }

          if (pos >= valueStr.size()) {
            // invalid value, no closing "
            break;
          }

          value = leftover.slice(valueStart + 1, valueStart + 1 + pos);
          // skip name, =, value and quotes
          leftover = leftover.slice(name.size() + 1 + value.size() + 2);

        } else {
          // parameter value with no quotes, just glob until the next ;
          KJ_IF_MAYBE(valueEnd, leftover.slice(valueStart).findFirst(';')) {
            value = leftover.slice(valueStart, valueStart + *valueEnd);
            leftover = leftover.slice(valueStart + *valueEnd + 1);
          } else {
            // there's nothing else
            value = leftover.slice(valueStart);
            leftover = leftover.slice(leftover.size());
          }

          // since there are no quotes, remove spurious whitespace at the end
          while(value.size() > 0 && value[value.size() - 1] == ' ') {
            value = value.slice(0, value.size() - 1);
          }
        }

        // have we got it?
        if (name == lowerParam && value.size() > 0) {
          return value;
        }
      } else {
        // skip to next parameter
        KJ_IF_MAYBE(nextParam, leftover.findFirst(';')) {
          leftover = leftover.slice(*nextParam + 1);
        } else {
          // we are done and we didn't find the parameter
          break;
        }
      }
    }
  }

  return nullptr;
}

kj::Maybe<kj::Exception> translateKjException(const kj::Exception& exception,
    std::initializer_list<ErrorTranslation> translations) {
  for (auto& t: translations) {
    if (strstr(exception.getDescription().cStr(), t.kjDescription.cStr()) != nullptr) {
      return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
          kj::str(JSG_EXCEPTION(TypeError) ": ", t.jsDescription));
    }
  }

  return nullptr;
}

namespace {

template <typename Func>
auto translateTeeErrors(Func&& f) -> decltype(kj::fwd<Func>(f)()) {
  return kj::evalNow(kj::fwd<Func>(f))
      .catch_([](kj::Exception&& exception) -> decltype(kj::fwd<Func>(f)()) {
    KJ_IF_MAYBE(e, translateKjException(exception, {
      { "tee buffer size limit exceeded"_kj,
        "ReadableStream.tee() buffer limit exceeded. This error usually occurs when a Request or "
        "Response with a large body is cloned, then only one of the clones is read, forcing "
        "the Workers runtime to buffer the entire body in memory. To fix this issue, remove "
        "unnecessary calls to Request/Response.clone() and ReadableStream.tee(), and always read "
        "clones/tees in parallel."_kj },
    })) {
      return kj::mv(*e);
    }

    return kj::mv(exception);
  });
}

}

kj::Own<kj::AsyncInputStream> newTeeErrorAdapter(kj::Own<kj::AsyncInputStream> inner) {
  class Adapter final: public kj::AsyncInputStream {
  public:
    explicit Adapter(kj::Own<AsyncInputStream> inner): inner(kj::mv(inner)) {}

    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return translateTeeErrors([&] {
        return inner->tryRead(buffer, minBytes, maxBytes);
      });
    }

    kj::Maybe<uint64_t> tryGetLength() override { return inner->tryGetLength(); };

    kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
      return translateTeeErrors([&] {
        return inner->pumpTo(output, amount);
      });
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
    bool probablyBase64Id = (span.size() >= 21 &&
                             digitCount >= 2 &&
                             upperCount >= 2 &&
                             lowerCount >= 2);

    if (isHexId || probablyBase64Id) {
      redacted.addAll("REDACTED"_kj);
    } else {
      redacted.addAll(span);
    }
  };

  for (const char& c: url) {
    bool isUpper = ('A' <= c && c <= 'Z');
    bool isLower = ('a' <= c && c <= 'z');
    bool isDigit = ('0' <= c && c <= '9');
    bool isHexDigit = (isDigit || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'));
    bool isSep = (c == '+' || c == '-' || c == '_');
    // These extra characters are used in the regular and url-safe versions of
    // base64, but might also be used for GUID-style separators in hex ids.
    // Regular base64 also includes '/', which we don't try to match here due
    // to its prevalence in URLs.  Likewise, we ignore the base64 "=" padding
    // character.

    if (isUpper || isLower || isDigit || isSep) {
      if (isHexDigit) { hexDigitCount++; }
      if (!isHexDigit && !isSep) { sawNonHexChar = true; }
      if (isUpper) { upperCount++; }
      if (isLower) { lowerCount++; }
      if (isDigit) { digitCount++; }
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

double dateNow() {
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    return (ioContext.now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }

  return 0.0;
}

kj::Maybe<jsg::V8Ref<v8::Object>> cloneRequestCf(
    jsg::Lock& js,
    kj::Maybe<jsg::V8Ref<v8::Object>> maybeCf) {
  KJ_IF_MAYBE(cf, maybeCf) {
    // In case the cf object has a logging proxy, we want to make sure
    // the logging is not triggered here when the object is cloned.
    NoRequestCfProxyLoggingScope noLoggingScope;
    auto cloned = cf->deepClone(js);
    maybeWrapBotManagement(js.v8Isolate, cloned.getHandle(js));
    return kj::mv(cloned);
  }
  return nullptr;
}

void maybeWrapBotManagement(v8::Isolate* isolate, v8::Local<v8::Object> handle) {
  auto context = isolate->GetCurrentContext();
  auto botManagement = jsg::v8StrIntern(isolate, "botManagement");
  v8::Local<v8::Value> maybeBotManagement = jsg::check(handle->Get(context, botManagement));
  // If the botManagement field exists, we replace its value here with a Proxy object that
  // will log the first time any of its properties are accessed. Logging will occur only
  // once per worker instance.
  //
  // Replacing the value with a proxy rather than setting an accessor for botManagement on
  // the request.cf object itself avoids false positives when someone is simply iterating
  // over the fields of request.cf without actually using them. It also allows us to avoid
  // having to create accessors or a class to intercept every individual property on the
  // botManagement object.
  if (maybeBotManagement->IsObject() && !maybeBotManagement->IsProxy()) {
    v8::Local<v8::Object> bmObj = maybeBotManagement.As<v8::Object>();
    auto& js = jsg::Lock::from(isolate);

    // Create the Proxy handler exactly once per global context and cache it using a private
    // property on the global itself. The handler itself maintains no state so it is safe to
    // use it over and over for all requests.
    auto sym = v8::Private::ForApi(isolate, jsg::v8StrIntern(isolate, "loggingProxyHandler"));
    auto handler = jsg::check(context->Global()->GetPrivate(context, sym));
    if (handler->IsUndefined()) {
      handler = v8::Object::New(isolate);
      jsg::check(handler.As<v8::Object>()->Set(context, jsg::v8StrIntern(isolate, "get"),
          js.wrapReturningFunction(context,
              [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
        if (IoContext::hasCurrent()) {
          if (!NoRequestCfProxyLoggingScope::isActive()) {
            IoContext::current().getMetrics().logBotManagementUse();
          }
        } else {
          // TODO(later): There is an edge case where the request.cf could be set to a global
          // scope variable and read outside of an IoContext. In such cases we would not get the
          // logging. Is that ok? It should be rare but in theory it means the logging won't be
          // 100% effective at identifying all uses.
        }
        if (args[0]->IsObject()) {
          return jsg::check(args[0].As<v8::Object>()->Get(
              js.v8Isolate->GetCurrentContext(), args[1]));
        } else {
          return js.v8Undefined();
        }
      })));
      context->Global()->SetPrivate(context, sym, handler);
    }

    jsg::check(handle->Set(context, botManagement,
        jsg::check(v8::Proxy::New(context, bmObj, handler.As<v8::Object>()))));
  }
}

}  // namespace workerd::api
