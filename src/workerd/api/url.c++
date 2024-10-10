// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "url.h"

#include "util.h"

#include <kj/encoding.h>
#include <kj/parse/char.h>
#include <kj/string-tree.h>

#include <algorithm>
#include <map>
#include <set>

namespace workerd::api {

namespace {

// Helper functions for the origin, pathname, and search getters and setters.

// The folowing two lists needs to be kept in sync since the length and the order
// of them are needed to properly calculate the hash/index.
constexpr kj::StringPtr is_special_list[] = {
  "http"_kj, " "_kj, "https"_kj, "ws"_kj, "ftp"_kj, "wss"_kj, "file"_kj, " "_kj};
constexpr kj::StringPtr special_ports[] = {
  "80"_kj, ""_kj, "443"_kj, "80"_kj, "21"_kj, "443"_kj, ""_kj, ""_kj};

// Taken from Ada URL library.
// Ref: https://github.com/ada-url/ada/blob/b431670699cf4f3ebb2e2c394c23a89850bb6f3f/include/ada/scheme-inl.h#L49
bool isSpecialScheme(kj::StringPtr scheme) noexcept {
  if (scheme.size() == 0) {
    return false;
  }
  // Depending on the first character and the size of the input, this line calculates
  // the index from the list above.
  //
  // Generate a simple hash value that will always be between 0 and 7 (inclusive),
  // regardless of the input. This is because the bitwise AND with 7 ensures that
  // only the last 3 bits of the result are kept.
  int hash_value = (2 * scheme.size() + static_cast<unsigned>(scheme[0])) & 7;
  const auto target = is_special_list[hash_value];
  return (target[0] == scheme[0]) && (target.slice(1) == scheme.slice(1));
}

// Taken from Ada URL library.
// Ref: https://github.com/ada-url/ada/blob/b431670699cf4f3ebb2e2c394c23a89850bb6f3f/include/ada/scheme-inl.h#L57
kj::Maybe<kj::StringPtr> defaultPortForScheme(kj::StringPtr scheme) noexcept {
  if (scheme.size() == 0) {
    return kj::none;
  }
  int hash_value = (2 * scheme.size() + static_cast<unsigned>(scheme[0])) & 7;
  const auto target = is_special_list[hash_value];
  if ((target[0] == scheme[0]) && (target.slice(1) == scheme.slice(1))) {
    auto port = special_ports[hash_value];
    if (port.size() == 0) {
      return kj::none;
    }
    return port;
  }

  return kj::none;
}

void normalizePort(kj::Url& url) {
  // Remove trailing ':', and remove ':xxx' if xxx is the scheme-default port.

  KJ_IF_SOME(colon, url.host.findFirst(':')) {
    if (url.host.size() == colon + 1) {
      // Remove trailing ':'.
      url.host = kj::str(url.host.first(colon));
    } else KJ_IF_SOME(defaultPort, defaultPortForScheme(url.scheme)) {
      if (defaultPort == url.host.slice(colon + 1)) {
        // Remove scheme-default port.
        url.host = kj::str(url.host.first(colon));
      }
    }
  }
}

kj::Maybe<kj::ArrayPtr<const char>> trySplit(kj::ArrayPtr<const char>& text, char c) {
  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  for (auto i: kj::indices(text)) {
    if (text[i] == c) {
      kj::ArrayPtr<const char> result = text.first(i);
      text = text.slice(i + 1, text.size());
      return result;
    }
  }
  return kj::none;
}

kj::ArrayPtr<const char> split(kj::StringPtr& text, const kj::parse::CharGroup_& chars) {
  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  for (auto i: kj::indices(text)) {
    if (chars.contains(text[i])) {
      kj::ArrayPtr<const char> result = text.first(i);
      text = text.slice(i);
      return result;
    }
  }
  auto result = text.asArray();
  text = "";
  return result;
}

kj::String percentDecode(kj::ArrayPtr<const char> text, bool& hadErrors) {
  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  auto result = kj::decodeUriComponent(text);
  if (result.hadErrors) hadErrors = true;
  return kj::mv(result);
}

kj::String percentDecodeQuery(kj::ArrayPtr<const char> text, bool& hadErrors) {
  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  auto result = kj::decodeWwwForm(text);
  if (result.hadErrors) hadErrors = true;
  return kj::mv(result);
}

// Use this instead of calling kj::Url::toString() directly.
kj::String kjUrlToString(const kj::Url& url) {
  kj::String result;
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
    result = url.toString();
    // TODO(soon): This stringifier does not append trailing slashes to the pathname conformantly.
    //   For example, this equality currently does not hold true:
    //
    //     new URL('https://capnproto.org?query').href === 'https://capnproto.org/?query'
    //
    //   Fixing this bug would enable a plurality of the W3C test cases which currently fail. I.e.,
    //   it's the lowest hanging fruit. ;)
  })) {
    // TODO(conform): toString() really shouldn't be throwing anything, because it shouldn't be
    //   possible to get the URL object in a state where it has any invalid component. However, a
    //   variety of bugs conspire to make it possible (notably, EW-962 and EW-1731), and we're stuck
    //   with the situation for now. Rather than expose these errors to the user as opaque internal
    //   errors (and nag us via Sentry), we get our hands dirty with some string matching, in the
    //   hopes of helping users work around the bugs.
    KJ_IF_SOME(e,
        translateKjException(exception,
            {
              {"invalid hostname when stringifying URL"_kj,
                "Invalid hostname when stringifying URL."_kj},
              {"invalid name in URL path"_kj, "Invalid pathname when stringifying URL."_kj},
            })) {
      kj::throwFatalException(kj::mv(e));
    }

    // This is either an error we should know about and expect, or an "internal error". Either way,
    // squawk about it.
    KJ_LOG(ERROR, exception);
    JSG_FAIL_REQUIRE(TypeError, "Error stringifying URL.");
  }

  return kj::mv(result);
}

}  // namespace

