#include <workerd/util/url-wrapper.h>
#include <workerd/jsg/jsg.h>

namespace workerd {

namespace {
kj::OneOf<kj::String, kj::Url, jsg::Url> initialize(UrlWrapper::Mode mode, kj::StringPtr input) {
  switch (mode) {
    case UrlWrapper::Mode::LEGACY: {
      return kj::str(input);
    }
    case UrlWrapper::Mode::STANDARD: {
      return JSG_REQUIRE_NONNULL(jsg::Url::tryParse(input), TypeError, "Invalid URL");
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

UrlWrapper UrlWrapper::kLegacyFake = UrlWrapper::legacy("https://fake-host/");
UrlWrapper UrlWrapper::kStandardFake = UrlWrapper::standard("https://fake-host/");

UrlWrapper::UrlWrapper(Mode mode, kj::StringPtr input)
   : inner(initialize(mode, input)) {}

UrlWrapper::UrlWrapper(kj::OneOf<kj::Url, jsg::Url> inner)
    : inner(kj::mv(inner)) {}

void UrlWrapper::ensureParsed() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(url, jsg::Url) {
      // Parsed!
    }
    KJ_CASE_ONEOF(url, kj::Url) {
      // Parsed!
    }
    KJ_CASE_ONEOF(str, kj::String) {
      inner = JSG_REQUIRE_NONNULL(kj::Url::tryParse(str), TypeError, "Invalid URL");
    }
  }
}

UrlWrapper UrlWrapper::resolve(kj::StringPtr other) {
  ensureParsed();
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(url, jsg::Url) {
      auto resolved = JSG_REQUIRE_NONNULL(url.resolve(other), TypeError, "Invalid URL");
      return UrlWrapper(kj::mv(resolved));
    }
    KJ_CASE_ONEOF(url, kj::Url) {
      auto resolved = JSG_REQUIRE_NONNULL(url.tryParseRelative(other), TypeError, "Invalid URL");
      return UrlWrapper(kj::mv(resolved));
    }
    KJ_CASE_ONEOF(str, kj::String) {
      // The call to ensureParsed above should have already parsed the URL.
      KJ_UNREACHABLE;
    }
  }
  KJ_UNREACHABLE;
}

kj::String UrlWrapper::toString(kj::Maybe<ToStringOptions> maybeOptions) {
  auto options = maybeOptions.orDefault({});
  ensureParsed();
  auto context = options.context.orDefault(kj::Url::Context::REMOTE_HREF);
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(url, jsg::Url) {
      switch (context) {
        case kj::Url::Context::REMOTE_HREF: {
          if (options.includeFragments) {
            return kj::str(url.getHref());
          } else {
            auto cloned = url.clone(jsg::Url::EquivalenceOption::IGNORE_FRAGMENTS);
            return kj::str(cloned.getHref());
          }
        }
        case kj::Url::Context::HTTP_PROXY_REQUEST: {
          return kj::str(url.getOrigin(), url.getPathname(), url.getSearch());
        }
        case kj::Url::Context::HTTP_REQUEST: {
          return kj::str(url.getPathname(), url.getSearch());
        }
      }
    }
    KJ_CASE_ONEOF(url, kj::Url) {
      if (options.includeFragments) {
        return url.toString(context);
      } else {
        auto cloned = url.clone();
        cloned.fragment = kj::none;
        return cloned.toString(context);
      }
    }
    KJ_CASE_ONEOF(str, kj::String) {
      // The call to ensureParsed above should have already parsed the URL.
      KJ_UNREACHABLE;
    }
  }
  KJ_UNREACHABLE;
}

UrlWrapper UrlWrapper::clone() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(url, jsg::Url) {
      return UrlWrapper(url.clone());
    }
    KJ_CASE_ONEOF(url, kj::Url) {
      return UrlWrapper(url.clone());
    }
    KJ_CASE_ONEOF(str, kj::String) {
      // The only reason this would be a string is if we're in LEGACY mode.
      return UrlWrapper(Mode::LEGACY, str);
    }
  }
  KJ_UNREACHABLE;
}

UrlWrapper UrlWrapper::legacy(kj::StringPtr input) {
  return UrlWrapper(Mode::LEGACY, input);
}

UrlWrapper UrlWrapper::standard(kj::StringPtr input) {
  return UrlWrapper(Mode::STANDARD, input);
}

size_t UrlList::size() const {
  return urls.size();
}

void UrlList::add(UrlWrapper&& url) {
  urls.add(kj::mv(url));
}

kj::Maybe<UrlWrapper&> UrlList::back() {
  if (urls.size() == 0) return kj::none;
  return urls.back();
}

}  // namespace workerd
