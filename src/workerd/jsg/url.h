#pragma once
#include <workerd/jsg/memory.h>

#include <kj/common.h>
#include <kj/one-of.h>
#include <kj/string.h>

namespace workerd::jsg {

// A WHATWG-compliant URL implementation provided by ada-url.
class Url final {
 public:
  // Keep in sync with ada::scheme:type
  enum class SchemeType {
    HTTP = 0,
    NOT_SPECIAL = 1,
    HTTPS = 2,
    WS = 3,
    FTP = 4,
    WSS = 5,
    FILE = 6
  };

  // Keep in sync with ada::url_host_type
  enum class HostType {
    DEFAULT = 0,
    IPV4 = 1,
    IPV6 = 2,
  };

  Url(decltype(nullptr)) {}
  Url(Url&& other) = default;
  KJ_DISALLOW_COPY(Url);

  Url& operator=(Url&& other) = default;
  bool operator==(const Url& other) const KJ_WARN_UNUSED_RESULT;

  enum class EquivalenceOption {
    DEFAULT = 0,
    // When set, the fragment/hash portion of the URL will be ignored when comparing or
    // cloning URLs.
    IGNORE_FRAGMENTS = 1 << 0,
    // When set, the search portion of the URL will be ignored when comparing or cloning URLs.
    IGNORE_SEARCH = 1 << 1,
    // When set, the pathname portion of the URL will be normalized by percent-decoding
    // then re-encoding the pathname. This is useful when comparing URLs that may have
    // different, but equivalent percent-encoded paths. e.g. %66oo and foo are equivalent.
    NORMALIZE_PATH = 1 << 2,
  };

  bool equal(const Url& other,
      EquivalenceOption option = EquivalenceOption::DEFAULT) const KJ_WARN_UNUSED_RESULT;

  // Returns true if the given input can be successfully parsed as a URL. This is generally
  // more performant than using tryParse and checking for a kj::none result if all you want
  // to do is verify that the input is parseable. If you actually want to parse and use the
  // result, use tryParse instead.
  static bool canParse(kj::ArrayPtr<const char> input,
      kj::Maybe<kj::ArrayPtr<const char>> base = kj::none) KJ_WARN_UNUSED_RESULT;
  static bool canParse(
      kj::StringPtr input, kj::Maybe<kj::StringPtr> base = kj::none) KJ_WARN_UNUSED_RESULT;

  static kj::Maybe<Url> tryParse(kj::ArrayPtr<const char> input,
      kj::Maybe<kj::ArrayPtr<const char>> base = kj::none) KJ_WARN_UNUSED_RESULT;
  static kj::Maybe<Url> tryParse(
      kj::StringPtr input, kj::Maybe<kj::StringPtr> base = kj::none) KJ_WARN_UNUSED_RESULT;

  kj::Array<const char> getOrigin() const KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getProtocol() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getHref() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getPathname() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getUsername() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getPassword() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getPort() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getHash() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getHost() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getHostname() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::ArrayPtr<const char> getSearch() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;

  bool setHref(kj::ArrayPtr<const char> value);
  bool setHost(kj::ArrayPtr<const char> value);
  bool setHostname(kj::ArrayPtr<const char> value);
  bool setProtocol(kj::ArrayPtr<const char> value);
  bool setUsername(kj::ArrayPtr<const char> value);
  bool setPassword(kj::ArrayPtr<const char> value);
  bool setPort(kj::Maybe<kj::ArrayPtr<const char>> value);
  bool setPathname(kj::ArrayPtr<const char> value);
  void setSearch(kj::Maybe<kj::ArrayPtr<const char>> value);
  void setHash(kj::Maybe<kj::ArrayPtr<const char>> value);

  kj::uint hashCode() const;

  kj::Maybe<Url> resolve(kj::ArrayPtr<const char> input) KJ_WARN_UNUSED_RESULT;

  // Copies this Url. If the option is set of EquivalenceOption::IGNORE_FRAGMENTS, the
  // copied Url will clear any fragment/hash that exists.
  Url clone(EquivalenceOption option = EquivalenceOption::DEFAULT) const KJ_WARN_UNUSED_RESULT;

  // Resolve the input relative to this URL
  kj::Maybe<Url> tryResolve(kj::ArrayPtr<const char> input) const KJ_WARN_UNUSED_RESULT;

  HostType getHostType() const;
  SchemeType getSchemeType() const;

  // Convert an ASCII hostname to Unicode.
  static kj::Array<const char> idnToUnicode(kj::ArrayPtr<const char> value) KJ_WARN_UNUSED_RESULT;

  // Convert a Unicode hostname to ASCII.
  static kj::Array<const char> idnToAscii(kj::ArrayPtr<const char> value) KJ_WARN_UNUSED_RESULT;

