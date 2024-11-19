#pragma once

#include <workerd/io/actor-id.h>

#include <openssl/sha.h>

namespace workerd::server {
class ActorIdFactoryImpl final: public ActorIdFactory {
 public:
  ActorIdFactoryImpl(kj::StringPtr uniqueKey);
  class ActorIdImpl final: public ActorId {
   public:
    ActorIdImpl(const kj::byte idParam[SHA256_DIGEST_LENGTH], kj::Maybe<kj::String> name);

    kj::String toString() const override;
    kj::Maybe<kj::StringPtr> getName() const override;
    bool equals(const ActorId& other) const override;
    kj::Own<ActorId> clone() const override;

   private:
    kj::byte id[SHA256_DIGEST_LENGTH];
    kj::Maybe<kj::String> name;
  };

  kj::Own<ActorId> newUniqueId(kj::Maybe<kj::StringPtr> jurisdiction) override;
  kj::Own<ActorId> idFromName(kj::String name) override;
  kj::Own<ActorId> idFromString(kj::String str) override;
  kj::Own<ActorIdFactory> cloneWithJurisdiction(kj::StringPtr jurisdiction) override;
  bool matchesJurisdiction(const ActorId& id) override;

 private:
  kj::byte key[SHA256_DIGEST_LENGTH];

  uint64_t counter = 0;  // only used in predictable mode

  static constexpr size_t BASE_LENGTH = SHA256_DIGEST_LENGTH / 2;
  void computeMac(kj::byte id[BASE_LENGTH + SHA256_DIGEST_LENGTH]);
};

}  // namespace workerd::server
