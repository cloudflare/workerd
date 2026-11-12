// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "urlpattern-bootstrap.h"

#include "ada.h"
#include "urlpattern-regex.h"

#include <workerd/jsg/url.h>

#include <kj/vector.h>

namespace workerd::api {

namespace {

using Init = kj::FixedArray<kj::Maybe<jsg::USVString>, UrlPatternBootstrap::INIT_LENGTH>;

jsg::USVString toUsvString(jsg::Lock& js, jsg::JsValue value, kj::StringPtr what) {
  return JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsString>(), TypeError, what, " must be a string")
      .toUSVString(js);
}

kj::Maybe<jsg::USVString> toOptionalUsvString(
    jsg::Lock& js, jsg::JsValue value, kj::StringPtr what) {
  if (value.isUndefined()) return kj::none;
  return toUsvString(js, value, what);
}

bool toBoolean(jsg::Lock& js, jsg::JsValue value, kj::StringPtr what) {
  return JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsBoolean>(), TypeError, what, " must be a boolean")
      .value(js);
}

Init toInit(jsg::Lock& js, jsg::JsValue value) {
  auto array =
      JSG_REQUIRE_NONNULL(value.tryCast<jsg::JsArray>(), TypeError, "init must be an array");
  JSG_REQUIRE(array.size() == UrlPatternBootstrap::INIT_LENGTH, TypeError, "init must have ",
      UrlPatternBootstrap::INIT_LENGTH, " elements");
  Init init;
  for (auto i: kj::indices(init)) {
    init[i] = toOptionalUsvString(js, array.get(js, i), "an init member"_kj);
  }
  return init;
}

std::optional<std::string> toStd(const kj::Maybe<jsg::USVString>& value) {
  KJ_IF_SOME(v, value) {
    return std::string(v.cStr(), v.size());
  }
  return std::nullopt;
}

ada::url_pattern_init toAdaInit(const Init& init) {
  return {
    .protocol = toStd(init[0]),
    .username = toStd(init[1]),
    .password = toStd(init[2]),
    .hostname = toStd(init[3]),
    .port = toStd(init[4]),
    .pathname = toStd(init[5]),
    .search = toStd(init[6]),
    .hash = toStd(init[7]),
    .base_url = toStd(init[8]),
  };
}

kj::Maybe<kj::String> toKjString(const kj::Maybe<jsg::USVString>& value) {
  return value.map([](const jsg::USVString& v) { return kj::str(v); });
}

jsg::UrlPattern::Init toLegacyInit(const Init& init) {
  return {
    .protocol = toKjString(init[0]),
    .username = toKjString(init[1]),
    .password = toKjString(init[2]),
    .hostname = toKjString(init[3]),
    .port = toKjString(init[4]),
    .pathname = toKjString(init[5]),
    .search = toKjString(init[6]),
    .hash = toKjString(init[7]),
    .baseUrl = toKjString(init[8]),
  };
}

jsg::JsString toJs(jsg::Lock& js, std::string_view value) {
  return js.str(kj::arrayPtr(value.data(), value.size()));
}

UrlPatternBootstrap::ComponentType toComponentType(ada::url_pattern_component_type type) {
  using Type = UrlPatternBootstrap::ComponentType;
  switch (type) {
    case ada::url_pattern_component_type::REGEXP:
      return Type::REGEXP;
    case ada::url_pattern_component_type::FULL_WILDCARD:
      return Type::FULL_WILDCARD;
    case ada::url_pattern_component_type::EXACT_MATCH:
      return Type::EXACT_MATCH;
    case ada::url_pattern_component_type::EMPTY:
      return Type::EMPTY;
  }
  KJ_UNREACHABLE;
}