  static bool isSpecialScheme(kj::StringPtr protocol);
  static bool isSpecialSchemeDefaultPort(kj::StringPtr protocol, kj::StringPtr port);

  JSG_MEMORY_INFO(Url) {
    tracker.trackFieldWithSize("inner",
        getProtocol().size() + getUsername().size() + getPassword().size() + getHost().size() +
            getPathname().size() + getHash().size() + getSearch().size());
  }

  static kj::Array<kj::byte> percentDecode(kj::ArrayPtr<const kj::byte> input);

 private:
  Url(kj::Own<void> inner);
  kj::Own<void> inner;
};

constexpr Url::EquivalenceOption operator|(Url::EquivalenceOption a, Url::EquivalenceOption b) {
  return static_cast<Url::EquivalenceOption>(static_cast<int>(a) | static_cast<int>(b));
}
constexpr Url::EquivalenceOption operator&(Url::EquivalenceOption a, Url::EquivalenceOption b) {
  return static_cast<Url::EquivalenceOption>(static_cast<int>(a) & static_cast<int>(b));
}

class UrlSearchParams final {
 public:
  class KeyIterator final {
   public:
    bool hasNext() const;
    kj::Maybe<kj::ArrayPtr<const char>> next() const;

   private:
    KeyIterator(kj::Own<void> inner);
    kj::Own<void> inner;
    friend class UrlSearchParams;
  };
  class ValueIterator final {
   public:
    bool hasNext() const;
    kj::Maybe<kj::ArrayPtr<const char>> next() const;

   private:
    ValueIterator(kj::Own<void> inner);
    kj::Own<void> inner;
    friend class UrlSearchParams;
  };
  class EntryIterator final {
   public:
    struct Entry {
      kj::ArrayPtr<const char> key;
      kj::ArrayPtr<const char> value;
    };
    bool hasNext() const;
    kj::Maybe<Entry> next() const;

   private:
    EntryIterator(kj::Own<void> inner);
    kj::Own<void> inner;
    friend class UrlSearchParams;
  };

  UrlSearchParams();
  UrlSearchParams(UrlSearchParams&& other) = default;
  KJ_DISALLOW_COPY(UrlSearchParams);

  UrlSearchParams& operator=(UrlSearchParams&& other) = default;
  bool operator==(const UrlSearchParams& other) const KJ_WARN_UNUSED_RESULT;

  static kj::Maybe<UrlSearchParams> tryParse(kj::ArrayPtr<const char> input) KJ_WARN_UNUSED_RESULT;

  size_t size() const;
  void append(kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value);
  void set(kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value);
  void delete_(
      kj::ArrayPtr<const char> key, kj::Maybe<kj::ArrayPtr<const char>> maybeValue = kj::none);
  bool has(kj::ArrayPtr<const char> key,
      kj::Maybe<kj::ArrayPtr<const char>> maybeValue = kj::none) const KJ_WARN_UNUSED_RESULT;
  kj::Maybe<kj::ArrayPtr<const char>> get(
      kj::ArrayPtr<const char> key) const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::ArrayPtr<const char>> getAll(
      kj::ArrayPtr<const char> key) const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  void sort();
  KeyIterator getKeys() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  ValueIterator getValues() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  EntryIterator getEntries() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;

  kj::Array<const char> toStr() const KJ_WARN_UNUSED_RESULT;

  JSG_MEMORY_INFO(Url) {
    tracker.trackField("inner", toStr());
  }

  void reset(kj::Maybe<kj::ArrayPtr<const char>> input);

 private:
  UrlSearchParams(kj::Own<void> inner);
  kj::Own<void> inner;
};

inline kj::String KJ_STRINGIFY(const Url& url) {
  return kj::str(url.getHref());
}

inline kj::String KJ_STRINGIFY(const UrlSearchParams& searchParams) {
  return kj::str(searchParams.toStr());
}

// ======================================================================================

// Encapsulates a parsed URLPattern.
// @see https://wicg.github.io/urlpattern
class UrlPattern final {
 public:
  // If the value is T, the operation is successful.
  // If the value is kj::String, that's an Error message.
  template <typename T>
  using Result = kj::OneOf<T, kj::String>;

  // An individual, compiled component of a URLPattern.
  class Component final {
   public:
    Component(kj::String pattern, kj::String regex, kj::Array<kj::String> names);

    Component(Component&&) = default;
    Component& operator=(Component&&) = default;
    KJ_DISALLOW_COPY(Component);

    inline kj::StringPtr getPattern() const KJ_LIFETIMEBOUND {
      return pattern;
    }
    inline kj::StringPtr getRegex() const KJ_LIFETIMEBOUND {
      return regex;
    }
    inline kj::ArrayPtr<const kj::String> getNames() const KJ_LIFETIMEBOUND {
      return names.asPtr();
    }