// =======================================================================================
// URL

jsg::Ref<URL> URL::constructor(kj::String url, jsg::Optional<kj::String> base) {
  KJ_IF_SOME(b, base) {
    auto baseUrl =
        JSG_REQUIRE_NONNULL(kj::Url::tryParse(kj::mv(b)), TypeError, "Invalid base URL string.");
    return jsg::alloc<URL>(JSG_REQUIRE_NONNULL(
        baseUrl.tryParseRelative(kj::mv(url)), TypeError, "Invalid relative URL string."));
  }
  return jsg::alloc<URL>(
      JSG_REQUIRE_NONNULL(kj::Url::tryParse(kj::mv(url)), TypeError, "Invalid URL string."));
}

URL::URL(kj::Url&& u): url(kj::refcounted<RefcountedUrl>(kj::mv(u))) {
  normalizePort(*url);
}

// Setters and Getters
//
// When possible, getters just pull out the corresponding attribute from kj::Url and return it.
// Sometimes we need to modify the output a bit, e.g. to get the hostname and port separately.
//
// Setters need to set and validate new input. To accomplish this without reimplementing validation
// code that ought to live in kj::Url, I have implemented setters using the following general
// strategy:
//
// 1. Pre-process the input, if necessary. E.g., we drop anything after a ':' when setting protocol.
// 2. Clone the kj::Url object.
// 3. Replace the cloned component in question with the new value.
// 4. Stringify and parse the clone. If this succeeds, the clone is the new kj::Url object.
//
// Notably, we do little to no validation in this wrapper class. As validation checks are added to
// kj::Url's parser, more and more unit tests for this wrapper class should start passing without
// modification.
//
// TODO(perf): Pre-processing input, cloning, stringifying, and parsing the cloned URL is an awfully
//   heavyweight operation when all we want to do is validly replace a URL component. A couple
//   attributes, pathname and search, are able to take advantage of the kj::Url parser's context
//   argument: we can parse a pathname using the HTTP_REQUEST context, for instance. The WHATWG URL
//   spec defines a parser state machine allowing for the state to be overridden to parse only
//   specific components of a URL. This is more or less a generalization of kj::Url's parser
//   context, and offers an obvious path forward to both conformance and performance.

