// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sandbox.h"

#include <v8-internal.h>
#include <v8-isolate.h>
#include <v8-platform.h>

#include <kj/debug.h>

#include <cstdlib>

namespace workerd::jsg {

#ifdef V8_ENABLE_SANDBOX
namespace {

constexpr uint64_t CAGE_SIZE = uint64_t{1} << 32;
static_assert(v8::internal::kPtrComprCageReservationSize == CAGE_SIZE);
static_assert(v8::internal::kSandboxSize == 2 * CAGE_SIZE);

uintptr_t backingStoreCageBase(v8::Isolate* isolate) {
  // The sandbox reservation starts at the JS cage because sandbox guard regions are disabled.
  return isolate->GetGroup().GetSandboxAddressSpace()->base() + CAGE_SIZE;
}

}  // namespace
#endif

SandboxedBytes::SandboxedBytes(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> bytes) {
  if (bytes.size() == 0) return;
#ifdef V8_ENABLE_SANDBOX
  auto start = reinterpret_cast<uintptr_t>(bytes.begin());
  auto relative = start - backingStoreCageBase(isolate);
  if (bytes.size() > static_cast<uint32_t>(kj::maxValue) || relative >= CAGE_SIZE ||
      bytes.size() > CAGE_SIZE - relative) {
    KJ_LOG(FATAL, "byte view cannot be encoded in the backing-store cage", bytes.begin(),
        bytes.size());
    abort();
  }
  offset = static_cast<uint32_t>(relative);
#else
  KJ_REQUIRE(
      bytes.size() <= static_cast<uint32_t>(kj::maxValue), "byte view exceeds 32-bit length");
  pointer = bytes.begin();
#endif
  length = static_cast<uint32_t>(bytes.size());
}

kj::ArrayPtr<const kj::byte> SandboxedBytes::get(v8::Isolate* isolate) const {
  if (length == 0) return nullptr;
#ifdef V8_ENABLE_SANDBOX
  return kj::arrayPtr(
      reinterpret_cast<const kj::byte*>(backingStoreCageBase(isolate) + offset), length);
#else
  return kj::arrayPtr(pointer, length);
#endif
}

}  // namespace workerd::jsg
