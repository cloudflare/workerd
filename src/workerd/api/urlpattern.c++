// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "urlpattern.h"

#include <kj/vector.h>

namespace workerd::api {

namespace {
jsg::JsRef<jsg::JsRegExp> compileRegex(
    jsg::Lock& js, const jsg::UrlPattern::Component& component, bool ignoreCase) {
  return js.tryCatch([&] {
    jsg::Lock::RegExpFlags flags = jsg::Lock::RegExpFlags::kUNICODE;
    if (ignoreCase) {
      flags = static_cast<jsg::Lock::RegExpFlags>(
          flags | static_cast<int>(jsg::Lock::RegExpFlags::kIGNORE_CASE));
    }
    return jsg::JsRef<jsg::JsRegExp>(js, js.regexp(component.getRegex(), flags));
  }, [&](auto reason) -> jsg::JsRef<jsg::JsRegExp> {
    JSG_FAIL_REQUIRE(TypeError, "Invalid regular expression syntax.");
  });
}

jsg::Ref<URLPattern> create(jsg::Lock& js, jsg::UrlPattern pattern) {
  bool ignoreCase = pattern.getIgnoreCase();

  // Might look a bit confusing here. The URL_PATTERN_COMPONENTS macro
  // is used also to define the constructor for URLPattern so to make
  // sure things line up right we reuse that pattern here also. Because
  // we are moving the pattern into the constructor, we need to make sure
  // the regex patterns are compiled first so we use the macro twice.
#define V(Name, var) auto var = compileRegex(js, pattern.get##Name(), ignoreCase);
  URL_PATTERN_COMPONENTS(V)
#undef V

#define V(_, var) , kj::mv(var)
  return jsg::alloc<URLPattern>(kj::mv(pattern) URL_PATTERN_COMPONENTS(V));
#undef V
}

kj::Maybe<URLPattern::URLPatternComponentResult> execRegex(jsg::Lock& js,
    jsg::JsRef<jsg::JsRegExp>& regex,
    kj::ArrayPtr<const kj::String> nameList,
    kj::StringPtr input) {
  using Groups = jsg::Dict<kj::String, kj::String>;

  KJ_IF_SOME(array, regex.getHandle(js)(js, input)) {
    // Starting at 1 here looks a bit odd but it is intentional. The result of the regex
    // is an array and we're skipping the first element.
    uint32_t index = 1;
    uint32_t length = array.size();
    kj::Vector<Groups::Field> fields(length - 1);

    while (index < length) {
      auto value = array.get(js, index);
      fields.add(Groups::Field{
        .name = kj::str(nameList[index - 1]),
        .value = value.isUndefined() ? kj::str() : kj::str(value),
      });
      index++;
    }

    return URLPattern::URLPatternComponentResult{
      .input = kj::str(input),
      .groups = Groups{.fields = fields.releaseAsArray()},
    };
  }

  return kj::none;
}
}  // namespace

URLPattern::URLPattern(jsg::UrlPattern inner,
    jsg::JsRef<jsg::JsRegExp> protocolRegex,
    jsg::JsRef<jsg::JsRegExp> usernameRegex,
    jsg::JsRef<jsg::JsRegExp> passwordRegex,
    jsg::JsRef<jsg::JsRegExp> hostnameRegex,
    jsg::JsRef<jsg::JsRegExp> portRegex,
    jsg::JsRef<jsg::JsRegExp> pathnameRegex,
    jsg::JsRef<jsg::JsRegExp> searchRegex,
    jsg::JsRef<jsg::JsRegExp> hashRegex)
    : inner(kj::mv(inner)),
      protocolRegex(kj::mv(protocolRegex)),
      usernameRegex(kj::mv(usernameRegex)),
      passwordRegex(kj::mv(passwordRegex)),
      hostnameRegex(kj::mv(hostnameRegex)),
      portRegex(kj::mv(portRegex)),
      pathnameRegex(kj::mv(pathnameRegex)),
      searchRegex(kj::mv(searchRegex)),
      hashRegex(kj::mv(hashRegex)) {}

void URLPattern::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(protocolRegex, usernameRegex, passwordRegex, hostnameRegex, portRegex,
      pathnameRegex, searchRegex, hashRegex);
}

kj::StringPtr URLPattern::getProtocol() {
  return inner.getProtocol().getPattern();
}
kj::StringPtr URLPattern::getUsername() {
  return inner.getUsername().getPattern();
}
kj::StringPtr URLPattern::getPassword() {
  return inner.getPassword().getPattern();
}
kj::StringPtr URLPattern::getHostname() {
  return inner.getHostname().getPattern();
}
kj::StringPtr URLPattern::getPort() {
  return inner.getPort().getPattern();
}
kj::StringPtr URLPattern::getPathname() {
  return inner.getPathname().getPattern();
}
kj::StringPtr URLPattern::getSearch() {
  return inner.getSearch().getPattern();
}
kj::StringPtr URLPattern::getHash() {
  return inner.getHash().getPattern();
}