void addComponent(jsg::Lock& js,
    kj::Vector<jsg::JsValue>& out,
    const ada::url_pattern_component<UrlPatternRegexProvider>& component) {
  out.add(toJs(js, component.pattern));
  if (component.type == ada::url_pattern_component_type::REGEXP) {
    out.add(component.regexp.getHandle(js));
  } else {
    out.add(js.undefined());
  }
  const auto& names = component.group_name_list;
  out.add(js.arr(kj::arrayPtr(names.data(), names.size()),
      [](jsg::Lock& js, const std::string& name) -> jsg::JsValue { return toJs(js, name); }));
  out.add(js.num(static_cast<double>(toComponentType(component.type))));
  out.add(toJs(js, component.exact_match_value));
}

jsg::JsRegExp compileLegacyRegex(
    jsg::Lock& js, const jsg::UrlPattern::Component& component, bool ignoreCase) {
  JSG_TRY(js) {
    auto flags = jsg::Lock::RegExpFlags::kUNICODE;
    if (ignoreCase) {
      flags = static_cast<jsg::Lock::RegExpFlags>(
          flags | static_cast<int>(jsg::Lock::RegExpFlags::kIGNORE_CASE));
    }
    return js.regexp(component.getRegex(), flags);
  }
  JSG_CATCH(_) {
    JSG_FAIL_REQUIRE(TypeError, "Invalid regular expression syntax.");
  }
}

void addLegacyComponent(jsg::Lock& js,
    kj::Vector<jsg::JsValue>& out,
    const jsg::UrlPattern::Component& component,
    bool ignoreCase) {
  out.add(js.str(component.getPattern()));
  out.add(compileLegacyRegex(js, component, ignoreCase));
  out.add(js.arr(component.getNames(),
      [](jsg::Lock& js, const kj::String& name) -> jsg::JsValue { return js.str(name); }));
  out.add(js.num(static_cast<double>(UrlPatternBootstrap::ComponentType::REGEXP)));
  out.add(js.str());
}

}  // namespace

jsg::JsValue UrlPatternBootstrap::compile(
    jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue ignoreCase) {
  // Owns what the string_views below view.
  kj::Maybe<jsg::USVString> inputString;
  ada::url_pattern_input adaInput;
  KJ_IF_SOME(string, input.tryCast<jsg::JsString>()) {
    auto& str = inputString.emplace(string.toUSVString(js));
    adaInput = std::string_view(str.begin(), str.size());
  } else {
    adaInput = toAdaInit(toInit(js, input));
  }
  auto baseString = toOptionalUsvString(js, base, "base"_kj);
  ada::url_pattern_options options{.ignore_case = toBoolean(js, ignoreCase, "ignoreCase"_kj)};

  std::string_view baseView;
  std::string_view* basePtr = nullptr;
  KJ_IF_SOME(b, baseString) {
    baseView = std::string_view(b.begin(), b.size());
    basePtr = &baseView;
  }

  auto result =
      ada::parse_url_pattern<UrlPatternRegexProvider>(kj::mv(adaInput), basePtr, &options);
  JSG_REQUIRE(result.has_value(), TypeError, "Failed to construct URLPattern");

  kj::Vector<jsg::JsValue> out(COMPILED_LENGTH);
  out.add(js.boolean(result->has_regexp_groups()));
  addComponent(js, out, result->protocol_component);
  addComponent(js, out, result->username_component);
  addComponent(js, out, result->password_component);
  addComponent(js, out, result->hostname_component);
  addComponent(js, out, result->port_component);
  addComponent(js, out, result->pathname_component);
  addComponent(js, out, result->search_component);
  addComponent(js, out, result->hash_component);
  KJ_DASSERT(out.size() == COMPILED_LENGTH);
  return js.arr(out.asPtr());
}