kj::String URL::getHref() {
  return toString();
}
void URL::setHref(jsg::Lock& js, kj::String value) {
  KJ_IF_SOME(u, kj::Url::tryParse(kj::mv(value))) {
    url->kj::Url::operator=(kj::mv(u));
  } else {
    auto context = jsg::TypeErrorContext::setterArgument(typeid(URL), "href");
    jsg::throwTypeError(js.v8Isolate, context, "valid URL string");
    // href's is the only setter which is allowed to throw on invalid input, according to the spec.
  }
}

kj::String URL::getOrigin() {
  // TODO(cleanup): Move this logic into kj::Url.

  if (isSpecialScheme(url->scheme) && url->scheme != "file") {
    return kj::str(url->scheme, "://", url->host);
  } else if (url->scheme == "file") {
    return kj::str("null");
  } else if (url->scheme == "blob") {
    // TODO(soon): Parse url->path[0] and return that if it has an origin.
    return kj::str("null");
  }
  return kj::str("null");
}

kj::String URL::getProtocol() {
  return kj::str(url->scheme, ':');
}
void URL::setProtocol(kj::String value) {
  KJ_IF_SOME(colon, value.findFirst(':')) {
    value = kj::str(value.first(colon));
  }

  auto copy = url->clone();
  copy.scheme = kj::mv(value);

  KJ_IF_SOME(u, kj::Url::tryParse(kjUrlToString(copy))) {
    url->kj::Url::operator=(kj::mv(u));
  }

  normalizePort(*url);
}

kj::String URL::getUsername() {
  KJ_IF_SOME(userInfo, url->userInfo) {
    return kj::encodeUriUserInfo(userInfo.username);
  }
  return {};
}
void URL::setUsername(kj::String value) {
  auto copy = url->clone();
  KJ_IF_SOME(ui, copy.userInfo) {
    ui.username = kj::mv(value);
  } else {
    copy.userInfo = kj::Url::UserInfo{.username = kj::mv(value)};
  }

  KJ_IF_SOME(u, kj::Url::tryParse(kjUrlToString(copy))) {
    url->kj::Url::operator=(kj::mv(u));
  }
}

kj::String URL::getPassword() {
  KJ_IF_SOME(userInfo, url->userInfo) {
    KJ_IF_SOME(password, userInfo.password) {
      return kj::encodeUriUserInfo(password);
    }
  }
  return {};
}
void URL::setPassword(kj::String value) {
  auto copy = url->clone();
  KJ_IF_SOME(ui, copy.userInfo) {
    // We already have userInfo. Set the password if we were given a non-empty string, otherwise
    // reset the password Maybe.
    if (value.size() > 0) {
      ui.password = kj::mv(value);
    } else {
      ui.password = kj::none;
    }
  } else if (value.size() > 0) {
    copy.userInfo = kj::Url::UserInfo{.password = kj::mv(value)};
  }

  KJ_IF_SOME(u, kj::Url::tryParse(kjUrlToString(copy))) {
    url->kj::Url::operator=(kj::mv(u));
  }
}

kj::String URL::getHost() {
  return kj::str(url->host);
}
void URL::setHost(kj::String value) {
  // The spec provides the following helpful note:
  //
  //   If the given value for the host attribute’s setter lacks a port, context object’s url’s port
  //   will not change. This can be unexpected as host attribute’s getter does return a URL-port
  //   string so one might have assumed the setter to always "reset" both.

  // If the new host value lacks a port, copy the current one over to the new value, if any. We can
  // assume that if the current one has a port, it must not be the default port for this URL's
  // scheme.
  KJ_IF_SOME(colon, url->host.findFirst(':')) {
    KJ_IF_SOME(newHostColon, value.findFirst(':')) {
      if (value.size() == newHostColon + 1) {
        // The new host has a colon but nothing after it. Adopt the current port.
        value = kj::str(kj::mv(value), url->host.slice(colon + 1));
      } else {
        // The new host has a port, so we don't copy the current one over.
      }
    } else {
      // The new host has no port. Adopt the current port.
      value = kj::str(kj::mv(value), url->host.slice(colon));
    }
  }

  // TODO(soon): Validate the new host string. kj::Url::isValidHost(value)?
  url->host = kj::mv(value);

  normalizePort(*url);
}