    JSG_MEMORY_INFO(Component) {
      tracker.trackField("pattern", pattern);
      tracker.trackField("regex", regex);
      for (const auto& name: names) {
        tracker.trackField("name", name);
      }
    }

   private:
    // The normalized pattern for this component.
    kj::String pattern = nullptr;

    // The generated JavaScript regular expression for this component.
    kj::String regex = nullptr;

    // The list of sub-component names extracted for this component.
    kj::Array<kj::String> names = nullptr;
  };

  // A structure providing matching patterns for individual components of a URL.
  struct Init {
    kj::Maybe<kj::String> protocol;
    kj::Maybe<kj::String> username;
    kj::Maybe<kj::String> password;
    kj::Maybe<kj::String> hostname;
    kj::Maybe<kj::String> port;
    kj::Maybe<kj::String> pathname;
    kj::Maybe<kj::String> search;
    kj::Maybe<kj::String> hash;
    kj::Maybe<kj::String> baseUrl;
  };

  struct ProcessInitOptions {
    enum class Mode {
      PATTERN,
      URL,
    };
    Mode mode = Mode::PATTERN;
    kj::Maybe<kj::StringPtr> protocol = kj::none;
    kj::Maybe<kj::StringPtr> username = kj::none;
    kj::Maybe<kj::StringPtr> password = kj::none;
    kj::Maybe<kj::StringPtr> hostname = kj::none;
    kj::Maybe<kj::StringPtr> port = kj::none;
    kj::Maybe<kj::StringPtr> pathname = kj::none;
    kj::Maybe<kj::StringPtr> search = kj::none;
    kj::Maybe<kj::StringPtr> hash = kj::none;
  };

  // Processes the given init according to the specified mode and options.
  // If a kj::String is returned, then processing failed and the string
  // is the description to include in the error message (if any).
  static Result<Init> processInit(
      Init init, kj::Maybe<ProcessInitOptions> options = kj::none) KJ_WARN_UNUSED_RESULT;

  struct CompileOptions {
    // The base URL to use. Only used in the compile(kj::StringPtr, ...) variant.
    kj::Maybe<kj::StringPtr> baseUrl;
    bool ignoreCase = false;
  };

  static Result<UrlPattern> tryCompile(
      kj::StringPtr, kj::Maybe<CompileOptions> = kj::none) KJ_WARN_UNUSED_RESULT;
  static Result<UrlPattern> tryCompile(
      Init init, kj::Maybe<CompileOptions> = kj::none) KJ_WARN_UNUSED_RESULT;

  UrlPattern(UrlPattern&&) = default;
  UrlPattern& operator=(UrlPattern&&) = default;
  KJ_DISALLOW_COPY(UrlPattern);

  inline const Component& getProtocol() const KJ_LIFETIMEBOUND {
    return protocol;
  }
  inline const Component& getUsername() const KJ_LIFETIMEBOUND {
    return username;
  }
  inline const Component& getPassword() const KJ_LIFETIMEBOUND {
    return password;
  }
  inline const Component& getHostname() const KJ_LIFETIMEBOUND {
    return hostname;
  }
  inline const Component& getPort() const KJ_LIFETIMEBOUND {
    return port;
  }
  inline const Component& getPathname() const KJ_LIFETIMEBOUND {
    return pathname;
  }
  inline const Component& getSearch() const KJ_LIFETIMEBOUND {
    return search;
  }
  inline const Component& getHash() const KJ_LIFETIMEBOUND {
    return hash;
  }

  // If ignoreCase is true, the JavaScript regular expression created for each pattern
  // must use the `vi` flag. Otherwise, they must use the `v` flag.
  inline bool getIgnoreCase() const {
    return ignoreCase;
  }

  JSG_MEMORY_INFO(UrlPattern) {
    tracker.trackField("protocol", protocol);
    tracker.trackField("username", username);
    tracker.trackField("password", password);
    tracker.trackField("hostname", hostname);
    tracker.trackField("port", port);
    tracker.trackField("pathname", pathname);
    tracker.trackField("search", search);
    tracker.trackField("hash", hash);
  }

 private:
  UrlPattern(kj::Array<Component> components, bool ignoreCase);

  Component protocol;
  Component username;
  Component password;
  Component hostname;
  Component port;
  Component pathname;
  Component search;
  Component hash;
  bool ignoreCase;

  static Result<UrlPattern> tryCompileInit(UrlPattern::Init init, const CompileOptions& options);
};
}  // namespace workerd::jsg

// Append _url to a string literal to create a parsed URL. An assert will be triggered
// if the value cannot be parsed successfully.
const workerd::jsg::Url operator""_url(const char* str, size_t size) KJ_WARN_UNUSED_RESULT;