jsg::JsValue UrlPatternBootstrap::processInit(jsg::Lock& js, jsg::JsValue init) {
  // As ada's url_pattern::match() processes an init input: every component defaults to ''.
  std::string_view empty;
  auto result = ada::url_pattern_init::process(toAdaInit(toInit(js, init)),
      ada::url_pattern_init::process_type::url, empty, empty, empty, empty, empty, empty, empty,
      empty);
  if (!result.has_value()) return js.undefined();

  const auto view = [](const std::optional<std::string>& value) -> std::string_view {
    return value.has_value() ? std::string_view(*value) : std::string_view();
  };
  auto search = view(result->search);
  if (search.starts_with('?')) search.remove_prefix(1);
  jsg::JsValue components[] = {
    toJs(js, view(result->protocol)),
    toJs(js, view(result->username)),
    toJs(js, view(result->password)),
    toJs(js, view(result->hostname)),
    toJs(js, view(result->port)),
    toJs(js, view(result->pathname)),
    toJs(js, search),
    toJs(js, view(result->hash)),
  };
  return js.arr(kj::arrayPtr(components));
}

jsg::JsValue UrlPatternBootstrap::compileLegacy(
    jsg::Lock& js, jsg::JsValue input, jsg::JsValue base, jsg::JsValue ignoreCase) {
  auto baseString = toOptionalUsvString(js, base, "base"_kj);
  jsg::UrlPattern::CompileOptions options{
    .baseUrl = baseString.map([](jsg::USVString& b) { return b.asPtr(); }),
    .ignoreCase = toBoolean(js, ignoreCase, "ignoreCase"_kj),
  };

  auto compiled = [&]() {
    KJ_IF_SOME(string, input.tryCast<jsg::JsString>()) {
      return jsg::UrlPattern::tryCompile(string.toUSVString(js).asPtr(), options);
    }
    return jsg::UrlPattern::tryCompile(toLegacyInit(toInit(js, input)), options);
  }();

  KJ_SWITCH_ONEOF(compiled) {
    KJ_CASE_ONEOF(error, kj::String) {
      JSG_FAIL_REQUIRE(TypeError, kj::mv(error));
    }
    KJ_CASE_ONEOF(pattern, jsg::UrlPattern) {
      bool ignore = pattern.getIgnoreCase();
      kj::Vector<jsg::JsValue> out(COMPILED_LENGTH);
      out.add(js.boolean(false));
      addLegacyComponent(js, out, pattern.getProtocol(), ignore);
      addLegacyComponent(js, out, pattern.getUsername(), ignore);
      addLegacyComponent(js, out, pattern.getPassword(), ignore);
      addLegacyComponent(js, out, pattern.getHostname(), ignore);
      addLegacyComponent(js, out, pattern.getPort(), ignore);
      addLegacyComponent(js, out, pattern.getPathname(), ignore);
      addLegacyComponent(js, out, pattern.getSearch(), ignore);
      addLegacyComponent(js, out, pattern.getHash(), ignore);
      KJ_DASSERT(out.size() == COMPILED_LENGTH);
      return js.arr(out.asPtr());
    }
  }
  KJ_UNREACHABLE;
}

jsg::JsValue UrlPatternBootstrap::processInitLegacy(jsg::Lock& js, jsg::JsValue init) {
  jsg::UrlPattern::ProcessInitOptions options = {
    .mode = jsg::UrlPattern::ProcessInitOptions::Mode::URL,
  };
  KJ_SWITCH_ONEOF(jsg::UrlPattern::processInit(toLegacyInit(toInit(js, init)), kj::mv(options))) {
    KJ_CASE_ONEOF(error, kj::String) {
      JSG_FAIL_REQUIRE(TypeError, kj::mv(error));
    }
    KJ_CASE_ONEOF(processed, jsg::UrlPattern::Init) {
      const auto component = [&](kj::Maybe<kj::String>& value) -> jsg::JsValue {
        KJ_IF_SOME(v, value) {
          return js.str(v);
        }
        return js.str();
      };
      jsg::JsValue components[] = {
        component(processed.protocol),
        component(processed.username),
        component(processed.password),
        component(processed.hostname),
        component(processed.port),
        component(processed.pathname),
        component(processed.search),
        component(processed.hash),
      };
      return js.arr(kj::arrayPtr(components));
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::api
