#pragma once

#include <capnp/membrane.h>

namespace workerd {

enum class MembraneCallDirection {
  // From outside the membrane to a capability inside it.
  INBOUND,
  // From inside the membrane to a capability outside it.
  OUTBOUND,
};

// Returned by the rootPolicy() of a CompletionMembrane or RevokerMembrane. A capability wrapped in
// it, in the direction the membrane would have wrapped it, is unwrapped when it crosses back through
// the membrane, like one the membrane wrapped itself. But it doesn't hold a reference to the
// membrane's policy, so it doesn't count as held by a CompletionMembrane and isn't revoked by a
// RevokerMembrane.
class DetachedMembranePolicy final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
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
};

// Decides whether a call crossing a membrane is redirected, with the semantics of
// capnp::MembranePolicy::inboundCall() and outboundCall(). Returns kj::none to let the call
// through. A redirect target is on the caller's side of the membrane, so it receives the
// membrane's `detachedPolicy` in order to wrap any results that must be unwrapped when they cross
// back.
using MembraneCallRedirector = kj::Maybe<capnp::Capability::Client> (*)(uint64_t interfaceId,
    uint16_t methodId,
    capnp::Capability::Client target,
    DetachedMembranePolicy& detachedPolicy,
    MembraneCallDirection direction);

namespace _ {  // private

inline kj::Maybe<capnp::Capability::Client> maybeRedirect(
    kj::Maybe<MembraneCallRedirector> redirector,
    uint64_t interfaceId,
    uint16_t methodId,
    capnp::Capability::Client target,
    DetachedMembranePolicy& detachedPolicy,
    MembraneCallDirection direction) {
  KJ_IF_SOME(r, redirector) {
    return r(interfaceId, methodId, kj::mv(target), detachedPolicy, direction);
  }
  return kj::none;
}

}  // namespace _

// A membrane applied which detects when no capabilities are held any longer, at which point it
// fulfills a fulfiller.
//
// If `redirector` is given, calls for which it returns a redirect go there. Capabilities that the
// redirect target returns unwrapped, or wrapped only in the DetachedMembranePolicy, don't count as
// held.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class CompletionMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  explicit CompletionMembrane(kj::Own<kj::PromiseFulfiller<void>> doneFulfiller,
      kj::Maybe<MembraneCallRedirector> redirector = kj::none)
      : doneFulfiller(kj::mv(doneFulfiller)),
        redirector(redirector) {}
  ~CompletionMembrane() noexcept(false) {
    doneFulfiller->fulfill();
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return _::maybeRedirect(redirector, interfaceId, methodId, kj::mv(target), *detached,
        MembraneCallDirection::INBOUND);
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return _::maybeRedirect(redirector, interfaceId, methodId, kj::mv(target), *detached,
        MembraneCallDirection::OUTBOUND);
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

  capnp::MembranePolicy& rootPolicy() override {
    return *detached;
  }

  void reject(kj::Exception&& e) {
    doneFulfiller->reject(kj::mv(e));
  }

 private:
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;
  kj::Maybe<MembraneCallRedirector> redirector;
  kj::Own<DetachedMembranePolicy> detached = kj::refcounted<DetachedMembranePolicy>();
};

// A membrane which revokes when some Promise is fulfilled.
//
// If `redirector` is given, calls for which it returns a redirect go there. Capabilities that the
// redirect target returns unwrapped, or wrapped only in the DetachedMembranePolicy, aren't revoked.
//
// TODO(cleanup): This is generally useful, should it be part of capnp?
class RevokerMembrane final: public capnp::MembranePolicy, public kj::Refcounted {
 public:
  explicit RevokerMembrane(
      kj::Promise<void> promise, kj::Maybe<MembraneCallRedirector> redirector = kj::none)
      : promise(promise.fork()),
        redirector(redirector) {}

  // Keeps the revocation promise pending until the membrane and all capabilities crossing it are
  // gone.
  void retainRevoker(kj::Own<kj::PromiseFulfiller<void>> revoker) {
    this->revoker = kj::mv(revoker);
  }

  kj::Maybe<capnp::Capability::Client> inboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return _::maybeRedirect(redirector, interfaceId, methodId, kj::mv(target), *detached,
        MembraneCallDirection::INBOUND);
  }

  kj::Maybe<capnp::Capability::Client> outboundCall(
      uint64_t interfaceId, uint16_t methodId, capnp::Capability::Client target) override {
    return _::maybeRedirect(redirector, interfaceId, methodId, kj::mv(target), *detached,
        MembraneCallDirection::OUTBOUND);
  }

  kj::Own<MembranePolicy> addRef() override {
    return kj::addRef(*this);
  }

  capnp::MembranePolicy& rootPolicy() override {
    return *detached;
  }

  kj::Maybe<kj::Promise<void>> onRevoked() override {
    return promise.addBranch();
  }

 private:
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> revoker;
  kj::ForkedPromise<void> promise;
  kj::Maybe<MembraneCallRedirector> redirector;
  kj::Own<DetachedMembranePolicy> detached = kj::refcounted<DetachedMembranePolicy>();
};

}  // namespace workerd
