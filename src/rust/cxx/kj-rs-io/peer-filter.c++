#include "kj-rs-io/peer-filter.h"

namespace kj_rs_io {

PeerFilter::PeerFilter(kj::ArrayPtr<const kj::StringPtr> allow,
    kj::ArrayPtr<const kj::StringPtr> deny,
    kj::Rc<PeerFilter> next)
    : next(kj::mv(next)),
      impl(allow, deny, this->next->impl) {}

bool PeerFilter::shouldAllow(const struct sockaddr *addr, kj::uint addrlen) {
  return impl.shouldAllow(addr, addrlen);
}

bool PeerFilter::shouldAllowParse(const struct sockaddr *addr, kj::uint addrlen) const {
  return impl.shouldAllowParse(addr, addrlen);
}

}  // namespace kj_rs_io
