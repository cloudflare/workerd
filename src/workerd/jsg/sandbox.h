// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8config.h>

#include <kj/common.h>

#include <cstdint>

namespace v8 {
class Isolate;
}

namespace workerd::jsg {

// A non-owning view of ArrayBuffer bytes. With the V8 sandbox enabled, the address is a 32-bit
// offset into the 4 GiB immediately after the JS pointer cage. Backing-store allocators must
// place the bytes in this second cage, with sandbox guard regions disabled. Construction
// aborts if a nonempty view cannot be encoded. Decoding uses the supplied isolate's cage base, so even
// a corrupted offset cannot select another isolate group's memory. The 32-bit length bounds
// overrun distance; it does not guarantee that the entire range remains inside the cage.
// Without the sandbox, the view stores an ordinary pointer.
class SandboxedBytes {
 public:
  SandboxedBytes() = default;
  SandboxedBytes(v8::Isolate* isolate, kj::ArrayPtr<const kj::byte> bytes);

  // The owner must keep the bytes alive. Access requires the owning isolate's lock and sandbox
  // memory permissions; this view retains neither the owner nor the isolate.
  kj::ArrayPtr<const kj::byte> get(v8::Isolate* isolate) const;

  size_t size() const {
    return length;
  }

 private:
#ifdef V8_ENABLE_SANDBOX
  uint32_t offset = 0;
#else
  const kj::byte* pointer = nullptr;
#endif
  uint32_t length = 0;
};

}  // namespace workerd::jsg
