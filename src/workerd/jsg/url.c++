#include "url.h"

#include <kj/hash.h>

extern "C" {
#include "ada_c.h"
}
#include "ada.h"

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <kj/debug.h>
#include <kj/string-tree.h>
#include <kj/vector.h>

#include <algorithm>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace workerd::jsg {

namespace {
class AdaOwnedStringDisposer: public kj::ArrayDisposer {
public:
  static const AdaOwnedStringDisposer INSTANCE;

protected:
  void disposeImpl(void* firstElement,
      size_t elementSize,
      size_t elementCount,
      size_t capacity,
      void (*destroyElement)(void*)) const {
    ada_owned_string data = {static_cast<const char*>(firstElement), elementCount};
    ada_free_owned_string(data);
  }
};
const AdaOwnedStringDisposer AdaOwnedStringDisposer::INSTANCE;

kj::Own<void> wrap(ada_url url) {
  return kj::disposeWith<ada_free>(url);
}

template <typename T>
T getInner(const kj::Own<void>& inner) {
  const void* value = inner.get();
  KJ_DASSERT(value != nullptr);
  return const_cast<T>(value);
}

kj::Array<const char> normalizePathEncoding(kj::ArrayPtr<const char> pathname) {
  // Sadly, this is a bit tricky because we do not want to decode %2f as a slash.
  // we want to keep those as is. So we'll split the input around those bits.
  // Unfortunately we need to split on either %2f or %2F, so we'll need to search
  // through ourselves. This is simple enough, tho. We'll percent decode as we go,
  // re-encode the pieces and then join them back together with %2F.

  static constexpr auto findNext = [](std::string_view input) -> kj::Maybe<size_t> {
    size_t pos = input.find("%2", 0);
    if (pos != std::string_view::npos) {
      if (input[pos + 2] == 'f' || input[pos + 2] == 'F') {
        return pos;
      }
    }
    return kj::none;
  };

  std::string_view input(pathname.begin(), pathname.end());
  std::vector<std::string> parts;

  while (true) {
    if (input.size() == 0) {
      parts.push_back("");
      break;
    }
    KJ_IF_SOME(pos, findNext(input)) {
      parts.push_back(ada::unicode::percent_decode(input.substr(0, pos), 0));
      input = input.substr(pos + 3);
      continue;
    } else {
      // No more %2f or %2F found. Add input to parts
      parts.push_back(ada::unicode::percent_decode(input, 0));
      break;
    }
  }

  std::string res;
  bool first = true;
  for (auto& part: parts) {
    auto encoded = ada::unicode::percent_encode(part, ada::character_sets::PATH_PERCENT_ENCODE);
    if (!first)
      res += "%2F";
    else
      first = false;
    res += encoded;
  }

  kj::Array<const char> ret = kj::heapArray<const char>(res.length());
  memcpy(const_cast<char*>(ret.begin()), res.data(), res.length());
  return kj::mv(ret);
}

}  // namespace

Url::Url(kj::Own<void> inner): inner(kj::mv(inner)) {}

bool Url::operator==(const Url& other) const {
  return getHref() == other.getHref();
}

bool Url::equal(const Url& other, EquivalenceOption option) const {
  if (option == EquivalenceOption::DEFAULT) {
    return *this == other;
  }

  auto otherPathname = other.getPathname();
  auto thisPathname = getPathname();
  kj::Array<const char> otherPathnameStore = nullptr;
  kj::Array<const char> thisPathnameStore = nullptr;

  if ((option & EquivalenceOption::NORMALIZE_PATH) == EquivalenceOption::NORMALIZE_PATH) {
    otherPathnameStore = normalizePathEncoding(otherPathname);
    otherPathname = otherPathnameStore;
    thisPathnameStore = normalizePathEncoding(thisPathname);
    thisPathname = thisPathnameStore;
  }

  // If we are ignoring fragments, we'll compare each component separately:
  return (other.getProtocol() == getProtocol()) && (other.getHost() == getHost()) &&
      (other.getUsername() == getUsername()) && (other.getPassword() == getPassword()) &&
      (otherPathname == thisPathname) &&
      (((option & EquivalenceOption::IGNORE_SEARCH) == EquivalenceOption::IGNORE_SEARCH)
              ? true
              : other.getSearch() == getSearch()) &&
      (((option & EquivalenceOption::IGNORE_FRAGMENTS) == EquivalenceOption::IGNORE_FRAGMENTS)
              ? true
              : other.getHash() == getHash());
}

bool Url::canParse(kj::StringPtr input, kj::Maybe<kj::StringPtr> base) {
  return canParse(kj::ArrayPtr<const char>(input), base);
}

bool Url::canParse(kj::ArrayPtr<const char> input, kj::Maybe<kj::ArrayPtr<const char>> base) {
  KJ_IF_SOME(b, base) {
    return ada_can_parse_with_base(input.begin(), input.size(), b.begin(), b.size());
  }
  return ada_can_parse(input.begin(), input.size());
}

kj::Maybe<Url> Url::tryParse(kj::StringPtr input, kj::Maybe<kj::StringPtr> base) {
  return tryParse(kj::ArrayPtr<const char>(input), base);
}

kj::Maybe<Url> Url::tryParse(
    kj::ArrayPtr<const char> input, kj::Maybe<kj::ArrayPtr<const char>> base) {
  ada_url result = nullptr;
  KJ_IF_SOME(b, base) {
    result = ada_parse_with_base(input.begin(), input.size(), b.begin(), b.size());
  } else {
    result = ada_parse(input.begin(), input.size());
  }
  if (!ada_is_valid(result)) {
    ada_free(result);
    return kj::none;
  }
  return Url(wrap(result));
}

kj::Maybe<Url> Url::resolve(kj::ArrayPtr<const char> input) {
  return tryParse(input, getHref());
}

