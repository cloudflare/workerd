// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "urlpattern.h"

#include "ada.h"

namespace workerd::api {
std::optional<URLPattern::URLPatternRegexEngine::regex_type> URLPattern::URLPatternRegexEngine::
    create_instance(std::string_view pattern, bool ignore_case) {
  jsg::Lock& js = jsg::Lock::from(v8::Isolate::GetCurrent());
  jsg::Lock::RegExpFlags flags = jsg::Lock::RegExpFlags::kUNICODE_SETS;
  if (ignore_case) {
    flags = static_cast<jsg::Lock::RegExpFlags>(
        flags | static_cast<int>(jsg::Lock::RegExpFlags::kIGNORE_CASE));
  }

  return js.tryCatch([&]() -> std::optional<regex_type> {
    return jsg::JsRef(js, js.regexp(kj::StringPtr(pattern.data(), pattern.size()), flags));
  }, [&](auto reason) -> std::optional<regex_type> { return std::nullopt; });
}

bool URLPattern::URLPatternRegexEngine::regex_match(
    std::string_view input, const regex_type& pattern) {
  jsg::Lock& js = jsg::Lock::from(v8::Isolate::GetCurrent());
  return pattern.getHandle(js).match(js, kj::StringPtr(input.data(), input.size()));
}

std::optional<std::vector<std::optional<std::string>>> URLPattern::URLPatternRegexEngine::
    regex_search(std::string_view input, const regex_type& pattern) {
  jsg::Lock& js = jsg::Lock::from(v8::Isolate::GetCurrent());
  KJ_IF_SOME(matches, pattern.getHandle(js)(js, kj::StringPtr(input.data(), input.size()))) {
    std::vector<std::optional<std::string>> results;
    results.reserve(matches.size());
    for (size_t i = 1; i < matches.size(); i++) {
      auto value = matches.get(js, i);
      if (value.isUndefined()) {
        results.emplace_back(std::nullopt);
      } else {
        KJ_DASSERT(value.isString());
        auto str = value.toString(js);
        results.emplace_back(std::string(str.cStr(), str.size()));
      }
    }
    return kj::mv(results);
  }
  return std::nullopt;
}

ada::url_pattern_options URLPattern::URLPatternOptions::toAdaType() const {
  ada::url_pattern_options options{};
  options.ignore_case = ignoreCase.orDefault(false);
  return options;
}

ada::url_pattern_init URLPattern::URLPatternInit::toAdaType() const {
  ada::url_pattern_init init{};
#define V(_, name)                                                                                 \
  KJ_IF_SOME(v, name) {                                                                            \
    init.name = std::string(v.cStr(), v.size());                                                   \
  }
  URL_PATTERN_COMPONENTS(V)
#undef V
  KJ_IF_SOME(b, baseURL) {
    init.base_url = std::string(b.cStr(), b.size());
  }
  return init;
}

URLPattern::URLPatternInit URLPattern::createURLPatternInit(const ada::url_pattern_init& other) {
  URLPatternInit result{};
#define V(_, name)                                                                                 \
  if (auto v = other.name) {                                                                       \
    result.name = kj::str(kj::ArrayPtr(v->c_str(), v->size()));                                    \
  }
  URL_PATTERN_COMPONENTS(V)
#undef V

  if (auto v = other.base_url) {
    result.baseURL = kj::str(kj::ArrayPtr(v->c_str(), v->size()));
  }
  return result;
}

URLPattern::URLPatternComponentResult URLPattern::createURLPatternComponentResult(
    jsg::Lock& js, const ada::url_pattern_component_result& other) {
  auto result = URLPatternComponentResult{
    .input = js.str(kj::ArrayPtr(other.input.c_str(), other.input.size())),
    .groups = js.obj(),
  };

  for (auto& [key, value]: other.groups) {
    auto k = js.str(kj::ArrayPtr(key.c_str(), key.size()));

    if (value) {
      result.groups.set(js, k, js.str(kj::ArrayPtr(value->c_str(), value->size())));
    } else {
      result.groups.set(js, k, js.undefined());
    }
  }
  return result;
}

bool URLPattern::getHasRegExpGroups() const {
  return inner.has_regexp_groups();
}

URLPattern::URLPatternResult URLPattern::createURLPatternResult(
    jsg::Lock& js, const ada::url_pattern_result& other) {
  URLPatternResult result{
#define V(_, name) .name = URLPattern::createURLPatternComponentResult(js, other.name),
    URL_PATTERN_COMPONENTS(V)
#undef V
  };

  auto vecInputs = kj::heapArray<kj::OneOf<jsg::JsString, URLPatternInit>>(other.inputs.size());
  size_t i = 0;
  for (const auto& input: other.inputs) {
    if (std::holds_alternative<std::string_view>(input)) {
      auto raw = std::get<std::string_view>(input);
      vecInputs[i] = js.str(kj::ArrayPtr(raw.data(), raw.size()));
    } else {
      KJ_DASSERT(std::holds_alternative<ada::url_pattern_init>(input));
      auto obj = std::get<ada::url_pattern_init>(input);
      vecInputs[i] = createURLPatternInit(obj);
    }
    i++;
  }
  result.inputs = mv(vecInputs);
  return result;
}

#define V(uppercase, lowercase)                                                                    \
  kj::StringPtr URLPattern::get##uppercase() const {                                               \
    auto value = inner.get_##lowercase();                                                          \
    return kj::StringPtr(value.data(), value.size());                                              \
  }
