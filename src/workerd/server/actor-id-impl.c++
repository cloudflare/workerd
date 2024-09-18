#include <workerd/jsg/exception.h>
#include <workerd/server/actor-id-impl.h>
#include <workerd/util/thread-scopes.h>

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <kj/encoding.h>
#include <kj/memory.h>

namespace workerd::server {

ActorIdFactoryImpl::ActorIdImpl::ActorIdImpl(
    const kj::byte idParam[SHA256_DIGEST_LENGTH], kj::Maybe<kj::String> name)
    : name(kj::mv(name)) {
  memcpy(id, idParam, sizeof(id));
}

kj::String ActorIdFactoryImpl::ActorIdImpl::toString() const {
  return kj::encodeHex(kj::ArrayPtr<const kj::byte>(id));
}

kj::Maybe<kj::StringPtr> ActorIdFactoryImpl::ActorIdImpl::getName() const {
  return name;
}

bool ActorIdFactoryImpl::ActorIdImpl::equals(const ActorId& other) const {
  return kj::arrayPtr(id) == kj::arrayPtr(kj::downcast<const ActorIdImpl>(other).id);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::ActorIdImpl::clone() const {
  return kj::heap<ActorIdImpl>(id, name.map([](kj::StringPtr str) { return kj::str(str); }));
}

ActorIdFactoryImpl::ActorIdFactoryImpl(kj::StringPtr uniqueKey) {
  KJ_ASSERT(SHA256(uniqueKey.asBytes().begin(), uniqueKey.size(), key) == key);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::newUniqueId(
    kj::Maybe<kj::StringPtr> jurisdiction) {
  JSG_REQUIRE(
      jurisdiction == kj::none, Error, "Jurisdiction restrictions are not implemented in workerd.");

  // We want to randomly-generate the first 16 bytes, then HMAC those to produce the latter
  // 16 bytes. But the HMAC will produce 32 bytes, so we're only taking a prefix of it. We'll
  // allocate a single array big enough to output the HMAC as a suffix, which will then get
  // truncated.
  kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]{};

  if (isPredictableModeForTest()) {
    memcpy(id, &counter, sizeof(counter));
    kj::arrayPtr(id).slice(counter).fill(0);
    ++counter;
  } else {
    KJ_ASSERT(RAND_bytes(id, BASE_LENGTH) == 1);
  }

  computeMac(id);
  return kj::heap<ActorIdImpl>(id, kj::none);
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromName(kj::String name) {
  kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]{};

  // Compute the first half of the ID by HMACing the name itself. We're using HMAC as a keyed
  // hash here, not actually for authentication, but it works.
  unsigned int len = SHA256_DIGEST_LENGTH;
  KJ_ASSERT(
      HMAC(EVP_sha256(), key, sizeof(key), name.asBytes().begin(), name.size(), id, &len) == id);
  KJ_ASSERT(len == SHA256_DIGEST_LENGTH);

  computeMac(id);
  return kj::heap<ActorIdImpl>(id, kj::mv(name));
}

kj::Own<ActorIdFactory::ActorId> ActorIdFactoryImpl::idFromString(kj::String str) {
  auto decoded = kj::decodeHex(str);
  JSG_REQUIRE(str.size() == SHA256_DIGEST_LENGTH * 2 && !decoded.hadErrors &&
          decoded.size() == SHA256_DIGEST_LENGTH,
      TypeError, "Invalid Durable Object ID: must be 64 hex digits");

  kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]{};
  memcpy(id, decoded.begin(), BASE_LENGTH);
  computeMac(id);

  // Verify that the computed mac matches the input.
  JSG_REQUIRE(kj::arrayPtr(id).slice(BASE_LENGTH).startsWith(decoded.asPtr().slice(BASE_LENGTH)),
      TypeError, "Durable Object ID is not valid for this namespace.");

  return kj::heap<ActorIdImpl>(id, kj::none);
}

kj::Own<ActorIdFactory> ActorIdFactoryImpl::cloneWithJurisdiction(kj::StringPtr jurisdiction) {
  JSG_FAIL_REQUIRE(Error, "Jurisdiction restrictions are not implemented in workerd.");
}

bool ActorIdFactoryImpl::matchesJurisdiction(const ActorId& id) {
  return true;
}

void ActorIdFactoryImpl::computeMac(kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]) {
  // Given that the first `BASE_LENGTH` bytes of `id` are filled in, compute the second half
  // of the ID by HMACing the first half. The id must be in a buffer large enough to store the
  // first half of the ID plus a full HMAC, even though only a prefix of the HMAC becomes part
  // of the final ID.

  kj::byte* hmacOut = id + BASE_LENGTH;
  unsigned int len = SHA256_DIGEST_LENGTH;
  KJ_ASSERT(HMAC(EVP_sha256(), key, sizeof(key), id, BASE_LENGTH, hmacOut, &len) == hmacOut);
  KJ_ASSERT(len == SHA256_DIGEST_LENGTH);
}

}  //namespace workerd::server
