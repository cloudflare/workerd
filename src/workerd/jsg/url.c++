#include "url.h"
#include <kj/hash.h>

extern "C" {
  #include "ada_c.h"
}

#include <kj/debug.h>
#include <kj/vector.h>

namespace workerd::jsg {

namespace {
class AdaOwnedStringDisposer : public kj::ArrayDisposer {
public:
  static const AdaOwnedStringDisposer INSTANCE;

protected:
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const {
    ada_owned_string data = {
      static_cast<const char*>(firstElement),
      elementCount };
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

}  // namespace

Url::Url(kj::Own<void> inner) : inner(kj::mv(inner)) {}

bool Url::operator==(const Url& other) const {
  return getHref() == other.getHref();
}

bool Url::equal(const Url& other, EquivalenceOption option) const {
  if (option == EquivalenceOption::DEFAULT) {
    return *this == other;
  }

  // If we are ignoring fragments, we'll compare each component separately:
  return other.getProtocol() == getProtocol() &&
         other.getHost() == getHost() &&
         other.getUsername() == getUsername() &&
         other.getPassword() == getPassword() &&
         other.getPathname() == getPathname() &&
         other.getSearch() == getSearch();
 }

bool Url::canParse(kj::StringPtr input, kj::Maybe<kj::StringPtr> base) {
  return canParse(kj::ArrayPtr<const char>(input), base);
}

bool Url::canParse(kj::ArrayPtr<const char> input,
                   kj::Maybe<kj::ArrayPtr<const char>> base) {
  KJ_IF_SOME(b, base) {
    return ada_can_parse_with_base(input.begin(), input.size(),
                                   b.begin(), b.size());
  }
  return ada_can_parse(input.begin(), input.size());
}

kj::Maybe<Url> Url::tryParse(kj::StringPtr input,
                             kj::Maybe<kj::StringPtr> base) {
  return tryParse(kj::ArrayPtr<const char>(input), base);
}

kj::Maybe<Url> Url::tryParse(kj::ArrayPtr<const char> input,
                             kj::Maybe<kj::ArrayPtr<const char>> base) {
  ada_url result = nullptr;
  KJ_IF_SOME(b, base) {
    result = ada_parse_with_base(input.begin(), input.size(),
                                 b.begin(), b.size());
  } else {
    result = ada_parse(input.begin(), input.size());
  }
  if (!ada_is_valid(result)) return kj::none;
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
      const_cast<char*>(result.data),
      result.length,
      AdaOwnedStringDisposer::INSTANCE);
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

Url Url::clone(EquivalenceOption option) {
  ada_url copy = ada_copy(getInner<ada_url>(inner));
  if (option == EquivalenceOption::IGNORE_FRAGMENTS) {
    ada_clear_hash(copy);
  }
  return Url(wrap(copy));
}

kj::Array<const char> Url::idnToUnicode(kj::ArrayPtr<const char> value) {
  ada_owned_string result = ada_idna_to_unicode(value.begin(), value.size());
  return kj::Array<const char>(
      result.data,
      result.length,
      AdaOwnedStringDisposer::INSTANCE);
}

kj::Array<const char> Url::idnToAscii(kj::ArrayPtr<const char> value) {
  ada_owned_string result = ada_idna_to_ascii(value.begin(), value.size());
  return kj::Array<const char>(
      result.data,
      result.length,
      AdaOwnedStringDisposer::INSTANCE);
}

kj::uint Url::hashCode() const {
  return kj::hashCode(getHref());
}

// ======================================================================================

namespace {
kj::Own<void> emptySearchParams() {
  ada_url_search_params result = ada_parse_search_params(nullptr, 0);
  KJ_ASSERT(result);
  return kj::disposeWith<ada_free_search_params>(result);
}
}  // namespace

UrlSearchParams::UrlSearchParams() : inner(emptySearchParams()) {}

UrlSearchParams::UrlSearchParams(kj::Own<void> inner) : inner(kj::mv(inner)) {}

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
  ada_search_params_append(getInner<ada_url_search_params>(inner),
                           key.begin(), key.size(),
                           value.begin(), value.size());
}

void UrlSearchParams::set(kj::ArrayPtr<const char> key, kj::ArrayPtr<const char> value) {
  ada_search_params_set(getInner<ada_url_search_params>(inner),
                        key.begin(), key.size(),
                        value.begin(), value.size());
}

void UrlSearchParams::delete_(kj::ArrayPtr<const char> key,
                              kj::Maybe<kj::ArrayPtr<const char>> maybeValue) {
  KJ_IF_SOME(value, maybeValue) {
    ada_search_params_remove_value(getInner<ada_url_search_params>(inner),
                                   key.begin(), key.size(),
                                   value.begin(), value.size());
  } else {
    ada_search_params_remove(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  }
}

bool UrlSearchParams::has(kj::ArrayPtr<const char> key,
                          kj::Maybe<kj::ArrayPtr<const char>> maybeValue) const {
  KJ_IF_SOME(value, maybeValue) {
    return ada_search_params_has_value(getInner<ada_url_search_params>(inner),
                                       key.begin(), key.size(),
                                       value.begin(), value.size());
  } else {
    return ada_search_params_has(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  }
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::get(kj::ArrayPtr<const char> key) const {
  auto result = ada_search_params_get(getInner<ada_url_search_params>(inner), key.begin(), key.size());
  if (result.data == nullptr) return kj::none;
  return kj::ArrayPtr<const char>(result.data, result.length);
}

kj::Array<kj::ArrayPtr<const char>> UrlSearchParams::getAll(kj::ArrayPtr<const char> key) const {
  ada_strings results = ada_search_params_get_all(getInner<ada_url_search_params>(inner),
                                                  key.begin(), key.size());
  size_t size = ada_strings_size(results);
  kj::Vector<kj::ArrayPtr<const char>> items(size);
  for (size_t n = 0; n < size; n++) {
    auto item = ada_strings_get(results, n);
    items.add(kj::ArrayPtr<const char>(item.data, item.length));
  }
  return items.releaseAsArray();
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
  return kj::Array<const char>(
      result.data,
      result.length,
      AdaOwnedStringDisposer::INSTANCE);
}

UrlSearchParams::KeyIterator::KeyIterator(kj::Own<void> inner) : inner(kj::mv(inner)) {}

bool UrlSearchParams::KeyIterator::hasNext() const {
  return ada_search_params_keys_iter_has_next(
      getInner<ada_url_search_params_keys_iter>(inner));
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::KeyIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next = ada_search_params_keys_iter_next(
      getInner<ada_url_search_params_keys_iter>(inner));
  return kj::ArrayPtr<const char>(next.data, next.length);
}

UrlSearchParams::ValueIterator::ValueIterator(kj::Own<void> inner) : inner(kj::mv(inner)) {}

bool UrlSearchParams::ValueIterator::hasNext() const {
  return ada_search_params_values_iter_has_next(
      getInner<ada_url_search_params_values_iter>(inner));
}

kj::Maybe<kj::ArrayPtr<const char>> UrlSearchParams::ValueIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next = ada_search_params_values_iter_next(
      getInner<ada_url_search_params_values_iter>(inner));
  return kj::ArrayPtr<const char>(next.data, next.length);
}

UrlSearchParams::EntryIterator::EntryIterator(kj::Own<void> inner) : inner(kj::mv(inner)) {}

bool UrlSearchParams::EntryIterator::hasNext() const {
  return ada_search_params_entries_iter_has_next(
      getInner<ada_url_search_params_entries_iter>(inner));
}

kj::Maybe<UrlSearchParams::EntryIterator::Entry> UrlSearchParams::EntryIterator::next() const {
  if (!hasNext()) return kj::none;
  auto next = ada_search_params_entries_iter_next(
      getInner<ada_url_search_params_entries_iter>(inner));
  return Entry {
    .key = kj::ArrayPtr<const char>(next.key.data, next.key.length),
    .value = kj::ArrayPtr<const char>(next.value.data, next.value.length),
  };
}

}  // namespace workerd::jsg