URL_PATTERN_COMPONENTS(V)
#undef V

jsg::Ref<URLPattern> URLPattern::constructor(jsg::Lock& js,
    jsg::Optional<kj::OneOf<kj::String, URLPatternInit>> maybeInput,
    jsg::Optional<kj::OneOf<kj::String, URLPatternOptions>> maybeBase,
    jsg::Optional<URLPatternOptions> maybeOptions) {
  ada::url_pattern_input input;
  std::optional<std::string_view> base{};
  std::optional<ada::url_pattern_options> options{};

  KJ_IF_SOME(mi, maybeInput) {
    KJ_SWITCH_ONEOF(mi) {
      KJ_CASE_ONEOF(str, kj::String) {
        input = std::string_view(str.begin(), str.size());
      }
      KJ_CASE_ONEOF(init, URLPatternInit) {
        input = init.toAdaType();
      }
    }
  } else {
    input = ada::url_pattern_init{};
  }

  KJ_IF_SOME(b, maybeBase) {
    KJ_SWITCH_ONEOF(b) {
      KJ_CASE_ONEOF(str, kj::String) {
        base = std::string_view(str.begin(), str.size());
      }
      KJ_CASE_ONEOF(o, URLPatternOptions) {
        options = o.toAdaType();
      }
    }
  }

  if (!options.has_value()) {
    KJ_IF_SOME(o, maybeOptions) {
      options = o.toAdaType();
    }
  }

  std::string_view* base_opt = base ? &(*base) : nullptr;
  auto result =
      ada::parse_url_pattern<URLPatternRegexEngine>(input, base_opt, options ? &*options : nullptr);
  if (result.has_value()) {
    return jsg::alloc<URLPattern>(std::move(*result));
  }

  JSG_FAIL_REQUIRE(TypeError, "Failed to construct URLPattern");
}

bool URLPattern::test(jsg::Optional<kj::OneOf<kj::String, URLPatternInit>> maybeInput,
    jsg::Optional<kj::String> maybeBase) {
  ada::result<bool> result;
  std::optional<std::string_view> base{};

  KJ_IF_SOME(b, maybeBase) {
    base = std::string_view(b.begin(), b.size());
  }

  std::string_view* base_ptr = base ? &*base : nullptr;

  KJ_IF_SOME(mi, maybeInput) {
    KJ_SWITCH_ONEOF(mi) {
      KJ_CASE_ONEOF(str, kj::String) {
        result = inner.test(std::string_view(str.begin(), str.size()), base_ptr);
      }
      KJ_CASE_ONEOF(pi, URLPattern::URLPatternInit) {
        result = inner.test(pi.toAdaType(), base_ptr);
      }
    }
  } else {
    result = inner.test(ada::url_pattern_init{}, base_ptr);
  }

  JSG_REQUIRE(result.has_value(), TypeError, "Failed to test URLPattern");

  return *result;
}

kj::Maybe<URLPattern::URLPatternResult> URLPattern::exec(jsg::Lock& js,
    jsg::Optional<kj::OneOf<kj::String, URLPatternInit>> maybeInput,
    jsg::Optional<kj::String> maybeBase) {
  ada::result<std::optional<ada::url_pattern_result>> result;
  std::optional<std::string_view> base_url{};
  std::string base_url_base;

  KJ_IF_SOME(b, maybeBase) {
    base_url_base = std::string(b.begin(), b.size());
    base_url = std::string_view(base_url_base);
  }

  KJ_IF_SOME(mi, maybeInput) {
    KJ_SWITCH_ONEOF(mi) {
      KJ_CASE_ONEOF(str, kj::String) {
        result = inner.exec(
            std::string_view(str.begin(), str.size()), base_url ? &(*base_url) : nullptr);
      }
      KJ_CASE_ONEOF(pi, URLPattern::URLPatternInit) {
        result = inner.exec(pi.toAdaType(), base_url ? &(*base_url) : nullptr);
      }
    }
  } else {
    result = inner.exec(ada::url_pattern_init{}, base_url ? &(*base_url) : nullptr);
  }

  // If result does not exist, we should throw.
  JSG_REQUIRE(result.has_value(), TypeError, "Failed to exec URLPattern");

  if (result->has_value()) {
    return createURLPatternResult(js, **result);
  }

  // Return null
  return kj::none;
}
}  // namespace workerd::api