kj::String URL::getHostname() {
  KJ_IF_SOME(colon, url->host.findFirst(':')) {
    return kj::str(url->host.first(colon));
  }
  return kj::str(url->host);
}
void URL::setHostname(kj::String value) {
  // In contrast to the host setter, the hostname setter explicitly ignores any new port. We take
  // the hostname from the new value and the port from the old value.
  auto hostnameString = value.first(value.findFirst(':').orDefault(value.size()));
  auto portString = url->host.slice(url->host.findFirst(':').orDefault(url->host.size()));

  url->host = kj::str(hostnameString, portString);
}

kj::String URL::getPort() {
  KJ_IF_SOME(colon, url->host.findFirst(':')) {
    return kj::str(url->host.slice(colon + 1));
  }
  return {};
}
void URL::setPort(kj::String value) {
  KJ_IF_SOME(colon, url->host.findFirst(':')) {
    // Our url's host already has a port. Replace it.
    value = kj::str(url->host.first(colon + 1), kj::mv(value));
  } else {
    value = kj::str(url->host, ':', kj::mv(value));
  }

  url->host = kj::mv(value);

  normalizePort(*url);
}

kj::String URL::getPathname() {
  if (url->path.size() > 0) {
    auto components =
        KJ_MAP(component, url->path) { return kj::str('/', kj::encodeUriPath(component)); };
    return kj::str(kj::strArray(components, ""), url->hasTrailingSlash ? "/" : "");
  } else if (url->hasTrailingSlash || isSpecialScheme(url->scheme)) {
    // Special URLs have non-empty paths by definition, regardless of the value of hasTrailingSlash.
    return kj::str('/');
  }
  return {};
}
void URL::setPathname(kj::String value) {
  decltype(url->path) newPath;
  bool newHasTrailingSlash;

  auto text = value.slice(0);
  bool err = false;

  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  auto addPart = [&]() {
    // We only look for / to end path components in this setter, not ? and # like in
    // kj::Url::tryParse().
    constexpr auto END_PATH_PART = kj::parse::anyOfChars("/");
    auto part = split(text, END_PATH_PART);
    if (part.size() == 2 && part[0] == '.' && part[1] == '.') {
      if (newPath.size() != 0) {
        newPath.removeLast();
      }
      newHasTrailingSlash = true;
    } else if (part.size() == 0 || (part.size() == 1 && part[0] == '.')) {
      // Collapse consecutive slashes and "/./".
      newHasTrailingSlash = true;
    } else {
      newPath.add(percentDecode(part, err));
      newHasTrailingSlash = false;
    }
  };

  // Unlike kj::Url::tryParse(), the pathname being set doesn't have to begin with a slash.
  if (!text.startsWith("/")) {
    addPart();
  }

  while (text.startsWith("/")) {
    text = text.slice(1);
    addPart();
  }

  if (!err) {
    url->hasTrailingSlash = newHasTrailingSlash;
    url->path = newPath.releaseAsArray();
  }
}

kj::String URL::getSearch() {
  auto query = KJ_MAP(q, url->query) {
    // TODO(soon): We shouldn't be performing any encoding here, because our setSearch() (and URL
    //   constructor) shouldn't be performing application/x-www-form-urlencoded decoding on the
    //   query string themselves -- that's for URLSearchParams to do.
    if (q.value.begin() != nullptr) {
      return kj::str(kj::encodeWwwForm(q.name), '=', kj::encodeWwwForm(q.value));
    }
    return kj::str(kj::encodeWwwForm(q.name));
  };

  if (query.size() > 0) {
    return kj::str('?', kj::strArray(query, "&"));
  }
  return {};
}
void URL::setSearch(kj::String value) {
  decltype(url->query) newQuery;

  auto text = value.slice(value.startsWith("?") ? 1 : 0);
  bool err = false;

  // TODO(cleanup): Code duplication with kj/compat/url.c++.

  for (;;) {
    // We only look for & to end path components in this setter, not # like in kj::Url::tryParse().
    constexpr auto END_QUERY_PART = kj::parse::anyOfChars("&");
    auto part = split(text, END_QUERY_PART);

    if (part.size() > 0) {
      // TODO(soon): We shouldn't be performing any decoding here. Rather, the spec dictates that we
      //   should actually be percent-*encoding* with a very specific character set. Note that this
      //   also applies to URL's constructor as well.
      //
      //   See step 1.3.1 of https://url.spec.whatwg.org/#query-state
      KJ_IF_SOME(key, trySplit(part, '=')) {
        newQuery.add(
            kj::Url::QueryParam{percentDecodeQuery(key, err), percentDecodeQuery(part, err)});
      } else {
        newQuery.add(kj::Url::QueryParam{percentDecodeQuery(part, err), nullptr});
      }
    }

    if (!text.startsWith("&")) break;
    text = text.slice(1);
  }

  if (!err) {
    url->query = newQuery.releaseAsArray();
  }
}

