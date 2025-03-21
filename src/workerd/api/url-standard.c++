// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url-standard.h"

#include "blob.h"
#include "util.h"

#include <workerd/io/features.h>

#include <unicode/uchar.h>
#include <unicode/uidna.h>
#include <unicode/ustring.h>
#include <unicode/utf8.h>

#include <kj/array.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>

#if _WIN32
#include <ws2tcpip.h>
#undef RELATIVE
#else
#include <arpa/inet.h>
#endif

namespace workerd::api::url {

namespace {
jsg::Url parseImpl(kj::StringPtr url, kj::Maybe<kj::StringPtr> maybeBase) {
  return JSG_REQUIRE_NONNULL(jsg::Url::tryParse(url, maybeBase), TypeError, "Invalid URL string.");
}
}  // namespace

jsg::Ref<URL> URL::constructor(jsg::USVString url, jsg::Optional<jsg::USVString> base) {
  return jsg::alloc<URL>(kj::mv(url), base.map([](jsg::USVString& base) { return base.asPtr(); }));
}

URL::URL(kj::StringPtr url, kj::Maybe<kj::StringPtr> base): inner(parseImpl(url, base)) {}

URL::~URL() noexcept(false) {
  KJ_IF_SOME(searchParams, maybeSearchParams) {
    searchParams->maybeUrl = kj::none;
  }
}

bool URL::canParse(jsg::USVString url, jsg::Optional<jsg::USVString> maybeBase) {
  return jsg::Url::canParse(url, maybeBase.map([](jsg::USVString& str) { return str.asPtr(); }));
}

jsg::JsString URL::createObjectURL(
    jsg::Lock& js, kj::OneOf<jsg::Ref<File>, jsg::Ref<Blob>> object) {
  // TODO(soon): Implement this
  JSG_FAIL_REQUIRE(Error, "URL.createObjectURL() is not implemented"_kj);
}

void URL::revokeObjectURL(jsg::Lock& js, jsg::USVString object_url) {
  // TODO(soon): Implement this
  JSG_FAIL_REQUIRE(Error, "URL.revokeObjectURL() is not implemented"_kj);
}

jsg::Ref<URLSearchParams> URL::getSearchParams() {
  KJ_IF_SOME(searchParams, maybeSearchParams) {
    return searchParams.addRef();
  }
  return maybeSearchParams.emplace(jsg::alloc<URLSearchParams>(inner.getSearch(), *this)).addRef();
}

kj::Array<const char> URL::getOrigin() {
  return inner.getOrigin();
}

kj::ArrayPtr<const char> URL::getHref() {
  return inner.getHref();
}

void URL::setHref(jsg::Lock& js, jsg::USVString value) {
  // Href setter is the only place in URL parser that is allowed to throw except the constructor.
  if (!inner.setHref(value)) {
    auto context = jsg::TypeErrorContext::setterArgument(typeid(URL), "href");
    jsg::throwTypeError(js.v8Isolate, context, "valid URL string");
  }
  KJ_IF_SOME(searchParams, maybeSearchParams) {
    searchParams->reset();
  }
}

kj::ArrayPtr<const char> URL::getProtocol() {
  return inner.getProtocol();
}

void URL::setProtocol(jsg::USVString value) {
  inner.setProtocol(value);
}

kj::ArrayPtr<const char> URL::getUsername() {
  return inner.getUsername();
}

void URL::setUsername(jsg::USVString value) {
  inner.setUsername(value);
}

kj::ArrayPtr<const char> URL::getPassword() {
  return inner.getPassword();
}

void URL::setPassword(jsg::USVString value) {
  inner.setPassword(value);
}

kj::ArrayPtr<const char> URL::getHost() {
  return inner.getHost();
}

void URL::setHost(jsg::USVString value) {
  inner.setHost(value);
}

kj::ArrayPtr<const char> URL::getHostname() {
  return inner.getHostname();
}

void URL::setHostname(jsg::USVString value) {
  inner.setHostname(value);
}

kj::ArrayPtr<const char> URL::getPort() {
  return inner.getPort();
}

void URL::setPort(jsg::USVString value) {
  inner.setPort(kj::Maybe(value.asPtr()));
}

kj::ArrayPtr<const char> URL::getPathname() {
  return inner.getPathname();
}

void URL::setPathname(jsg::USVString value) {
  inner.setPathname(value);
}

kj::ArrayPtr<const char> URL::getSearch() {
  return inner.getSearch();
}

void URL::setSearch(jsg::USVString value) {
  inner.setSearch(kj::Maybe(value.asPtr()));
  KJ_IF_SOME(searchParams, maybeSearchParams) {
    searchParams->reset();
  }
}

kj::ArrayPtr<const char> URL::getHash() {
  return inner.getHash();
}

void URL::setHash(jsg::USVString hash) {
  inner.setHash(kj::Maybe(hash.asPtr()));
}

void URL::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(maybeSearchParams);
}

