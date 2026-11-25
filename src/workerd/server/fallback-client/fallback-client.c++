// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "fallback-client.h"

#include <kj-rs/kj-rs.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/debug.h>
#include <kj/vector.h>

namespace workerd::fallback {
namespace {

using kj_rs::Rust;

::rust::Vec<uint8_t> toRustVec(kj::ArrayPtr<const kj::byte> bytes) {
  ::rust::Vec<uint8_t> vec;
  vec.reserve(bytes.size());
  for (auto b: bytes) vec.push_back(b);
  return vec;
}

}  // namespace

FallbackServiceClient::FallbackServiceClient(kj::StringPtr address)
    : FallbackServiceClient(newFallbackClient(address.asBytes().as<Rust>())) {}

FallbackServiceClient::FallbackServiceClient(NewClient parts)
    : thread(kj::heap<kj::Thread>(
          [worker = kj::mv(parts.worker)]() mutable { runFallbackWorker(kj::mv(worker)); })),
      client(kj::mv(parts.client)) {}

FallbackServiceClient::~FallbackServiceClient() noexcept(false) {}

ModuleOrRedirect FallbackServiceClient::tryResolve(Version version,
    ImportType type,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes) {
  KJ_ASSERT(!busy.exchange(true), "FallbackServiceClient does not support concurrent requests");
  KJ_DEFER(busy.store(false));

  kj::Vector<Attribute> attrs(attributes.size());
  for (auto& attr: attributes) {
    attrs.add(
        Attribute{.name = toRustVec(attr.key.asBytes()), .value = toRustVec(attr.value.asBytes())});
  }
  auto resolution = client->resolve(version, type, specifier.asBytes().as<Rust>(),
      rawSpecifier.asBytes().as<Rust>(), referrer.asBytes().as<Rust>(),
      ::rust::Slice<const Attribute>(attrs.begin(), attrs.size()));

  auto bytes = kj::from<Rust>(resolution.bytes);
  switch (resolution.kind) {
    case ResolutionKind::ShutDown:
      KJ_FAIL_REQUIRE("FallbackServiceClient has been shut down");
    case ResolutionKind::NotFound:
      return kj::none;
    case ResolutionKind::Redirect:
      return ModuleOrRedirect(kj::heapString(bytes.asChars()));
    case ResolutionKind::Module: {
      // A rust::Vec<uint8_t> need not be word-aligned, so the message is copied into words.
      auto words = kj::heapArray<capnp::word>(bytes.size() / sizeof(capnp::word));
      words.asBytes().copyFrom(bytes.first(words.asBytes().size()));
      // The module, a Wasm module especially, may be larger than the default traversal limit.
      capnp::FlatArrayMessageReader reader(words, {.traversalLimitInWords = kj::maxValue});
      return ModuleOrRedirect(capnp::clone(reader.getRoot<server::config::Worker::Module>()));
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace workerd::fallback