kj::ArrayPtr<const char> Url::getHref() const {
  ada_string href = ada_get_href(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(href.data, href.length);
}

kj::ArrayPtr<const char> Url::getUsername() const {
  ada_string username = ada_get_username(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(username.data, username.length);
}

kj::ArrayPtr<const char> Url::getPassword() const {
  ada_string password = ada_get_password(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(password.data, password.length);
}

kj::ArrayPtr<const char> Url::getPort() const {
  ada_string port = ada_get_port(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(port.data, port.length);
}

kj::ArrayPtr<const char> Url::getHash() const {
  ada_string hash = ada_get_hash(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(hash.data, hash.length);
}

kj::ArrayPtr<const char> Url::getHost() const {
  ada_string host = ada_get_host(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(host.data, host.length);
}

kj::ArrayPtr<const char> Url::getHostname() const {
  ada_string hostname = ada_get_hostname(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(hostname.data, hostname.length);
}

kj::ArrayPtr<const char> Url::getPathname() const {
  ada_string path = ada_get_pathname(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(path.data, path.length);
}

kj::ArrayPtr<const char> Url::getSearch() const {
  ada_string search = ada_get_search(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(search.data, search.length);
}

kj::ArrayPtr<const char> Url::getProtocol() const {
  ada_string protocol = ada_get_protocol(getInner<ada_url>(inner));
  return kj::ArrayPtr<const char>(protocol.data, protocol.length);
}

kj::Array<const char> Url::getOrigin() const {
  ada_owned_string result = ada_get_origin(getInner<ada_url>(inner));
  return kj::Array<const char>(
      const_cast<char*>(result.data), result.length, AdaOwnedStringDisposer::INSTANCE);
}

bool Url::setHref(kj::ArrayPtr<const char> value) {
  return ada_set_href(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setHost(kj::ArrayPtr<const char> value) {
  return ada_set_host(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setHostname(kj::ArrayPtr<const char> value) {
  return ada_set_hostname(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setProtocol(kj::ArrayPtr<const char> value) {
  return ada_set_protocol(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setUsername(kj::ArrayPtr<const char> value) {
  return ada_set_username(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setPassword(kj::ArrayPtr<const char> value) {
  return ada_set_password(getInner<ada_url>(inner), value.begin(), value.size());
}

bool Url::setPort(kj::Maybe<kj::ArrayPtr<const char>> value) {
  KJ_IF_SOME(v, value) {
    return ada_set_port(getInner<ada_url>(inner), v.begin(), v.size());
  }
  ada_clear_port(getInner<ada_url>(inner));
  return true;
}

bool Url::setPathname(kj::ArrayPtr<const char> value) {
  return ada_set_pathname(getInner<ada_url>(inner), value.begin(), value.size());
}

void Url::setSearch(kj::Maybe<kj::ArrayPtr<const char>> value) {
  KJ_IF_SOME(v, value) {
    return ada_set_search(getInner<ada_url>(inner), v.begin(), v.size());
  }
  ada_clear_search(getInner<ada_url>(inner));
}

void Url::setHash(kj::Maybe<kj::ArrayPtr<const char>> value) {
  KJ_IF_SOME(v, value) {
    return ada_set_hash(getInner<ada_url>(inner), v.begin(), v.size());
  }
  ada_clear_hash(getInner<ada_url>(inner));
}

Url::SchemeType Url::getSchemeType() const {
  uint8_t value = ada_get_scheme_type(const_cast<void*>(getInner<ada_url>(inner)));
  KJ_REQUIRE(value <= static_cast<uint8_t>(SchemeType::FILE));
  return static_cast<SchemeType>(value);
}

Url::HostType Url::getHostType() const {
  uint8_t value = ada_get_host_type(const_cast<void*>(getInner<ada_url>(inner)));
  KJ_REQUIRE(value <= static_cast<uint8_t>(HostType::IPV6));
  return static_cast<HostType>(value);
}

Url Url::clone(EquivalenceOption option) const {
  ada_url copy = ada_copy(getInner<ada_url>(inner));
  if ((option & EquivalenceOption::IGNORE_FRAGMENTS) == EquivalenceOption::IGNORE_FRAGMENTS) {
    ada_clear_hash(copy);
  }
  if ((option & EquivalenceOption::IGNORE_SEARCH) == EquivalenceOption::IGNORE_SEARCH) {
    ada_clear_search(copy);
  }
  if ((option & EquivalenceOption::NORMALIZE_PATH) == EquivalenceOption::NORMALIZE_PATH) {
    auto normalized = normalizePathEncoding(getPathname());
    ada_set_pathname(copy, normalized.begin(), normalized.size());
  }
  return Url(wrap(copy));
}

kj::Array<const char> Url::idnToUnicode(kj::ArrayPtr<const char> value) {
  ada_owned_string result = ada_idna_to_unicode(value.begin(), value.size());
  return kj::Array<const char>(result.data, result.length, AdaOwnedStringDisposer::INSTANCE);
}

kj::Array<const char> Url::idnToAscii(kj::ArrayPtr<const char> value) {
  ada_owned_string result = ada_idna_to_ascii(value.begin(), value.size());
  return kj::Array<const char>(result.data, result.length, AdaOwnedStringDisposer::INSTANCE);
}

kj::Maybe<Url> Url::tryResolve(kj::ArrayPtr<const char> input) const {
  return tryParse(input, getHref());
}

kj::uint Url::hashCode() const {
  return kj::hashCode(getHref());
}

kj::Array<kj::byte> Url::percentDecode(kj::ArrayPtr<const kj::byte> input) {
  std::string_view data(input.asChars().begin(), input.size());
  auto str = ada::unicode::percent_decode(data, 0);
  auto ret = kj::heapArray<kj::byte>(str.size());
  memcpy(ret.begin(), str.data(), str.size());
  return kj::mv(ret);
}

// ======================================================================================

namespace {
kj::Own<void> emptySearchParams() {
  ada_url_search_params result = ada_parse_search_params(nullptr, 0);
  KJ_ASSERT(result);
  return kj::disposeWith<ada_free_search_params>(result);
}
}  // namespace

UrlSearchParams::UrlSearchParams(): inner(emptySearchParams()) {}

UrlSearchParams::UrlSearchParams(kj::Own<void> inner): inner(kj::mv(inner)) {}

bool UrlSearchParams::operator==(const UrlSearchParams& other) const {
  return toStr() == other.toStr();
}

kj::Maybe<UrlSearchParams> UrlSearchParams::tryParse(kj::ArrayPtr<const char> input) {
  ada_url_search_params result = ada_parse_search_params(input.begin(), input.size());
  if (!result) return kj::none;
  return UrlSearchParams(kj::disposeWith<ada_free_search_params>(result));
}

size_t UrlSearchParams::size() const {
  return ada_search_params_size(getInner<ada_url_search_params>(inner));
}

void UrlSearchParams::append(kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value) {
  ada_search_params_append(
      getInner<ada_url_search_params>(inner), key.begin(), key.size(), value.begin(), value.size());
}

void UrlSearchParams::set(kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value) {
  ada_search_params_set(
      getInner<ada_url_search_params>(inner), key.begin(), key.size(), value.begin(), value.size());
}

void UrlSearchParams::delete_(
    kj::ArrayPtr<const char> key, kj::Maybe<kj::ArrayPtr<const char>> maybeValue) {
  KJ_IF_SOME(value, maybeValue) {
    ada_search_params_remove_value(getInner<ada_url_search_params>(inner), key.begin(), key.size(),
        value.begin(), value.size());
  } else {
    ada_search_params_remove(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  }
}

bool UrlSearchParams::has(
    kj::ArrayPtr<const char> key, kj::Maybe<kj::ArrayPtr<const char>> maybeValue) const {
  KJ_IF_SOME(value, maybeValue) {
    return ada_search_params_has_value(getInner<ada_url_search_params>(inner), key.begin(),
        key.size(), value.begin(), value.size());
  } else {
    return ada_search_params_has(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  }
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::get(kj::ArrayPtr<const char> key) const {
  auto result =
      ada_search_params_get(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  if (result.data == nullptr) return kj::none;
  return kj::ArrayPtr<const char>(result.data, result.length);
}

kj::Array<kj::ArrayPtr<const char>> UrlSearchParams::getAll(kj::ArrayPtr<const char> key) const {
  ada_strings results =
      ada_search_params_get_all(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  size_t size = ada_strings_size(results);
  kj::Vector<kj::ArrayPtr<const char>> items(size);
  for (size_t n = 0; n < size; n++) {
    auto item = ada_strings_get(results, n);
    items.add(kj::ArrayPtr<const char>(item.data, item.length));
  }
  return items.releaseAsArray().attach(kj::defer([results]() { ada_free_strings(results); }));
}

void UrlSearchParams::sort() {
  ada_search_params_sort(getInner<ada_url_search_params>(inner));
}

UrlSearchParams::KeyIterator UrlSearchParams::getKeys() const {
  return KeyIterator(kj::disposeWith<ada_free_search_params_keys_iter>(
      ada_search_params_get_keys(getInner<ada_url_search_params>(inner))));
}

UrlSearchParams::ValueIterator UrlSearchParams::getValues() const {
  return ValueIterator(kj::disposeWith<ada_free_search_params_values_iter>(
      ada_search_params_get_values(getInner<ada_url_search_params>(inner))));
}

UrlSearchParams::EntryIterator UrlSearchParams::getEntries() const {
  return EntryIterator(kj::disposeWith<ada_free_search_params_entries_iter>(
      ada_search_params_get_entries(getInner<ada_url_search_params>(inner))));
}

kj::Array<const char> UrlSearchParams::toStr() const {
  ada_owned_string result = ada_search_params_to_string(getInner<ada_url_search_params>(inner));
  return kj::Array<const char>(result.data, result.length, AdaOwnedStringDisposer::INSTANCE);
}

UrlSearchParams::KeyIterator::KeyIterator(kj::Own<void> inner): inner(kj::mv(inner)) {}

bool UrlSearchParams::KeyIterator::hasNext() const {
  return ada_search_params_keys_iter_has_next(getInner<ada_url_search_params_keys_iter>(inner));
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::KeyIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next = ada_search_params_keys_iter_next(getInner<ada_url_search_params_keys_iter>(inner));
  return kj::ArrayPtr<const char>(next.data, next.length);
}

UrlSearchParams::ValueIterator::ValueIterator(kj::Own<void> inner): inner(kj::mv(inner)) {}

bool UrlSearchParams::ValueIterator::hasNext() const {
  return ada_search_params_values_iter_has_next(getInner<ada_url_search_params_values_iter>(inner));
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::ValueIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next =
      ada_search_params_values_iter_next(getInner<ada_url_search_params_values_iter>(inner));
  return kj::ArrayPtr<const char>(next.data, next.length);
}

UrlSearchParams::EntryIterator::EntryIterator(kj::Own<void> inner): inner(kj::mv(inner)) {}

bool UrlSearchParams::EntryIterator::hasNext() const {
  return ada_search_params_entries_iter_has_next(
      getInner<ada_url_search_params_entries_iter>(inner));
}

kj::Maybe<UrlSearchParams::EntryIterator::Entry> UrlSearchParams::EntryIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next =
      ada_search_params_entries_iter_next(getInner<ada_url_search_params_entries_iter>(inner));
  return Entry{
    .key = kj::ArrayPtr<const char>(next.key.data, next.key.length),
    .value = kj::ArrayPtr<const char>(next.value.data, next.value.length),
  };
}

// ======================================================================================
// UrlPattern

namespace {

constexpr auto MODIFIER_OPTIONAL = "?"_kjc;
constexpr auto MODIFIER_ZERO_OR_MORE = "*"_kjc;
constexpr auto MODIFIER_ONE_OR_MORE = "+"_kjc;

inline bool isAsciiDigit(char c) {
  return c >= '0' && c <= '9';
};

inline bool isHexDigit(char c) {
  // Check if `c` is the ASCII code of a hexadecimal digit.
  return isAsciiDigit(c) || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F');
}

inline bool isAscii(char codepoint) {
  return codepoint >= 0x00 && codepoint <= 0x7f;
};

inline bool isForbiddenHostCodepoint(char c) {
  return c == 0x00 || c == 0x09 /* Tab */ || c == 0x0a /* LF */ || c == 0x0d /* CR */ || c == ' ' ||
      c == '#' || c == '%' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' ||
      c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|';
};

// This is not meant to be a comprehensive validation that the hostname is
// a proper IPv6 address. It's a quick check defined by the URLPattern spec.
inline bool isIpv6(kj::ArrayPtr<const char> hostname) {
  if (hostname.size() < 2) return false;
  auto c1 = hostname[0];
  auto c2 = hostname[1];
  return (c1 == '[' || ((c1 == '{' || c1 == '\\') && c2 == '['));
}

// This additional check deals with a known bug in the URLPattern spec. The URL parser will
// allow (and generally ignore) invalid characters in the hostname when running with the
// HOST state override. The URLPattern spec, however, assumes that it doesn't.
inline bool isValidHostnameInput(kj::StringPtr input) {
  return isIpv6(input) || std::none_of(input.begin(), input.end(), isForbiddenHostCodepoint);
}

inline bool isValidCodepoint(uint32_t codepoint, bool first) {
  // https://tc39.es/ecma262/#prod-IdentifierStart
  if (first) {
    return codepoint == '$' || codepoint == '_' || u_hasBinaryProperty(codepoint, UCHAR_ID_START);
  }
  return codepoint == '$' || codepoint == 0x200C ||  // Zero-width non-joiner
      codepoint == 0x200D ||                         // Zero-width joiner
      u_hasBinaryProperty(codepoint, UCHAR_ID_CONTINUE);
};

inline kj::Maybe<kj::String> strFromMaybePtr(const kj::Maybe<kj::StringPtr>& ptr) {
  return ptr.map([](const kj::StringPtr& ptr) { return kj::str(ptr); });
}

using Canonicalizer = kj::Maybe<kj::String>(kj::StringPtr, kj::Maybe<kj::StringPtr>);

kj::Maybe<kj::String> canonicalizeProtocol(
    kj::StringPtr protocol, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-protocol
  if (protocol.size() == 0) return kj::str();
  auto input = kj::str(protocol, "://dummy.test");
  KJ_IF_SOME(url, Url::tryParse(input.asPtr())) {
    auto result = url.getProtocol();
    return kj::str(result.slice(0, result.size() - 1));
  }
  return kj::none;
}

kj::Maybe<kj::String> canonicalizeUsername(
    kj::StringPtr username, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-username
  if (username.size() == 0) return kj::str();
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("fake://dummy.test"_kj));
  if (!url.setUsername(username)) return kj::none;
  return kj::str(url.getUsername());
}

kj::Maybe<kj::String> canonicalizePassword(
    kj::StringPtr password, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-password
  if (password.size() == 0) return kj::str();
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("fake://dummy.test"_kj));
  if (!url.setPassword(password)) return kj::none;
  return kj::str(url.getPassword());
}

kj::Maybe<kj::String> canonicalizeHostname(
    kj::StringPtr hostname, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-hostname
  if (hostname.size() == 0) return kj::str();
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("fake://dummy.test"_kj));
  if (!isValidHostnameInput(hostname)) return kj::none;
  if (!url.setHostname(hostname)) return kj::none;
  return kj::str(url.getHostname());
}

kj::Maybe<kj::String> canonicalizeIpv6Hostname(
    kj::StringPtr hostname, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-an-ipv6-hostname
  if (!std::all_of(hostname.begin(), hostname.end(),
          [](char c) { return isHexDigit(c) || c == '[' || c == ']' || c == ':'; })) {
    return kj::none;
  }
  return kj::str(hostname);
}

kj::Maybe<kj::String> canonicalizePort(kj::StringPtr port, kj::Maybe<kj::StringPtr> protocol) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-port
  if (port.size() == 0) return kj::str();
  auto input = kj::str(protocol.orDefault("fake"_kj), "://dummy.test");
  KJ_IF_SOME(url, Url::tryParse(input.asPtr())) {
    if (!url.setPort(kj::Maybe(port))) return kj::none;
    return kj::str(url.getPort());
  }
  return kj::none;
}

kj::Maybe<kj::String> canonicalizePathname(
    kj::StringPtr pathname, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-pathname
  if (pathname.size() == 0) return kj::str();
  bool leadingSlash = pathname[0] == '/';
  auto input = kj::str("fake://fake-url", leadingSlash ? "" : "/-", pathname);
  KJ_IF_SOME(url, Url::tryParse(input.asPtr())) {
    auto result = url.getPathname();
    return leadingSlash ? kj::str(result) : kj::str(result.slice(2));
  }
  return kj::none;
}

kj::Maybe<kj::String> canonicalizeOpaquePathname(
    kj::StringPtr pathname, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-an-opaque-pathname
  if (pathname.size() == 0) return kj::str();
  auto str = kj::str("fake:", pathname);
  auto url = KJ_ASSERT_NONNULL(Url::tryParse(str.asPtr()));
  return kj::str(url.getPathname());
}

kj::Maybe<kj::String> canonicalizeSearch(
    kj::StringPtr search, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-search
  if (search.size() == 0) return kj::str();
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("fake://dummy.test"_kj));
  url.setSearch(kj::Maybe(search));
  return url.getSearch().size() > 0 ? kj::str(url.getSearch().slice(1)) : kj::str();
}

kj::Maybe<kj::String> canonicalizeHash(kj::StringPtr hash, kj::Maybe<kj::StringPtr> = kj::none) {
  // @see https://wicg.github.io/urlpattern/#canonicalize-a-hash
  if (hash.size() == 0) return kj::str();
  auto url = KJ_ASSERT_NONNULL(Url::tryParse("fake://dummy.test"_kj));
  url.setHash(kj::Maybe(hash));
  return url.getHash().size() > 0 ? kj::str(url.getHash().slice(1)) : kj::str();
}

kj::Maybe<kj::String> chooseStr(kj::Maybe<kj::String> str, kj::Maybe<kj::StringPtr> other) {
  KJ_IF_SOME(s, str) {
    return kj::mv(s);
  } else {
    return strFromMaybePtr(other);
  }
}

kj::String stripSuffixFromProtocol(kj::ArrayPtr<const char> data) {
  if (data.back() == ':') {
    return kj::str(data.slice(0, data.size() - 1));
  }
  return kj::str(data);
}

kj::String escape(kj::ArrayPtr<const char> str, auto predicate) {
  // Best case we don't have to escape anything so size remains the same,
  // but let's pad a little just in case.
  kj::Vector<char> result(str.size() + 10);
  auto it = str.begin();
  while (it != str.end()) {
    auto c = *it;
    if (predicate(c)) result.add('\\');
    result.add(c);
    ++it;
  }
  result.add('\0');
  return kj::String(result.releaseAsArray());
}

kj::String escapeRegexString(kj::ArrayPtr<const char> str) {
  return escape(str, [](auto c) {
    return c == '.' || c == '+' || c == '*' || c == '?' || c == '^' || c == '$' || c == '{' ||
        c == '}' || c == '(' || c == ')' || c == '[' || c == ']' || c == '|' || c == '/' ||
        c == '\\';
  });
}

kj::String escapePatternString(kj::ArrayPtr<const char> str) {
  return escape(str, [](auto c) {
    return c == '+' || c == '*' || c == '?' || c == ':' || c == '{' || c == '}' || c == '(' ||
        c == ')' || c == '\\';
  });
}

struct CompileComponentOptions {
  kj::Maybe<char> delimiter;
  kj::Maybe<char> prefix;
  kj::String segmentWildcardRegexp;

  kj::String initSegmentWildcardRegexp() {
    KJ_IF_SOME(c, delimiter) {
      return kj::str("[^\\", c, "]+");
    } else {
      return kj::str("[^]+");
    }
  }

  CompileComponentOptions(kj::Maybe<char> delimiter, kj::Maybe<char> prefix)
      : delimiter(delimiter),
        prefix(prefix),
        segmentWildcardRegexp(initSegmentWildcardRegexp()) {}

  static const CompileComponentOptions DEFAULT;
  static const CompileComponentOptions HOSTNAME;
  static const CompileComponentOptions PATHNAME;
};
const CompileComponentOptions CompileComponentOptions::DEFAULT(kj::none, kj::none);
const CompileComponentOptions CompileComponentOptions::HOSTNAME('.', kj::none);
const CompileComponentOptions CompileComponentOptions::PATHNAME('/', '/');

// An individual piece of a URLPattern string. Used while parsing a URLPattern
// string for the URLPattern constructor, test, or exec call.
struct Part {
  enum class Type {
    FIXED_TEXT,
    REGEXP,
    SEGMENT_WILDCARD,
    FULL_WILDCARD,
  };

  enum class Modifier {
    NONE,
    OPTIONAL,      // ?
    ZERO_OR_MORE,  // *
    ONE_OR_MORE,   // +
  };

  Type type;
  Modifier modifier;
  kj::String value;
  kj::String name;
  kj::Maybe<kj::String> prefix;
  kj::Maybe<kj::String> suffix;
};

kj::Maybe<kj::StringPtr> modifierToString(const Part::Modifier& modifier) {
  switch (modifier) {
    case Part::Modifier::NONE:
      return kj::none;
    case Part::Modifier::OPTIONAL:
      return MODIFIER_OPTIONAL;
    case Part::Modifier::ZERO_OR_MORE:
      return MODIFIER_ZERO_OR_MORE;
    case Part::Modifier::ONE_OR_MORE:
      return MODIFIER_ONE_OR_MORE;
  }
  KJ_UNREACHABLE;
}

// String inputs passed into URLPattern constructor are parsed by first
// interpreting them into a list of Tokens. Each token has a type, a
// position index in the input string, and a value. The value is either
// a individual codepoint or a substring of input. Once the tokens are
// determined, the parsing algorithms convert those into a Part list.
// The part list is then used to generate the internal JavaScript RegExps
// that are used for the actual matching operation.
struct Token {
  // Per the URLPattern spec, the tokenizer runs in one of two modes:
  // Strict and Lenient. In Strict mode, invalid characters and sequences
  // detected by the tokenizer will cause a TypeError to be thrown.
  // In lenient mode, the invalid codepoints and sequences are marked
  // but no error is thrown. When parsing a string passed to the
  // URLPattern constructor, lenient mode is used. When parsing the
  // pattern string for an individual component, strict mode is used.
  enum class Policy {
    STRICT,
    LENIENT,
  };

  enum class Type {
    INVALID_CHAR,    // 0
    OPEN,            // 1
    CLOSE,           // 2
    REGEXP,          // 3
    NAME,            // 4
    CHAR,            // 5
    ESCAPED_CHAR,    // 6
    OTHER_MODIFIER,  // 7
    ASTERISK,        // 8
    END,             // 9
  };

  Type type = Type::INVALID_CHAR;
  size_t index = 0;
  kj::OneOf<char, kj::ArrayPtr<const char>> value = (char)0;
  Part::Modifier modifier = Part::Modifier::NONE;

  operator kj::String() const {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, char) {
        return kj::str(codepoint);
      }
      KJ_CASE_ONEOF(ptr, kj::ArrayPtr<const char>) {
        return kj::str(ptr);
      }
    }
    KJ_UNREACHABLE;
  }

  bool operator==(const kj::String& other) const {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, char) {
        return false;
      }
      KJ_CASE_ONEOF(string, kj::ArrayPtr<const char>) {
        return other == string;
      }
    }
    KJ_UNREACHABLE;
  }

  bool operator==(char other) {
    KJ_SWITCH_ONEOF(value) {
      KJ_CASE_ONEOF(codepoint, char) {
        return codepoint == other;
      }
      KJ_CASE_ONEOF(string, kj::ArrayPtr<const char>) {
        return false;
      }
    }
    KJ_UNREACHABLE;
  }

  static Token asterisk(size_t index) {
    return {
      .type = Type::ASTERISK,
      .index = index,
      .value = '*',
      .modifier = Part::Modifier::ZERO_OR_MORE,
    };
  }

  static Token char_(size_t index, char codepoint) {
    return {
      .type = Type::CHAR,
      .index = index,
      .value = codepoint,
    };
  }

  static Token close(size_t index) {
    return {
      .type = Type::CLOSE,
      .index = index,
    };
  }

  static Token end(size_t index) {
    return {
      .type = Type::END,
      .index = index,
    };
  }

  static Token escapedChar(size_t index, char codepoint) {
    return {
      .type = Type::ESCAPED_CHAR,
      .index = index,
      .value = codepoint,
    };
  }

  static Token invalidChar(size_t index, char codepoint) {
    return {
      .index = index,
      .value = codepoint,
    };
  }

  static Token invalidSegment(size_t index, kj::ArrayPtr<const char> segment) {
    return {
      .type = Type::INVALID_CHAR,
      .index = index,
      .value = segment,
    };
  }

  static Token name(size_t index, kj::ArrayPtr<const char> name) {
    return {
      .type = Type::NAME,
      .index = index,
      .value = name,
    };
  }

  static Token open(size_t index) {
    return {
      .type = Type::OPEN,
      .index = index,
    };
  }

  static Token otherModifier(size_t index, char codepoint) {
    KJ_DASSERT(codepoint == '?' || codepoint == '+');
    return {
      .type = Type::OTHER_MODIFIER,
      .index = index,
      .value = codepoint,
      .modifier = codepoint == '?' ? Part::Modifier::OPTIONAL : Part::Modifier::ONE_OR_MORE,
    };
  }

  static Token regex(size_t index, kj::ArrayPtr<const char> regex) {
    return {
      .type = Type::REGEXP,
      .index = index,
      .value = regex,
    };
  }
};

struct RegexAndNameList {
  kj::String regex;
  kj::Array<kj::String> names;
};

UrlPattern::Result<kj::Array<Token>> tokenize(kj::StringPtr input, Token::Policy policy) {
  auto it = input.begin();
  size_t pos = 0;
  kj::Vector<Token> tokenList(input.size() + 1);
  // Scan the input and advance both it and pos until the given predicate return false.
  const auto scanCodepoints = [&](auto predicate) {
    bool first = true;
    while (it != input.end()) {
      uint32_t codepoint;
      size_t starting = pos;
      // Reads to the next codepoint boundary, incrementing pos accordingly.
      // We use U8_NEXT_OR_FFFD here because the input is a raw sequence of
      // UTF8 bytes but the predicate needs to check the decoded codepoint
      // rather than looking at individual bytes. The macro will advance pos
      // at is scans.
      U8_NEXT_OR_FFFD(input.begin(), pos, input.size(), codepoint);
      KJ_DASSERT(pos <= input.size());
      // If our read codepoint does not match the predicate, we do not want to
      // advance and we stop scanning.
      if (!predicate(codepoint, first)) {
        pos = starting;
        break;
      }
      it += pos - starting;
      first = false;
    }
  };

  while (it != input.end()) {
    auto c = *it;
    switch (c) {
      case '*': {
        tokenList.add(Token::asterisk(pos++));
        break;
      }
      case '?': {
        KJ_FALLTHROUGH;
      }
      case '+': {
        tokenList.add(Token::otherModifier(pos++, c));
        break;
      }
      case '\\': {
        ++it;
        // The escape character is invalid if it comes at the end!
        if (it == input.end()) {
          if (policy == Token::Policy::STRICT) {
            return kj::str("Syntax error in URL Pattern: invalid escape character at ", pos);
          }
          tokenList.add(Token::invalidChar(pos++, c));
        } else {
          tokenList.add(Token::escapedChar(pos, *it));
          pos += 2;
        }
        break;
      }
      case '{': {
        tokenList.add(Token::open(pos++));
        break;
      }
      case '}': {
        tokenList.add(Token::close(pos++));
        break;
      }
      case ':': {
        ++it;
        // The name token is invalid if it comes at the end!
        if (it == input.end()) {
          if (policy == Token::Policy::STRICT) {
            return kj::str("Syntax error in URL Pattern: invalid name start at ", pos);
          }
          tokenList.add(Token::invalidChar(pos++, c));
          break;
        }
        auto start = ++pos;
        scanCodepoints(isValidCodepoint);
        if (start == pos) {
          // There was a name token suffix without a valid name! Oh, the inhumanity of it all.
          if (policy == Token::Policy::STRICT) {
            return kj::str("Syntax error in URL Pattern: invalid name start at ", pos - 1);
          }
          tokenList.add(Token::invalidChar(pos - 1, c));
        } else {
          if (it == input.end()) {
            tokenList.add(Token::name(start - 1, input.slice(start)));
          } else {
            tokenList.add(Token::name(start - 1, input.slice(start, pos)));
          }
        }
        // We purposefully do not increment the iterator here because we are
        // already at the next position.

        continue;
      }
      case '(': {
        ++it;
        // The group token is invalid if it comes at the end!
        if (it == input.end()) {
          if (policy == Token::Policy::STRICT) {
            return kj::str("Syntax error in URL Pattern: invalid regex start at ", pos);
          }
          tokenList.add(Token::invalidChar(pos++, c));
          break;
        }
        size_t depth = 1;
        size_t start = ++pos;
        bool error = false;
        while (it != input.end()) {
          auto rc = *it;
          if (!isAscii(rc)) {
            if (policy == Token::Policy::STRICT) {
              return kj::str("Syntax error in URL Pattern: invalid regex character at ", pos);
            }
            tokenList.add(Token::invalidChar(pos, rc));
            error = true;
            break;
          } else if (pos == start && rc == '?') {
            if (policy == Token::Policy::STRICT) {
              return kj::str("Syntax error in URL Pattern: invalid regex character at ", pos);
            }
            tokenList.add(Token::invalidChar(pos, rc));
            error = true;
            break;
          } else if (rc == '\\') {
            it++;
            // The escape character is invalid if it comes at the end of input
            if (it == input.end()) {
              if (policy == Token::Policy::STRICT) {
                return kj::str(
                    "Syntax error in URL Pattern: invalid escape character in regex at ", pos);
              }
              tokenList.add(Token::invalidChar(pos, rc));
              error = true;
              break;
            }
            pos++;
            rc = *it;
            if (!isAscii(rc)) {
              if (policy == Token::Policy::STRICT) {
                return kj::str(
                    "Syntax error in URL Pattern: invalid escaped character in regex at ", pos);
              }
              tokenList.add(Token::invalidChar(pos, rc));
              error = true;
              break;
            }
            pos++;
            it++;
            continue;
          } else if (rc == ')') {
            depth--;
            if (depth == 0) {
              pos++;
              it++;
              break;
            }
          } else if (rc == '(') {
            depth++;
            it++;
            // The group open character is invalid if it comes at the end of input
            if (it == input.end()) {
              if (policy == Token::Policy::STRICT) {
                return kj::str("Syntax error in URL Pattern: invalid group in regex at ", pos);
              }
              tokenList.add(Token::invalidChar(pos, rc));
              error = true;
              break;
            }
            pos++;
            rc = *it;
            if (rc != '?') {
              if (policy == Token::Policy::STRICT) {
                return kj::str("Syntax error in URL Pattern: invalid group in regex at ", pos);
              }
              tokenList.add(Token::invalidChar(pos, rc));
              error = true;
              break;
            }
          }
          it++;
          pos++;
        }
        if (error) continue;
        if (depth > 0 || start == pos) {
          if (policy == Token::Policy::STRICT) {
            return kj::str("Syntax error in URL Pattern: invalid regex segment at ", start);
          }
          tokenList.add(Token::invalidSegment(start, input.slice(start, pos - 1)));
        } else {
          tokenList.add(Token::regex(start - 1, input.slice(start, pos - 1)));
        }
        // We purposefully do not increment the iterator here because we are
        // already at the next position.
        continue;
      }
      default: {
        tokenList.add(Token::char_(pos++, c));
        break;
      }
    }
    if (it == input.end()) break;
    ++it;
  }

  tokenList.add(Token::end(input.size()));
  return tokenList.releaseAsArray();
}

UrlPattern::Result<kj::Array<Part>> parsePattern(
    kj::StringPtr input, Canonicalizer canonicalizer, const CompileComponentOptions& options) {
  kj::Array<Token> tokens = nullptr;
  KJ_SWITCH_ONEOF(tokenize(input, Token::Policy::STRICT)) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(list, kj::Array<Token>) {
      tokens = kj::mv(list);
    }
  }
  // There should be at least one token in the list (the end token)
  KJ_DASSERT(tokens.size() > 0);
  kj::Vector<Part> partList(tokens.size());
  kj::Maybe<kj::StringTree> pendingFixedValue;
  size_t index = 0;
  size_t nextNumericName = 0;

  auto segmentWildcardRegex = options.segmentWildcardRegexp.asPtr();

  auto appendToPendingFixedValue = [&](kj::StringPtr value) mutable {
    KJ_IF_SOME(pending, pendingFixedValue) {
      pendingFixedValue = kj::strTree(kj::mv(pending), value);
    } else {
      pendingFixedValue = kj::strTree(kj::mv(value));
    }
  };

  auto maybeAddPartFromPendingFixedValue = [&]() mutable -> bool {
    KJ_IF_SOME(fixedValue, pendingFixedValue) {
      auto value = fixedValue.flatten();
      pendingFixedValue = kj::none;
      if (value.size() == 0) return true;
      KJ_IF_SOME(canonical, canonicalizer(value, kj::none)) {
        partList.add(Part{
          .type = Part::Type::FIXED_TEXT,
          .modifier = Part::Modifier::NONE,
          .value = kj::mv(canonical),
        });
        return true;
      }
      return false;
    }
    return true;
  };

  auto tryConsumeToken = [&](Token::Type type) -> kj::Maybe<Token&> {
    KJ_DASSERT(index < tokens.size());
    auto& next = tokens[index];
    if (next.type != type) {
      return kj::none;
    }
    index++;
    return kj::Maybe<Token&>(next);
  };

  auto tryConsumeRegexOrWildcardToken = [&](kj::Maybe<Token&>& nameToken) {
    auto token = tryConsumeToken(Token::Type::REGEXP);
    if (nameToken == kj::none && token == kj::none) {
      token = tryConsumeToken(Token::Type::ASTERISK);
    }
    return token;
  };

  auto tryConsumeModifierToken = [&]() -> kj::Maybe<Token&> {
    KJ_IF_SOME(token, tryConsumeToken(Token::Type::OTHER_MODIFIER)) {
      return kj::Maybe<Token&>(token);
    }
    return tryConsumeToken(Token::Type::ASTERISK);
  };

  auto consumeText = [&]() mutable -> kj::String {
    kj::StringTree result = kj::strTree();
    while (true) {
      KJ_IF_SOME(token, tryConsumeToken(Token::Type::CHAR)) {
        result = kj::strTree(kj::mv(result), kj::String(token));
      } else KJ_IF_SOME(token, tryConsumeToken(Token::Type::ESCAPED_CHAR)) {
        result = kj::strTree(kj::mv(result), kj::String(token));
      } else {
        break;
      }
    }
    return result.flatten();
  };

  auto isDuplicateName = [&](kj::StringPtr name) -> bool {
    return std::any_of(
        partList.begin(), partList.end(), [&name](Part& part) { return part.name == name; });
  };

  auto maybeTokenToModifier = [](kj::Maybe<Token&> modifierToken) -> Part::Modifier {
    KJ_IF_SOME(token, modifierToken) {
      KJ_DASSERT(token.type == Token::Type::OTHER_MODIFIER || token.type == Token::Type::ASTERISK);
      return token.modifier;
    }
    return Part::Modifier::NONE;
  };

  auto addPart = [&](kj::Maybe<kj::String> maybePrefix, kj::Maybe<Token&> nameToken,
                     kj::Maybe<Token&> regexOrWildcardToken, kj::Maybe<kj::String> suffix,
                     kj::Maybe<Token&> modifierToken) mutable -> kj::Maybe<kj::String> {
    auto modifier = maybeTokenToModifier(modifierToken);
    if (nameToken == kj::none && regexOrWildcardToken == kj::none &&
        modifier == Part::Modifier::NONE) {
      KJ_IF_SOME(prefix, maybePrefix) {
        appendToPendingFixedValue(prefix);
      }
      return kj::none;
    }
    if (!maybeAddPartFromPendingFixedValue()) {
      return kj::str("Syntax error in URL Pattern");
    }
    if (nameToken == kj::none && regexOrWildcardToken == kj::none) {
      KJ_DASSERT(suffix == kj::none || KJ_ASSERT_NONNULL(suffix).size() == 0);
      KJ_IF_SOME(prefix, maybePrefix) {
        if (prefix.size() > 0) {
          KJ_IF_SOME(canonical, canonicalizer(prefix, kj::none)) {
            partList.add(Part{
              .type = Part::Type::FIXED_TEXT,
              .modifier = modifier,
              .value = kj::mv(canonical),
            });
          } else {
            return kj::str("Syntax error in URL Pattern");
          }
        }
      }
      return kj::none;
    }
    auto regexValue = kj::str();
    KJ_IF_SOME(token, regexOrWildcardToken) {
      if (token.type == Token::Type::ASTERISK) {
        regexValue = kj::str(".*");
      } else {
        regexValue = kj::String(token);
      }
    } else {
      regexValue = kj::str(segmentWildcardRegex);
    }
    auto type = Part::Type::REGEXP;
    if (regexValue == segmentWildcardRegex) {
      type = Part::Type::SEGMENT_WILDCARD;
      regexValue = kj::str();
    } else if (regexValue == ".*") {
      type = Part::Type::FULL_WILDCARD;
      regexValue = kj::str();
    }
    auto name = kj::str();
    KJ_IF_SOME(token, nameToken) {
      name = kj::String(token);
    } else if (regexOrWildcardToken != kj::none) {
      name = kj::str(nextNumericName++);
    }

    if (isDuplicateName(name)) {
      return kj::str("Syntax error in URL Pattern: Duplicated part names [", name, "]");
    }

    kj::Maybe<kj::String> encodedPrefix;
    kj::Maybe<kj::String> encodedSuffix;
    KJ_IF_SOME(prefix, maybePrefix) {
      KJ_IF_SOME(canonical, canonicalizer(prefix, kj::none)) {
        encodedPrefix = kj::mv(canonical);
      } else {
        return kj::str("Syntax error in URL Pattern");
      }
    }
    KJ_IF_SOME(s, suffix) {
      KJ_IF_SOME(canonical, canonicalizer(s, kj::none)) {
        encodedSuffix = kj::mv(canonical);
      } else {
        return kj::str("Syntax error in URL Pattern");
      }
    }

    partList.add(Part{
      .type = type,
      .modifier = modifier,
      .value = kj::mv(regexValue),
      .name = kj::mv(name),
      .prefix = kj::mv(encodedPrefix),
      .suffix = kj::mv(encodedSuffix),
    });

    return kj::none;
  };

  while (index < tokens.size()) {
    kj::Maybe<Token&> charToken = tryConsumeToken(Token::Type::CHAR);
    kj::Maybe<Token&> nameToken = tryConsumeToken(Token::Type::NAME);
    auto regexOrWildcardToken = tryConsumeRegexOrWildcardToken(nameToken);

    if (nameToken != kj::none || regexOrWildcardToken != kj::none) {
      auto maybePrefix = charToken.map([](Token& token) { return kj::String(token); });

      KJ_IF_SOME(prefix, maybePrefix) {
        if (prefix.size() > 0) {
          KJ_IF_SOME(c, options.prefix) {
            kj::String s;
            if (prefix[0] != c) {
              appendToPendingFixedValue(prefix);
              maybePrefix = kj::none;
            }
          } else {
            // If prefix is not empty, and is not the prefixCodePoint
            // (which it can't be if we're here given that there is
            // no prefix char), when we append prefix to pendingFixedValue,
            // and clear prefix.
            appendToPendingFixedValue(prefix);
            maybePrefix = kj::none;
          }
        }
      }
      if (!maybeAddPartFromPendingFixedValue()) {
        return kj::str("Syntax error in URL Pattern");
      }
      auto modifierToken = tryConsumeModifierToken();
      KJ_IF_SOME(err,
          addPart(kj::mv(maybePrefix), nameToken, regexOrWildcardToken, kj::none, modifierToken)) {
        return kj::mv(err);
      }
      continue;
    }

    kj::Maybe<Token&> fixedToken = charToken;
    if (fixedToken == kj::none) {
      fixedToken = tryConsumeToken(Token::Type::ESCAPED_CHAR);
    }
    KJ_IF_SOME(token, fixedToken) {
      appendToPendingFixedValue(kj::String(token));
      continue;
    }
    if (tryConsumeToken(Token::Type::OPEN) != kj::none) {
      auto maybePrefix = consumeText();
      auto nameToken = tryConsumeToken(Token::Type::NAME);
      regexOrWildcardToken = tryConsumeRegexOrWildcardToken(nameToken);
      auto suffix = consumeText();
      if (tryConsumeToken(Token::Type::CLOSE) == kj::none) {
        return kj::str("Syntax error in URL Pattern: Missing required close token");
      }
      auto modifierToken = tryConsumeModifierToken();
      KJ_IF_SOME(err,
          addPart(kj::mv(maybePrefix), nameToken, regexOrWildcardToken, kj::mv(suffix),
              modifierToken)) {
        return kj::mv(err);
      }
      continue;
    }
    if (!maybeAddPartFromPendingFixedValue()) {
      return kj::str("Syntax error in URL Pattern");
    }

    if (tryConsumeToken(Token::Type::END) == kj::none) {
      return kj::str("Syntax error in URL Pattern: Missing required end token");
    }
  }

  return partList.releaseAsArray();
}

RegexAndNameList generateRegexAndNameList(
    kj::ArrayPtr<Part> partList, const CompileComponentOptions& options) {
  // Worst case is that the nameList is equal to partList, although that will almost never
  // be the case, so let's be more conservative in what we reserve.
  kj::Vector<kj::String> nameList(partList.size() / 2);
  auto regex = kj::strTree("^");

  for (auto& part: partList) {
    if (part.type == Part::Type::FIXED_TEXT) {
      auto escaped = escapeRegexString(part.value);
      if (part.modifier == Part::Modifier::NONE) {
        regex = kj::strTree(kj::mv(regex), kj::mv(escaped));
      } else {
        regex = kj::strTree(kj::mv(regex), "(?:", kj::mv(escaped), ")");
        KJ_IF_SOME(c, modifierToString(part.modifier)) {
          regex = kj::strTree(kj::mv(regex), c);
        }
      }
      continue;
    }

    KJ_DASSERT(part.name.size() > 0);
    nameList.add(kj::mv(part.name));
    auto value = part.type == Part::Type::SEGMENT_WILDCARD ? kj::str(options.segmentWildcardRegexp)
        : part.type == Part::Type::FULL_WILDCARD           ? kj::str(".*")
                                                           : kj::mv(part.value);

    if (part.prefix == kj::none && part.suffix == kj::none) {
      if (part.modifier == Part::Modifier::NONE || part.modifier == Part::Modifier::OPTIONAL) {
        regex = kj::strTree(kj::mv(regex), "(", value, ")");
        KJ_IF_SOME(c, modifierToString(part.modifier)) {
          regex = kj::strTree(kj::mv(regex), c);
        }
      } else {
        regex = kj::strTree(kj::mv(regex), "((?:", value, ")");
        KJ_IF_SOME(c, modifierToString(part.modifier)) {
          regex = kj::strTree(kj::mv(regex), c, ")");
        } else {
          regex = kj::strTree(kj::mv(regex), ")");
        }
      }
      continue;
    }

    auto escapedPrefix = part.prefix.map([](kj::String& str) {
      return escapeRegexString(str);
    }).orDefault(kj::str());
    auto escapedSuffix = part.suffix.map([](kj::String& str) {
      return escapeRegexString(str);
    }).orDefault(kj::str());

    if (part.modifier == Part::Modifier::NONE || part.modifier == Part::Modifier::OPTIONAL) {
      regex = kj::strTree(kj::mv(regex), "(?:", escapedPrefix, "(", value, ")", escapedSuffix, ")");
      KJ_IF_SOME(c, modifierToString(part.modifier)) {
        regex = kj::strTree(kj::mv(regex), c);
      }
      continue;
    }

    regex = kj::strTree(kj::mv(regex), "(?:", escapedPrefix, "((?:", value, ")(?:", escapedSuffix,
        escapedPrefix, "(?:", value, "))*)", escapedSuffix, ")");
    if (part.modifier == Part::Modifier::ZERO_OR_MORE) {
      regex = kj::strTree(kj::mv(regex), MODIFIER_ZERO_OR_MORE);
    }
  }

  regex = kj::strTree(kj::mv(regex), "$");

  return RegexAndNameList{
    .regex = regex.flatten(),
    .names = nameList.releaseAsArray(),
  };
}

kj::String generatePatternString(
    kj::ArrayPtr<Part> partList, const CompileComponentOptions& options) {
  auto pattern = kj::strTree();
  Part* previousPart = nullptr;
  Part* nextPart = nullptr;
  bool customName = false;
  bool needsGrouping = false;
  bool prefixIsEmpty = false;

  const auto partPrefixEmpty = [](Part* part) {
    if (part == nullptr) return true;
    KJ_IF_SOME(prefix, part->prefix) {
      return prefix.size() == 0;
    }
    return true;
  };

  const auto partSuffixEmpty = [](Part* part) {
    if (part == nullptr) return true;
    KJ_IF_SOME(prefix, part->suffix) {
      return prefix.size() == 0;
    }
    return true;
  };

  const auto partSuffixIsValid = [&](Part* part) {
    if (partSuffixEmpty(part)) return false;
    auto& suffix = KJ_ASSERT_NONNULL(part->suffix);
    return isValidCodepoint(suffix[0], false);
  };

  const auto checkNeedsGrouping = [&](Part& part) {
    KJ_IF_SOME(suffix, part.suffix) {
      if (suffix.size() > 0) return true;
    }
    KJ_IF_SOME(prefix, part.prefix) {
      if (prefix.size() > 0) {
        KJ_IF_SOME(c, options.prefix) {
          return prefix[0] != c;
        }
      }
    }
    if (!needsGrouping && prefixIsEmpty && customName &&
        part.type == Part::Type::SEGMENT_WILDCARD && part.modifier == Part::Modifier::NONE &&
        nextPart != nullptr && partPrefixEmpty(nextPart) && partSuffixEmpty(nextPart)) {
      if (nextPart->type == Part::Type::FIXED_TEXT) {
        return nextPart->name.size() > 0 && isValidCodepoint(nextPart->name[0], false);
      } else {
        return nextPart->name.size() > 0 && isAsciiDigit(nextPart->name[0]);
      }
    }
    return false;
  };

  for (size_t n = 0; n < partList.size(); n++) {
    auto& part = partList[n];
    previousPart = nullptr;
    nextPart = nullptr;
    if (n > 0) previousPart = &partList[n - 1];
    if (n < partList.size() - 1) nextPart = &partList[n + 1];

    if (part.type == Part::Type::FIXED_TEXT) {
      if (part.modifier == Part::Modifier::NONE) {
        pattern = kj::strTree(kj::mv(pattern), escapePatternString(part.value));
        continue;
      }
      pattern = kj::strTree(kj::mv(pattern), "{", escapePatternString(part.value), "}");
      KJ_IF_SOME(c, modifierToString(part.modifier)) {
        pattern = kj::strTree(kj::mv(pattern), c);
      }
      continue;
    }

    KJ_DASSERT(part.name.size() > 0);
    customName = !isAsciiDigit(part.name[0]);
    prefixIsEmpty = partPrefixEmpty(&part);
    needsGrouping = checkNeedsGrouping(part);

    if (!needsGrouping && prefixIsEmpty && previousPart != nullptr) {
      // These additional checks on previousPart have to be separated out from the outer
      // if because in some cases, they may be evaluated before the previousPart != nullptr
      // check.
      if (previousPart->type == Part::Type::FIXED_TEXT &&
          (previousPart->value.size() > 0 &&
              previousPart->value[previousPart->value.size() - 1] == options.prefix.orDefault(0))) {
        needsGrouping = true;
      }
    }

    auto subPattern = kj::strTree();
    KJ_IF_SOME(prefix, part.prefix) {
      subPattern = kj::strTree(kj::mv(subPattern), escapePatternString(prefix));
    }
    if (customName) {
      subPattern = kj::strTree(kj::mv(subPattern), ":", part.name);
    }

    if (part.type == Part::Type::REGEXP) {
      subPattern = kj::strTree(kj::mv(subPattern), "(", part.value, ")");
    } else if (part.type == Part::Type::SEGMENT_WILDCARD && !customName) {
      subPattern = kj::strTree(kj::mv(subPattern), "(", options.segmentWildcardRegexp, ")");
    } else if (part.type == Part::Type::FULL_WILDCARD) {
      if (!customName &&
          (previousPart == nullptr || previousPart->type == Part::Type::FIXED_TEXT ||
              previousPart->modifier != Part::Modifier::NONE || needsGrouping || !prefixIsEmpty)) {
        subPattern = kj::strTree(kj::mv(subPattern), MODIFIER_ZERO_OR_MORE);
      } else {
        subPattern = kj::strTree(kj::mv(subPattern), "(.*)");
      }
    }
    if (part.type == Part::Type::SEGMENT_WILDCARD && customName && partSuffixIsValid(&part)) {
      subPattern = kj::strTree(kj::mv(subPattern), "\\");
    }

    KJ_IF_SOME(suffix, part.suffix) {
      subPattern = kj::strTree(kj::mv(subPattern), escapePatternString(suffix));
    }

    if (needsGrouping) {
      subPattern = kj::strTree("{", kj::mv(subPattern), "}");
    }

    KJ_IF_SOME(c, modifierToString(part.modifier)) {
      subPattern = kj::strTree(kj::mv(subPattern), c);
    }

    pattern = kj::strTree(kj::mv(pattern), kj::mv(subPattern));
  }
  return pattern.flatten();
}

UrlPattern::Result<UrlPattern::Component> tryCompileComponent(kj::Maybe<kj::String>& input,
    Canonicalizer canonicalizer,
    const CompileComponentOptions& options) {
  auto pattern = kj::mv(input).orDefault([] { return kj::str(MODIFIER_ZERO_OR_MORE); });
  KJ_SWITCH_ONEOF(parsePattern(pattern, canonicalizer, options)) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(partList, kj::Array<Part>) {
      auto pattern = generatePatternString(partList, options);
      auto regexAndNameList = generateRegexAndNameList(partList, options);
      return UrlPattern::Component(
          kj::mv(pattern), kj::mv(regexAndNameList.regex), kj::mv(regexAndNameList.names));
    }
  }
  KJ_UNREACHABLE;
}

bool protocolComponentMatchesSpecialScheme(
    kj::StringPtr regex, const UrlPattern::CompileOptions& options) {
  std::regex rx(regex.begin(), regex.size());
  std::cmatch cmatch;
  return std::regex_match("http", cmatch, rx) || std::regex_match("https", cmatch, rx) ||
      std::regex_match("ws", cmatch, rx) || std::regex_match("wss", cmatch, rx) ||
      std::regex_match("ftp", cmatch, rx);
}

UrlPattern::Result<UrlPattern::Init> tryParseConstructorString(
    kj::StringPtr input, const UrlPattern::CompileOptions& options) {
  enum class State {
    INIT,
    PROTOCOL,
    AUTHORITY,
    USERNAME,
    PASSWORD,
    HOSTNAME,
    PORT,
    PATHNAME,
    SEARCH,
    HASH,
    DONE,
  };
  State state = State::INIT;

  size_t inc = 0;
  size_t depth = 0;
  size_t ipv6Depth = 0;
  bool protocolMatchesSpecialScheme = false;

  UrlPattern::Init result{
    .baseUrl = strFromMaybePtr(options.baseUrl),
  };

  kj::Array<Token> tokens = nullptr;
  KJ_SWITCH_ONEOF(tokenize(input, Token::Policy::LENIENT)) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(list, kj::Array<Token>) {
      tokens = kj::mv(list);
    }
  }

  // There should always be at least one token, and it should be type end.
  KJ_DASSERT(tokens.size() > 0);
  KJ_DASSERT(tokens.back().type == Token::Type::END);
  auto it = tokens.begin();
  auto start = it;

  const auto rewind = [&](kj::Maybe<State> maybeNewState = kj::none) {
    KJ_DASSERT(start <= it);
    it = start;
    KJ_DASSERT(tokens.begin() <= it && it < tokens.end());
    inc = 0;
    KJ_IF_SOME(newState, maybeNewState) {
      state = newState;
    }
  };

  const auto makeComponentString = [&]() {
    KJ_DASSERT(tokens.begin() <= it && it < tokens.end());
    KJ_DASSERT(start->index <= it->index);
    return kj::str(input.slice(start->index, it->index));
  };

  const auto changeState = [&](State newState, int skip) {
    if (state != State::INIT && state != State::AUTHORITY && state != State::DONE) {
      auto value = makeComponentString();
      switch (state) {
        case State::PROTOCOL: {
          result.protocol = kj::mv(value);
          break;
        }
        case State::USERNAME: {
          result.username = kj::mv(value);
          break;
        }
        case State::PASSWORD: {
          result.password = kj::mv(value);
          break;
        }
        case State::HOSTNAME: {
          result.hostname = kj::mv(value);
          break;
        }
        case State::PORT: {
          result.port = kj::mv(value);
          break;
        }
        case State::PATHNAME: {
          result.pathname = kj::mv(value);
          break;
        }
        case State::SEARCH: {
          result.search = kj::mv(value);
          break;
        }
        case State::HASH: {
          result.hash = kj::mv(value);
          break;
        }
        case State::INIT: {
          KJ_FALLTHROUGH;
        }
        case State::AUTHORITY: {
          KJ_FALLTHROUGH;
        }
        case State::DONE: {
          KJ_UNREACHABLE;
        }
      }
    }
    state = newState;
    KJ_DASSERT(it + skip <= tokens.end());
    it += skip;
    KJ_DASSERT(tokens.begin() <= it && it < tokens.end());
    start = it;
    inc = 0;
  };

  const auto isNonSpecialPatternChar = [&](auto iter, char c) {
    KJ_DASSERT(tokens.begin() <= iter && iter < tokens.end());
    Token& token = *iter;
    return (token.type == Token::Type::CHAR || token.type == Token::Type::ESCAPED_CHAR ||
               token.type == Token::Type::INVALID_CHAR) &&
        token == c;
  };

  const auto isProtocolSuffix = [&]() { return isNonSpecialPatternChar(it, ':'); };

  const auto nextIsAuthoritySlashes = [&]() {
    return isNonSpecialPatternChar(it + 1, '/') && isNonSpecialPatternChar(it + 2, '/');
  };

  const auto isIdentityTerminator = [&]() { return isNonSpecialPatternChar(it, '@'); };

  const auto isPasswordPrefix = [&]() { return isNonSpecialPatternChar(it, ':'); };

  const auto isPortPrefix = [&]() { return isNonSpecialPatternChar(it, ':'); };

  const auto isPathnameStart = [&]() { return isNonSpecialPatternChar(it, '/'); };

  const auto isSearchPrefix = [&]() {
    if (isNonSpecialPatternChar(it, '?')) {
      return true;
    }
    auto& token = *it;
    if (token != '?') return false;

    if (it == tokens.begin()) return true;

    auto& previousToken = *(it - 1);
    return previousToken.type != Token::Type::NAME && previousToken.type != Token::Type::REGEXP &&
        previousToken.type != Token::Type::CLOSE && previousToken.type != Token::Type::ASTERISK;
  };

  const auto isHashPrefix = [&]() { return isNonSpecialPatternChar(it, '#'); };

  const auto isGroupOpen = [&]() { return it->type == Token::Type::OPEN; };

  const auto isGroupClose = [&]() { return it->type == Token::Type::CLOSE; };

  const auto isIPv6Open = [&]() { return isNonSpecialPatternChar(it, '['); };

  const auto isIPv6Close = [&]() { return isNonSpecialPatternChar(it, ']'); };

  const auto computeMatchesSpecialScheme = [&] {
    kj::Maybe<kj::String> input = makeComponentString();
    KJ_SWITCH_ONEOF(tryCompileComponent(
                        input, &canonicalizeProtocol, CompileComponentOptions::DEFAULT)) {
      KJ_CASE_ONEOF(err, kj::String) {
        // Ignore any errors at this point. If the component is invalid we'll
        // catch it later.
        return false;
      }
      KJ_CASE_ONEOF(component, UrlPattern::Component) {
        return protocolComponentMatchesSpecialScheme(component.getRegex(), options);
      }
    }
    KJ_UNREACHABLE;
  };

  while (it != tokens.end()) {
    Token& token = *it;
    inc = 1;

    if (token.type == Token::Type::END) {
      if (state == State::INIT) {
        rewind();
        if (isHashPrefix()) {
          changeState(State::HASH, 1);
        } else if (isSearchPrefix()) {
          changeState(State::SEARCH, 1);
          result.hash = kj::str();
        } else {
          changeState(State::PATHNAME, 0);
          result.search = kj::str();
          result.hash = kj::str();
        }
        // Since we called rewind and we know that sets inc to zero,
        // and we know that nothing else here changed inc, there's no
        // need to try to advance. Just continue.
        continue;
      }
      if (state == State::AUTHORITY) {
        rewind(State::HOSTNAME);
        // Since we called rewind and we know that sets inc to zero,
        // there's no need to try to advance. Just continue.
        continue;
      }
      // We hit the end and we're all done!
      changeState(State::DONE, 0);
      break;
    }
    if (isGroupOpen()) {
      depth++;
      it += inc;
      continue;
    }
    if (depth > 0) {
      if (isGroupClose()) {
        depth--;
      } else {
        it += inc;
        continue;
      }
    }

    switch (state) {
      case State::INIT: {
        if (isProtocolSuffix()) {
          result.username = kj::str();
          result.password = kj::str();
          result.hostname = kj::str();
          result.port = kj::str();
          result.pathname = kj::str();
          result.search = kj::str();
          result.hash = kj::str();
          rewind(State::PROTOCOL);
        }
        break;
      }
      case State::PROTOCOL: {
        if (isProtocolSuffix()) {
          computeMatchesSpecialScheme();
          if (protocolMatchesSpecialScheme) result.pathname = kj::str("/");
          if (nextIsAuthoritySlashes())
            changeState(State::AUTHORITY, 3);
          else if (protocolMatchesSpecialScheme)
            changeState(State::AUTHORITY, 1);
          else
            changeState(State::PATHNAME, 1);
        }
        break;
      }
      case State::AUTHORITY: {
        if (isIdentityTerminator())
          rewind(State::USERNAME);
        else if (isPathnameStart() || isSearchPrefix() || isHashPrefix())
          rewind(State::HOSTNAME);
        break;
      }
      case State::USERNAME: {
        if (isPasswordPrefix())
          changeState(State::PASSWORD, 1);
        else if (isIdentityTerminator())
          changeState(State::HOSTNAME, 1);
        break;
      }
      case State::PASSWORD: {
        if (isIdentityTerminator()) changeState(State::HOSTNAME, 1);
        break;
      }
      case State::HOSTNAME: {
        if (isIPv6Open())
          ipv6Depth++;
        else if (isIPv6Close())
          ipv6Depth--;
        else if (isPortPrefix() && ipv6Depth == 0)
          changeState(State::PORT, 1);
        else if (isPathnameStart())
          changeState(State::PATHNAME, 0);
        else if (isSearchPrefix())
          changeState(State::SEARCH, 1);
        else if (isHashPrefix())
          changeState(State::HASH, 1);
        break;
      }
      case State::PORT: {
        if (isPathnameStart())
          changeState(State::PATHNAME, 0);
        else if (isSearchPrefix())
          changeState(State::SEARCH, 1);
        else if (isHashPrefix())
          changeState(State::HASH, 1);
        break;
      }
      case State::PATHNAME: {
        if (isSearchPrefix())
          changeState(State::SEARCH, 1);
        else if (isHashPrefix())
          changeState(State::HASH, 1);
        break;
      }
      case State::SEARCH: {
        if (isHashPrefix()) changeState(State::HASH, 1);
        break;
      }
      case State::HASH: {
        // Nothing to do.
        break;
      }
      case State::DONE: {
        KJ_UNREACHABLE;
      }
    }

    it += inc;
  }

  if (result.protocol == kj::none && result.baseUrl == kj::none) {
    return kj::str("Syntax error in URL Pattern: a relative pattern must have a base URL.");
  }

  return kj::mv(result);
}
}  // namespace

UrlPattern::Component::Component(kj::String pattern, kj::String regex, kj::Array<kj::String> names)
    : pattern(kj::mv(pattern)),
      regex(kj::mv(regex)),
      names(kj::mv(names)) {}

UrlPattern::Result<UrlPattern> UrlPattern::tryCompileInit(
    UrlPattern::Init init, const UrlPattern::CompileOptions& options) {
  kj::Vector<UrlPattern::Component> components(7);

  bool matchesSpecialScheme = false;

  KJ_SWITCH_ONEOF(tryCompileComponent(
                      init.protocol, &canonicalizeProtocol, CompileComponentOptions::DEFAULT)) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(component, UrlPattern::Component) {
      matchesSpecialScheme = protocolComponentMatchesSpecialScheme(component.getRegex(), options);
      components.add(kj::mv(component));
    }
  }

  const auto handleComponent =
      [&](auto& input, Canonicalizer canonicalizer,
          const CompileComponentOptions& options) -> kj::Maybe<kj::String> {
    KJ_SWITCH_ONEOF(tryCompileComponent(input, canonicalizer, options)) {
      KJ_CASE_ONEOF(err, kj::String) {
        return kj::mv(err);
      }
      KJ_CASE_ONEOF(component, UrlPattern::Component) {
        components.add(kj::mv(component));
        return kj::none;
      }
    }
    KJ_UNREACHABLE;
  };

  KJ_IF_SOME(err,
      handleComponent(init.username, &canonicalizeUsername, CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }
  KJ_IF_SOME(err,
      handleComponent(init.password, &canonicalizePassword, CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }

  Canonicalizer* hostnameCanonicalizer = &canonicalizeHostname;
  KJ_IF_SOME(hostname, init.hostname) {
    if (isIpv6(hostname.asPtr())) {
      hostnameCanonicalizer = &canonicalizeIpv6Hostname;
    }
  }
  KJ_IF_SOME(err,
      handleComponent(init.hostname, hostnameCanonicalizer, CompileComponentOptions::HOSTNAME)) {
    return kj::mv(err);
  }

  KJ_IF_SOME(err, handleComponent(init.port, &canonicalizePort, CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }

  KJ_IF_SOME(err,
      handleComponent(init.pathname,
          matchesSpecialScheme ? &canonicalizePathname : &canonicalizeOpaquePathname,
          matchesSpecialScheme ? CompileComponentOptions::PATHNAME
                               : CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }
  KJ_IF_SOME(err,
      handleComponent(init.search, &canonicalizeSearch, CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }
  KJ_IF_SOME(err, handleComponent(init.hash, &canonicalizeHash, CompileComponentOptions::DEFAULT)) {
    return kj::mv(err);
  }
  return UrlPattern(components.releaseAsArray(), options.ignoreCase);
}

UrlPattern::Result<UrlPattern::Init> UrlPattern::processInit(
    UrlPattern::Init init, kj::Maybe<UrlPattern::ProcessInitOptions> maybeOptions) {
  auto options = maybeOptions.orDefault({});

  Init result;
  kj::Maybe<Url> maybeBaseUrl;

  const auto isAbsolutePathname = [&](kj::StringPtr str) {
    if (str.size() == 0) return false;
    char c = str[0];
    if (c == '/') return true;
    if (options.mode == ProcessInitOptions::Mode::URL) return false;
    return str.size() > 1 && (c == '\\' || c == '{') && str[1] == '/';
  };

  KJ_IF_SOME(base, init.baseUrl) {
    KJ_IF_SOME(url, Url::tryParse(base.asPtr())) {
      result.protocol = stripSuffixFromProtocol(url.getProtocol());
      result.username = kj::str(url.getUsername());
      result.password = kj::str(url.getPassword());
      result.hostname = kj::str(url.getHostname());
      result.port = kj::str(url.getPort());
      result.pathname = escapePatternString(url.getPathname());
      if (url.getSearch().size() > 0) {
        result.search = escapePatternString(url.getSearch().slice(1));
      } else {
        result.search = kj::str();
      }
      if (url.getHash().size() > 0) {
        result.hash = escapePatternString(url.getHash().slice(1));
      } else {
        result.hash = kj::str();
      }
      result.baseUrl = kj::mv(base);
      maybeBaseUrl = kj::mv(url);
    } else {
      return kj::str("Invalid base URL.");
    }
  }

  if (options.mode == ProcessInitOptions::Mode::PATTERN) {
    KJ_IF_SOME(protocol,
        chooseStr(kj::mv(init.protocol), options.protocol).map([](kj::String&& str) mutable {
      // It's silly but the URL spec always includes the : suffix in the value,
      // while the URLPattern spec always omits it. Silly specs.
      return stripSuffixFromProtocol(str.asPtr());
    })) {
      result.protocol = kj::mv(protocol);
    }
    KJ_IF_SOME(username, chooseStr(kj::mv(init.username), options.username)) {
      result.username = kj::mv(username);
    }
    KJ_IF_SOME(password, chooseStr(kj::mv(init.password), options.password)) {
      result.password = kj::mv(password);
    }
    KJ_IF_SOME(hostname, chooseStr(kj::mv(init.hostname), options.hostname)) {
      result.hostname = kj::mv(hostname);
    }
    KJ_IF_SOME(port, chooseStr(kj::mv(init.port), options.port)) {
      result.port = kj::mv(port);
    }
    KJ_IF_SOME(pathname, chooseStr(kj::mv(init.pathname), options.pathname)) {
      if (!isAbsolutePathname(pathname)) {
        KJ_IF_SOME(url, maybeBaseUrl) {
          auto basePathname = url.getPathname();
          KJ_IF_SOME(index, basePathname.findLast('/')) {
            result.pathname = kj::str(basePathname.slice(0, index + 1), pathname);
          } else {
            result.pathname = kj::str(basePathname);
          }
        } else {
          result.pathname = kj::mv(pathname);
        }
      } else {
        result.pathname = kj::mv(pathname);
      }
    }
    KJ_IF_SOME(search, chooseStr(kj::mv(init.search), options.search)) {
      if (search.size() > 0 && search[0] == '?') {
        result.search = kj::str(search.slice(1));
      } else {
        result.search = kj::mv(search);
      }
    }
    KJ_IF_SOME(hash, chooseStr(kj::mv(init.hash), options.hash)) {
      if (hash.size() > 0 && hash[0] == '#') {
        result.hash = kj::str(hash.slice(1));
      } else {
        result.hash = kj::mv(hash);
      }
    }
    return result;
  }

  KJ_DASSERT(options.mode == ProcessInitOptions::Mode::URL);

  // Things are a bit more complicated in this case. The individual components
  // of Init are interpreted as URL components. The processing here must convert
  // those into a canonical form. Unfortunately, however, it's not *quite* as
  // simple as constructing a URL string from the inputs, parsing it, and then
  // deconstructing the result. The validation rules per the URLPattern spec are
  // a bit different for some of the components than for the URL spec so we handle
  // each individually.

  bool isAbsolute = false;
  auto scratch = ([&]() -> kj::OneOf<Url, kj::String> {
    KJ_IF_SOME(protocol, chooseStr(kj::mv(init.protocol), options.protocol)) {
      // The protocol value we are given might not be valid. We'll check by
      // attempting to use it to parse a URL.
      bool emptyProtocol = protocol == "";
      auto str = kj::str((emptyProtocol ? "fake:"_kj : protocol.asPtr()),
          (emptyProtocol || protocol.asArray().back() == ':') ? "" : ":", "//a:b@fake-url");
      KJ_IF_SOME(parsed, Url::tryParse(str.asPtr())) {
        // Nice. We have a good protocol component. Let's set the normalized version
        // on the result and return the parsed URL to use as our temporary.
        if (!emptyProtocol) {
          result.protocol = stripSuffixFromProtocol(parsed.getProtocol());
        }

        // We set isAbsolute true here so that when we later want to normalize the
        // pathname, we know not to try to resolve the path relative to the base.
        isAbsolute = true;
        return kj::mv(parsed);
      } else {
        // Doh, parsing failed. The protocol component is invalid.
        return kj::str("Invalid URL protocol component");
      }
    } else {
      // There was not protocol component in the init or options. We still might
      // have a base URL protocol. If we do, we're going to use it to construct
      // our temporary URL we will use to canonicalize the rest. If we do not,
      // we'll use a fake URL scheme.
      KJ_IF_SOME(protocol, result.protocol) {
        // We only want to create the temporary URL here and return it.
        auto str = kj::str(protocol, "://fake-url");
        return KJ_ASSERT_NONNULL(Url::tryParse(str.asPtr()));
      } else {
        auto str = kj::str("fake://fake-url");
        return KJ_ASSERT_NONNULL(Url::tryParse(str.asPtr()));
      }
    }
  })();

  KJ_SWITCH_ONEOF(scratch) {
    KJ_CASE_ONEOF(err, kj::String) {
      // Invalid URL protocol component.
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(url, Url) {
      KJ_IF_SOME(username, chooseStr(kj::mv(init.username), options.username)) {
        if (!url.setUsername(username.asPtr())) {
          return kj::str("Invalid URL username component");
        }
        result.username = kj::str(url.getUsername());
      }
      KJ_IF_SOME(password, chooseStr(kj::mv(init.password), options.password)) {
        if (!url.setPassword(password.asPtr())) {
          return kj::str("Invalid URL password component");
        }
        result.password = kj::str(url.getPassword());
      }
      KJ_IF_SOME(hostname, chooseStr(kj::mv(init.hostname), options.hostname)) {
        if (!isValidHostnameInput(hostname) || !url.setHostname(hostname.asPtr())) {
          return kj::str("Invalid URL hostname component");
        }
        result.hostname = kj::str(url.getHostname());
      }
      KJ_IF_SOME(port, chooseStr(kj::mv(init.port), options.port)) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), isAsciiDigit)) {
          return kj::str("Invalid URL port component");
        }
        if (port.size() == 0) {
          url.setPort(kj::none);
        } else if (!url.setPort(kj::Maybe(port.asPtr()))) {
          return kj::str("Invalid URL port component");
        }
        result.port = kj::str(url.getPort());
      }
      KJ_IF_SOME(pathname, chooseStr(kj::mv(init.pathname), options.pathname)) {
        if (isAbsolute) {
          // isAbsolute is set only if we have an explicit protocol set for in init
          // or options. This tells us that we are not going to resolve the path
          // relative to the base URL at all.
          if (!url.setPathname(pathname.asPtr())) {
            return kj::str("Invalid URL pathname component");
          }
          result.pathname = kj::str(url.getPathname());
        } else {
          // Here, our init/options did not specify a protocol, so we're either relying
          // on the base URL or the fake. If we have a base URL, then, we want to resolve
          // the path relative to the base URL path.
          KJ_IF_SOME(base, maybeBaseUrl) {
            // If there is a base URL, then we'll normalize the path by attempting to
            // resolve against the base.
            KJ_IF_SOME(resolved, base.resolve(pathname.asPtr())) {
              result.pathname = kj::str(resolved.getPathname());
            } else {
              return kj::str("Invalid URL pathname component");
            }
          } else {
            if (!url.setPathname(pathname.asPtr())) {
              return kj::str("Invalid URL pathname component");
            }
            result.pathname = kj::str(url.getPathname());
          }
        }
      }
      KJ_IF_SOME(search, chooseStr(kj::mv(init.search), options.search)) {
        url.setSearch(kj::Maybe(search.asPtr()));
        // We slice here because the URL getter will always include the ? prefix
        // but the URLPattern spec does not want it.
        if (url.getSearch().size() > 0) {
          result.search = kj::str(url.getSearch().slice(1));
        } else {
          result.search = kj::str();
        }
      }
      KJ_IF_SOME(hash, chooseStr(kj::mv(init.hash), options.hash)) {
        url.setHash(kj::Maybe(hash.asPtr()));
        // We slice here because the URL getter will always include the # prefix
        // but the URLPattern spec does not want it.
        if (url.getHash().size() > 0) {
          result.hash = kj::str(url.getHash().slice(1));
        } else {
          result.hash = kj::str();
        }
      }
      return result;
    }
  }
  KJ_UNREACHABLE;
}

UrlPattern::Result<UrlPattern> UrlPattern::tryCompile(
    Init init, kj::Maybe<CompileOptions> maybeOptions) {
  auto options = maybeOptions.orDefault({});
  KJ_SWITCH_ONEOF(processInit(kj::mv(init))) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(init, UrlPattern::Init) {
      return tryCompileInit(kj::mv(init), options);
    }
  }
  KJ_UNREACHABLE;
}

UrlPattern::Result<UrlPattern> UrlPattern::tryCompile(
    kj::StringPtr input, kj::Maybe<CompileOptions> maybeOptions) {
  auto options = maybeOptions.orDefault({});
  KJ_SWITCH_ONEOF(tryParseConstructorString(input, options)) {
    KJ_CASE_ONEOF(err, kj::String) {
      return kj::mv(err);
    }
    KJ_CASE_ONEOF(init, UrlPattern::Init) {
      KJ_SWITCH_ONEOF(processInit(kj::mv(init))) {
        KJ_CASE_ONEOF(err, kj::String) {
          return kj::mv(err);
        }
        KJ_CASE_ONEOF(init, UrlPattern::Init) {
          return tryCompileInit(kj::mv(init), options);
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

UrlPattern::UrlPattern(kj::Array<Component> components, bool ignoreCase)
    : protocol(kj::mv(components[0])),
      username(kj::mv(components[1])),
      password(kj::mv(components[2])),
      hostname(kj::mv(components[3])),
      port(kj::mv(components[4])),
      pathname(kj::mv(components[5])),
      search(kj::mv(components[6])),
      hash(kj::mv(components[7])),
      ignoreCase(ignoreCase) {}

}  // namespace workerd::jsg

const workerd::jsg::Url operator"" _url(const char* str, size_t size) {
  return KJ_ASSERT_NONNULL(workerd::jsg::Url::tryParse(kj::ArrayPtr<const char>(str, size)));
}
