#pragma once

#include <workerd/jsg/url.h>
#include <kj/compat/url.h>
#include <kj/one-of.h>
#include <kj/vector.h>

namespace workerd {

class UrlWrapper final {
public:
  // Determines the mode under which the UrlWrapper should operate.
  enum class Mode {
    // Legacy mode indicates that UrlWrapper will use the kj::Url implementation.
    // This includes lazy parsing and a few other quirks.
    LEGACY,
    // Standard mode indicates that UrlWrapper will use the jsg::Url implementation.
    STANDARD,
  };

  UrlWrapper(Mode mode, kj::StringPtr input);
  UrlWrapper(UrlWrapper&&) = default;
  UrlWrapper& operator=(UrlWrapper&&) = default;
  KJ_DISALLOW_COPY(UrlWrapper);

  static UrlWrapper legacy(kj::StringPtr input);
  static UrlWrapper standard(kj::StringPtr input);

  void ensureParsed();

  UrlWrapper resolve(kj::StringPtr other);

  struct ToStringOptions {
    kj::Maybe<kj::Url::Context> context = kj::none;
    bool includeFragments = true;
  };

  // When running in STANDARD mode, the context argument is ignored.
  kj::String toString(kj::Maybe<ToStringOptions> options = kj::none);

  UrlWrapper clone();

  // Returns a legacy mode UrlWrapper that represents the "https://fake-host/" URL.
  static UrlWrapper kLegacyFake;

  // Returns a standard mode UrlWrapper that represents the "https://fake-host/" URL.
  static UrlWrapper kStandardFake;

private:
  kj::OneOf<kj::String, kj::Url, jsg::Url> inner;

  UrlWrapper(kj::OneOf<kj::Url, jsg::Url> inner);
};

class UrlList final {
public:
  UrlList() = default;
  UrlList(UrlList&&) = default;
  UrlList& operator=(UrlList&&) = default;
  KJ_DISALLOW_COPY(UrlList);

  size_t size() const;
  void add(UrlWrapper&& url);
  kj::Maybe<UrlWrapper&> back();

private:
  kj::Vector<UrlWrapper> urls;
};

}  // namespace workerd
