#pragma once

#include <capnp/membrane.h>

namespace workerd {

// A membrane applied which detects when no capabilities are held any longer, at which point it
// fulfills a fulfiller.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class CompletionMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  explicit CompletionMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller)
      : doneFulfiller(kj::mv(doneFulfiller)) {}
  ~CompletionMembrane() noexcept(false) {
    doneFulfiller->fulfill();
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

 private:
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
};

// A membrane which revokes when some Promise is fulfilled.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class RevokerMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  explicit RevokerMembrane(kj::Promise<void> promise): promise(promise.fork()) {}

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return kj::none;
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

  kj::Maybe<kj::Promise<void>> onRevoked() override {
    return promise.addBranch();
  }

 private:
  kj::ForkedPromise<void> promise;
};

}  // namespace workerd
