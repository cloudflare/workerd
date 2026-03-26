#pragma once

#include <workerd/util/autogate.h>

#include <kj-rs/convert.h>
#include <rust/cxx.h>

#include <kj/string.h>

namespace workerd::rust::jsg::autogate {

// Thin wrapper around workerd::util::Autogate::isEnabled().
// The key is a string matching the kebab-case name (e.g. "rust-backed-node-buffer").
inline bool is_enabled(::rust::String key) {
  auto keyStr = kj::str(kj::from<kj_rs::Rust>(key));
  for (auto i = 0u; i < static_cast<unsigned>(util::AutogateKey::NumOfKeys); ++i) {
    auto k = static_cast<util::AutogateKey>(i);
    if (kj::str(k) == keyStr) {
      return util::Autogate::isEnabled(k);
    }
  }
  return false;
}

}  // namespace workerd::rust::jsg::autogate