jsg::Ref<URLSearchParams> URL::getSearchParams() {
  KJ_IF_SOME(usp, searchParams) {
    return usp.addRef();
  } else {
    searchParams.emplace(jsg::alloc<URLSearchParams>(kj::addRef(*url)));
    return KJ_ASSERT_NONNULL(searchParams).addRef();
  }
}

kj::String URL::getHash() {
  KJ_IF_SOME(fragment, url->fragment) {
    if (fragment.size() > 0) {
      return kj::str('#', kj::encodeUriFragment(fragment));
    }
  }
  return {};
}
void URL::setHash(kj::String value) {
  // Omit any starting '#'.
  url->fragment = kj::decodeUriComponent(value.slice(value.startsWith("#") ? 1 : 0));
}

kj::String URL::toString() {
  return kjUrlToString(*url);
}
kj::String URL::toJSON() {
  return toString();
}

// =======================================================================================
// URLSearchParams

URLSearchParams::URLSearchParams(kj::Own<URL::RefcountedUrl> url): url(kj::mv(url)) {}

jsg::Ref<URLSearchParams> URLSearchParams::constructor(
    jsg::Optional<URLSearchParams::Initializer> init) {
  auto searchParams = jsg::alloc<URLSearchParams>(kj::refcounted<URL::RefcountedUrl>());

  KJ_IF_SOME(i, init) {
    KJ_SWITCH_ONEOF(i) {
      KJ_CASE_ONEOF(usp, jsg::Ref<URLSearchParams>) {
        searchParams->url->kj::Url::operator=(usp->url->clone());
      }
      KJ_CASE_ONEOF(queryString, kj::String) {
        parseQueryString(searchParams->url->query, kj::mv(queryString), true);
      }
      KJ_CASE_ONEOF(dict, jsg::Dict<kj::String>) {
        searchParams->url->query = KJ_MAP(entry, dict.fields) {
          return kj::Url::QueryParam{kj::mv(entry.name), kj::mv(entry.value)};
        };
      }
      KJ_CASE_ONEOF(arrayOfArrays, kj::Array<kj::Array<kj::String>>) {
        searchParams->url->query = KJ_MAP(entry, arrayOfArrays) {
          JSG_REQUIRE(entry.size() == 2, TypeError,
              "To initialize a URLSearchParams object "
              "from an array-of-arrays, each inner array must have exactly two elements.");
          return kj::Url::QueryParam{kj::mv(entry[0]), kj::mv(entry[1])};
        };
      }
    }
  }

  return searchParams;
}

void URLSearchParams::append(kj::String name, kj::String value) {
  url->query.add(kj::Url::QueryParam{kj::mv(name), kj::mv(value)});
}

void URLSearchParams::delete_(kj::String name) {
  auto pivot = std::remove_if(
      url->query.begin(), url->query.end(), [&name](const auto& kv) { return kv.name == name; });
  url->query.truncate(pivot - url->query.begin());
}

kj::Maybe<kj::String> URLSearchParams::get(kj::String name) {
  for (auto& [k, v]: url->query) {
    if (k == name) {
      return kj::str(v);
    }
  }
  return kj::none;
}

kj::Array<kj::String> URLSearchParams::getAll(kj::String name) {
  kj::Vector<kj::String> result;
  for (auto& [k, v]: url->query) {
    if (k == name) {
      result.add(kj::str(v));
    }
  }
  return result.releaseAsArray();
}