URLPattern::URLPatternInit::operator jsg::UrlPattern::Init() {
  return {
    .protocol = this->protocol.map([](kj::String& str) { return kj::str(str); }),
    .username = this->username.map([](kj::String& str) { return kj::str(str); }),
    .password = this->password.map([](kj::String& str) { return kj::str(str); }),
    .hostname = this->hostname.map([](kj::String& str) { return kj::str(str); }),
    .port = this->port.map([](kj::String& str) { return kj::str(str); }),
    .pathname = this->pathname.map([](kj::String& str) { return kj::str(str); }),
    .search = this->search.map([](kj::String& str) { return kj::str(str); }),
    .hash = this->hash.map([](kj::String& str) { return kj::str(str); }),
    .baseUrl = this->baseURL.map([](kj::String& str) { return kj::str(str); }),
  };
}

jsg::Ref<URLPattern> URLPattern::constructor(jsg::Lock& js,
    jsg::Optional<URLPatternInput> input,
    jsg::Optional<kj::String> baseURL,
    jsg::Optional<URLPatternOptions> patternOptions) {
  auto options = patternOptions.orDefault({});
  KJ_SWITCH_ONEOF(kj::mv(input).orDefault(URLPatternInit{})) {
    KJ_CASE_ONEOF(str, kj::String) {
      KJ_SWITCH_ONEOF(jsg::UrlPattern::tryCompile(str.asPtr(),
                          jsg::UrlPattern::CompileOptions{
                            .baseUrl = baseURL.map([](kj::String& str) { return str.asPtr(); }),
                            .ignoreCase = options.ignoreCase.orDefault(false),
                          })) {
        KJ_CASE_ONEOF(err, kj::String) {
          JSG_FAIL_REQUIRE(TypeError, kj::mv(err));
        }
        KJ_CASE_ONEOF(pattern, jsg::UrlPattern) {
          return create(js, kj::mv(pattern));
        }
      }
    }
    KJ_CASE_ONEOF(init, URLPatternInit) {
      KJ_SWITCH_ONEOF(jsg::UrlPattern::tryCompile(init,
                          jsg::UrlPattern::CompileOptions{
                            .ignoreCase = options.ignoreCase.orDefault(false),
                          })) {
        KJ_CASE_ONEOF(err, kj::String) {
          JSG_FAIL_REQUIRE(TypeError, kj::mv(err));
        }
        KJ_CASE_ONEOF(pattern, jsg::UrlPattern) {
          return create(js, kj::mv(pattern));
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

bool URLPattern::test(
    jsg::Lock& js, jsg::Optional<URLPatternInput> input, jsg::Optional<kj::String> baseURL) {
  return exec(js, kj::mv(input), kj::mv(baseURL)) != kj::none;
}

kj::Maybe<URLPattern::URLPatternResult> URLPattern::exec(
    jsg::Lock& js, jsg::Optional<URLPatternInput> maybeInput, jsg::Optional<kj::String> maybeBase) {
  auto input = kj::mv(maybeInput).orDefault(URLPattern::URLPatternInit());
  kj::Vector<URLPattern::URLPatternInput> inputs(2);

  kj::String protocol = kj::str();
  kj::String username = kj::str();
  kj::String password = kj::str();
  kj::String hostname = kj::str();
  kj::String port = kj::str();
  kj::String pathname = kj::str();
  kj::String search = kj::str();
  kj::String hash = kj::str();

  KJ_SWITCH_ONEOF(input) {
    KJ_CASE_ONEOF(string, kj::String) {
      KJ_IF_SOME(url, jsg::Url::tryParse(string.asPtr(), maybeBase.map([](kj::String& s) {
        return s.asPtr();
      }))) {
        auto p = url.getProtocol();
        protocol = kj::str(p.slice(0, p.size() - 1));
        username = kj::str(url.getUsername());
        password = kj::str(url.getPassword());
        hostname = kj::str(url.getHostname());
        port = kj::str(url.getPort());
        pathname = kj::str(url.getPathname());
        search = url.getSearch().size() > 0 ? kj::str(url.getSearch().slice(1)) : kj::str();
        hash = url.getHash().size() > 0 ? kj::str(url.getHash().slice(1)) : kj::str();
      } else {
        return kj::none;
      }
      inputs.add(kj::mv(string));
      KJ_IF_SOME(base, maybeBase) {
        inputs.add(kj::mv(base));
      }
    }
    KJ_CASE_ONEOF(i, URLPattern::URLPatternInit) {
      JSG_REQUIRE(
          maybeBase == kj::none, TypeError, "A baseURL is not allowed when input is an object.");
      inputs.add(URLPattern::URLPatternInit{
        .protocol = i.protocol.map([](kj::String& str) { return kj::str(str); }),
        .username = i.username.map([](kj::String& str) { return kj::str(str); }),
        .password = i.password.map([](kj::String& str) { return kj::str(str); }),
        .hostname = i.hostname.map([](kj::String& str) { return kj::str(str); }),
        .port = i.port.map([](kj::String& str) { return kj::str(str); }),
        .pathname = i.pathname.map([](kj::String& str) { return kj::str(str); }),
        .search = i.search.map([](kj::String& str) { return kj::str(str); }),
        .hash = i.hash.map([](kj::String& str) { return kj::str(str); }),
        .baseURL = i.baseURL.map([](kj::String& str) { return kj::str(str); }),
      });

      jsg::UrlPattern::Init init = {
        .protocol = kj::mv(i.protocol),
        .username = kj::mv(i.username),
        .password = kj::mv(i.password),
        .hostname = kj::mv(i.hostname),
        .port = kj::mv(i.port),
        .pathname = kj::mv(i.pathname),
        .search = kj::mv(i.search),
        .hash = kj::mv(i.hash),
        .baseUrl = kj::mv(i.baseURL),
      };

      jsg::UrlPattern::ProcessInitOptions options = {
        .mode = jsg::UrlPattern::ProcessInitOptions::Mode::URL};

      KJ_SWITCH_ONEOF(jsg::UrlPattern::processInit(kj::mv(init), kj::mv(options))) {
        KJ_CASE_ONEOF(err, kj::String) {
          JSG_FAIL_REQUIRE(TypeError, kj::mv(err));
        }
        KJ_CASE_ONEOF(init, jsg::UrlPattern::Init) {
          protocol = kj::mv(init.protocol).orDefault(kj::str());
          username = kj::mv(init.username).orDefault(kj::str());
          password = kj::mv(init.password).orDefault(kj::str());
          hostname = kj::mv(init.hostname).orDefault(kj::str());
          port = kj::mv(init.port).orDefault(kj::str());
          pathname = kj::mv(init.pathname).orDefault(kj::str());
          search = kj::mv(init.search).orDefault(kj::str());
          hash = kj::mv(init.hash).orDefault(kj::str());
        }
      }
    }
  }

  auto protocolExecResult = execRegex(js, protocolRegex, inner.getProtocol().getNames(), protocol);
  auto usernameExecResult = execRegex(js, usernameRegex, inner.getUsername().getNames(), username);
  auto passwordExecResult = execRegex(js, passwordRegex, inner.getPassword().getNames(), password);
  auto hostnameExecResult = execRegex(js, hostnameRegex, inner.getHostname().getNames(), hostname);
  auto portExecResult = execRegex(js, portRegex, inner.getPort().getNames(), port);
  auto pathnameExecResult = execRegex(js, pathnameRegex, inner.getPathname().getNames(), pathname);
  auto searchExecResult = execRegex(js, searchRegex, inner.getSearch().getNames(), search);
  auto hashExecResult = execRegex(js, hashRegex, inner.getHash().getNames(), hash);

  if (protocolExecResult == kj::none || usernameExecResult == kj::none ||
      passwordExecResult == kj::none || hostnameExecResult == kj::none ||
      portExecResult == kj::none || pathnameExecResult == kj::none ||
      searchExecResult == kj::none || hashExecResult == kj::none) {
    return kj::none;
  }

  return URLPattern::URLPatternResult{
    .inputs = inputs.releaseAsArray(),
    .protocol = kj::mv(KJ_REQUIRE_NONNULL(protocolExecResult)),
    .username = kj::mv(KJ_REQUIRE_NONNULL(usernameExecResult)),
    .password = kj::mv(KJ_REQUIRE_NONNULL(passwordExecResult)),
    .hostname = kj::mv(KJ_REQUIRE_NONNULL(hostnameExecResult)),
    .port = kj::mv(KJ_REQUIRE_NONNULL(portExecResult)),
    .pathname = kj::mv(KJ_REQUIRE_NONNULL(pathnameExecResult)),
    .search = kj::mv(KJ_REQUIRE_NONNULL(searchExecResult)),
    .hash = kj::mv(KJ_REQUIRE_NONNULL(hashExecResult)),
  };
}
}  // namespace workerd::api
