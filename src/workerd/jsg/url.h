#pragma once
#include <kj/string.h>
#include <kj/common.h>
#include <workerd/jsg/memory.h>

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
    DEFAULT,
    IGNORE_FRAGMENTS,
  };

  bool equal(const Url& other, EquivalenceOption option = EquivalenceOption::DEFAULT) const
      KJ_WARN_UNUSED_RESULT;

  // Returns true if the given input can be successfully parsed as a URL. This is generally
  // more performant than using tryParse and checking for a kj::none result if all you want
  // to do is verify that the input is parseable. If you actually want to parse and use the
  // result, use tryParse instead.
  static bool canParse(kj::ArrayPtr<const char> input,
                       kj::Maybe<kj::ArrayPtr<const char>> base = kj::none)
                       KJ_WARN_UNUSED_RESULT;
  static bool canParse(kj::StringPtr input,
                       kj::Maybe<kj::StringPtr> base = kj::none)
                       KJ_WARN_UNUSED_RESULT;

  static kj::Maybe<Url> tryParse(kj::ArrayPtr<const char> input,
                                 kj::Maybe<kj::ArrayPtr<const char>> base = kj::none)
                                 KJ_WARN_UNUSED_RESULT;
  static kj::Maybe<Url> tryParse(kj::StringPtr input,
                                 kj::Maybe<kj::StringPtr> base = kj::none)
                                 KJ_WARN_UNUSED_RESULT;

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
  Url clone(EquivalenceOption option = EquivalenceOption::DEFAULT) KJ_WARN_UNUSED_RESULT;

  HostType getHostType() const;
  SchemeType getSchemeType() const;

  // Convert an ASCII hostname to Unicode.
  static kj::Array<const char> idnToUnicode(kj::ArrayPtr<const char> value) KJ_WARN_UNUSED_RESULT;

  // Convert a Unicode hostname to ASCII.
  static kj::Array<const char> idnToAscii(kj::ArrayPtr<const char> value) KJ_WARN_UNUSED_RESULT;

  JSG_MEMORY_INFO(Url) {
    tracker.trackFieldWithSize("inner", getProtocol().size() +
                                        getUsername().size() +
                                        getPassword().size() +
                                        getHost().size() +
                                        getPathname().size() +
                                        getHash().size() +
                                        getSearch().size());
  }

private:
  Url(kj::Own<void> inner);
  kj::Own<void> inner;
};

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
  void delete_(kj::ArrayPtr<const char> key,
               kj::Maybe<kj::ArrayPtr<const char>> maybeValue = kj::none);
  bool has(kj::ArrayPtr<const char> key,
           kj::Maybe<kj::ArrayPtr<const char>> maybeValue = kj::none) const
           KJ_WARN_UNUSED_RESULT;
  kj::Maybe<kj::ArrayPtr<const char>> get(kj::ArrayPtr<const char> key) const
    KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  kj::Array<kj::ArrayPtr<const char>> getAll(kj::ArrayPtr<const char> key) const
    KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  void sort();
  KeyIterator getKeys() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  ValueIterator getValues() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;
  EntryIterator getEntries() const KJ_LIFETIMEBOUND KJ_WARN_UNUSED_RESULT;

  kj::Array<const char> toStr() const KJ_WARN_UNUSED_RESULT;

  JSG_MEMORY_INFO(Url) {
    tracker.trackField("inner", toStr());
  }

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

}  // namespace workerd::jsg