bool URLSearchParams::has(kj::String name) {
  for (auto& [k, v]: url->query) {
    if (k == name) {
      return true;
    }
  }
  return false;
}

// Set the first element named `name` to `value`, then remove all the rest matching that name.
void URLSearchParams::set(kj::String name, kj::String value) {
  const auto predicate = [name = name.slice(0)](const auto& kv) { return kv.name == name; };
  auto firstFound = std::find_if(url->query.begin(), url->query.end(), predicate);
  if (firstFound != url->query.end()) {
    firstFound->value = kj::mv(value);
    auto pivot = std::remove_if(++firstFound, url->query.end(), predicate);
    url->query.truncate(pivot - url->query.begin());
  } else {
    append(kj::mv(name), kj::mv(value));
  }
}

// Sort by UTF-16 code unit, preserving order of equal elements.
void URLSearchParams::sort() {
  // TODO(perf): This UTF-16 business is sad. The WPT points out the specific example 🌈 < ﬃ,
  //   because the rainbow is lexicographically less than the ligature in UTF-16 code units. In
  //   UTF-8 code units, their order is the opposite.
  //
  //       UTF-8       |   UTF-16
  //   ﬃ   ef ac 83    |  fb03
  //   🌈  f0 9f 8c 88 |  d83c df08

  std::stable_sort(url->query.begin(), url->query.end(), [](const auto& left, const auto& right) {
    auto leftUtf16 = fastEncodeUtf16(left.name.asArray());
    auto rightUtf16 = fastEncodeUtf16(right.name.asArray());
    return std::lexicographical_compare(
        leftUtf16.begin(), leftUtf16.end(), rightUtf16.begin(), rightUtf16.end());
  });
}

void URLSearchParams::forEach(jsg::Lock& js,
    jsg::Function<void(kj::StringPtr, kj::StringPtr, jsg::Ref<URLSearchParams>)> callback,
    jsg::Optional<jsg::Value> thisArg) {
  auto receiver = js.v8Undefined();
  KJ_IF_SOME(arg, thisArg) {
    auto handle = arg.getHandle(js);
    if (!handle->IsNullOrUndefined()) {
      receiver = handle;
    }
  }
  callback.setReceiver(js.v8Ref(receiver));

  // On each iteration of the for loop, a JavaScript callback is invoked. If a new
  // item is appended to the this->url->query within that function, the loop must pick
  // it up. Using the classic for (;;) syntax here allows for that. However, this does
  // mean that it's possible for a user to trigger an infinite loop here if new items
  // are added to the search params unconditionally on each iteration.
  for (size_t i = 0; i < this->url->query.size(); i++) {
    auto& [key, value] = this->url->query[i];
    callback(js, value, key, JSG_THIS);
  }
}

jsg::Ref<URLSearchParams::EntryIterator> URLSearchParams::entries(jsg::Lock&) {
  return jsg::alloc<EntryIterator>(IteratorState{JSG_THIS});
}

jsg::Ref<URLSearchParams::KeyIterator> URLSearchParams::keys(jsg::Lock&) {
  return jsg::alloc<KeyIterator>(IteratorState{JSG_THIS});
}

jsg::Ref<URLSearchParams::ValueIterator> URLSearchParams::values(jsg::Lock&) {
  return jsg::alloc<ValueIterator>(IteratorState{JSG_THIS});
}

kj::String URLSearchParams::toString() {
  kj::Vector<char> chars(128);

  bool first = true;
  for (auto& param: url->query) {
    if (!first) chars.add('&');
    first = false;
    chars.addAll(kj::encodeWwwForm(param.name));
    // This *intentionally* differs from the behavior in URL::getSearch() and kj::Url::toString()!
    // URLSearchParams has no concept of "null-valued" query parameters -- they get coerced to
    // empty-valued query parameters, so we unconditionally add the '=' sign.
    chars.add('=');
    chars.addAll(kj::encodeWwwForm(param.value));
  }

  chars.add('\0');
  return kj::String(chars.releaseAsArray());
}

}  // namespace workerd::api