// ======================================================================================
namespace {
jsg::UrlSearchParams initSearchParams(const URLSearchParams::Initializer& init) {
  KJ_SWITCH_ONEOF(init) {
    KJ_CASE_ONEOF(pairs, URLSearchParams::StringPairs) {
      jsg::UrlSearchParams params;
      for (auto& item: pairs) {
        JSG_REQUIRE(item.size() == 2, TypeError, "Invalid URL search params sequence.");
        params.append(item[0], item[1]);
      }
      return kj::mv(params);
    }
    KJ_CASE_ONEOF(dict, jsg::Dict<jsg::USVString, jsg::USVString>) {
      kj::HashMap<kj::StringPtr, kj::StringPtr> paramsMap;
      for (auto& item: dict.fields) {
        paramsMap.upsert(item.name, item.value);
      }

      jsg::UrlSearchParams params;
      for (auto& item: paramsMap) {
        params.append(item.key, item.value);
      }
      return kj::mv(params);
    }
    KJ_CASE_ONEOF(str, jsg::USVString) {
      return JSG_REQUIRE_NONNULL(
          jsg::UrlSearchParams::tryParse(str), TypeError, "Invalid URL search params string.");
    }
  }
  KJ_UNREACHABLE;
}

jsg::UrlSearchParams initFromSearch(kj::Maybe<kj::ArrayPtr<const char>>& maybeQuery) {
  KJ_IF_SOME(query, maybeQuery) {
    return JSG_REQUIRE_NONNULL(
        jsg::UrlSearchParams::tryParse(query), TypeError, "Invalid URL search params string.");
  }
  return jsg::UrlSearchParams();
}
}  // namespace

jsg::Ref<URLSearchParams> URLSearchParams::constructor(jsg::Optional<Initializer> init) {
  return jsg::alloc<URLSearchParams>(kj::mv(init).orDefault(jsg::USVString()));
}

URLSearchParams::URLSearchParams(Initializer initializer): inner(initSearchParams(initializer)) {}

URLSearchParams::URLSearchParams(kj::Maybe<kj::ArrayPtr<const char>> maybeQuery, URL& url)
    : inner(initFromSearch(maybeQuery)),
      maybeUrl(url) {}

uint URLSearchParams::getSize() {
  return inner.size();
}

void URLSearchParams::update() {
  KJ_IF_SOME(url, maybeUrl) {
    auto serialized = toString();
    url.inner.setSearch(kj::Maybe(serialized.asPtr()));
  }
}

void URLSearchParams::reset() {
  KJ_IF_SOME(url, maybeUrl) {
    auto search = kj::Maybe(url.inner.getSearch());
    inner.reset(search);
  }
}

void URLSearchParams::append(jsg::USVString name, jsg::USVString value) {
  inner.append(name, value);
  update();
}

void URLSearchParams::delete_(
    jsg::Lock& js, jsg::USVString name, jsg::Optional<jsg::USVString> value) {
  KJ_ON_SCOPE_SUCCESS(update());
  if (FeatureFlags::get(js).getUrlSearchParamsDeleteHasValueArg()) {
    // The whatwg url spec was updated to add a second optional argument to delete()
    // and has(). While it was determined that it likely didn't break browser users,
    // the change could break at least a couple existing deployed workers so we have
    // to gate support behind a compat flag.
    KJ_IF_SOME(v, value) {
      inner.delete_(name, kj::Maybe(v.asPtr()));
      return;
    }
  }
  inner.delete_(name);
}

