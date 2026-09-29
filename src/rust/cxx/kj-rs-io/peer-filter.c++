#include "kj-rs-io/peer-filter.h"

namespace kj_rs_io {

PeerFilter::PeerFilter(): next(nullptr) {}

PeerFilter::PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
    kj::ArrayPtr<const kj::StringPtr> deny,
    kj::Arc<PeerFilter> nextFilter)
    : next(kj::mv(nextFilter)),
      impl(allow, deny, next->impl) {}

bool PeerFilter::allows(const struct sockaddr *addr, kj::uint addrlen) const {
  return impl.shouldAllow(addr, addrlen);
}

}  // namespace kj_rs_io