kj::Maybe<kj::ArrayPtr<const char>> URLSearchParams::get(jsg::USVString name) {
  return inner.get(name);
}

kj::Array<kj::ArrayPtr<const char>> URLSearchParams::getAll(jsg::USVString name) {
  return inner.getAll(name);
}

bool URLSearchParams::has(jsg::Lock& js, jsg::USVString name, jsg::Optional<jsg::USVString> value) {
  if (FeatureFlags::get(js).getUrlSearchParamsDeleteHasValueArg()) {
    // The whatwg url spec was updated to add a second optional argument to delete()
    // and has(). While it was determined that it likely didn't break browser users,
    // the change could break at least a couple existing deployed workers so we have
    // to gate support behind a compat flag.
    KJ_IF_SOME(v, value) {
      return inner.has(name, kj::Maybe(v.asPtr()));
    }
  }
  return inner.has(name);
}

void URLSearchParams::set(jsg::USVString name, jsg::USVString value) {
  inner.set(name, value);
  update();
}

void URLSearchParams::sort() {
  inner.sort();
  update();
}

jsg::Ref<URLSearchParams::EntryIterator> URLSearchParams::entries(jsg::Lock&) {
  return jsg::alloc<URLSearchParams::EntryIterator>(
      IteratorState<jsg::UrlSearchParams::EntryIterator>(JSG_THIS, inner.getEntries()));
}

jsg::Ref<URLSearchParams::KeyIterator> URLSearchParams::keys(jsg::Lock&) {
  return jsg::alloc<URLSearchParams::KeyIterator>(
      IteratorState<jsg::UrlSearchParams::KeyIterator>(JSG_THIS, inner.getKeys()));
}

jsg::Ref<URLSearchParams::ValueIterator> URLSearchParams::values(jsg::Lock&) {
  return jsg::alloc<URLSearchParams::ValueIterator>(
      IteratorState<jsg::UrlSearchParams::ValueIterator>(JSG_THIS, inner.getValues()));
}

kj::Maybe<kj::Array<kj::ArrayPtr<const char>>> URLSearchParams::entryIteratorNext(
    jsg::Lock& js, URLSearchParams::IteratorState<jsg::UrlSearchParams::EntryIterator>& state) {
  return state.inner.next().map([](const jsg::UrlSearchParams::EntryIterator::Entry& entry) {
    return kj::arr(entry.key, entry.value);
  });
}

kj::Maybe<kj::ArrayPtr<const char>> URLSearchParams::keyIteratorNext(
    jsg::Lock& js, URLSearchParams::IteratorState<jsg::UrlSearchParams::KeyIterator>& state) {
  return state.inner.next();
}

kj::Maybe<kj::ArrayPtr<const char>> URLSearchParams::valueIteratorNext(
    jsg::Lock& js, URLSearchParams::IteratorState<jsg::UrlSearchParams::ValueIterator>& state) {
  return state.inner.next();
}

void URLSearchParams::forEach(jsg::Lock& js,
    jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<URLSearchParams>)> callback,
    jsg::Optional<jsg::JsValue> thisArg) {
  auto receiver = js.undefined();
  KJ_IF_SOME(arg, thisArg) {
    if (!arg.isNullOrUndefined()) {
      receiver = arg;
    }
  }
  callback.setReceiver(js.v8Ref<v8::Value>(receiver));

  auto entries = inner.getEntries();
  while (entries.hasNext()) {
    auto next = KJ_ASSERT_NONNULL(entries.next());
    kj::String value(
        const_cast<char*>(next.value.begin()), next.value.size(), kj::NullArrayDisposer::instance);
    kj::String key(
        const_cast<char*>(next.key.begin()), next.key.size(), kj::NullArrayDisposer::instance);
    callback(js, value, key, JSG_THIS);
  }
}

kj::String URLSearchParams::toString() {
  return kj::str(inner);
}

}  // namespace workerd::api::url
