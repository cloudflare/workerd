// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative cases for the `custom-raw-v8-trycatch` query-based clang-tidy check
// defined in the workerd `.clang-tidy` config. None of the declarations below
// should be flagged.

namespace v8 {
class Isolate;
class TryCatch {
 public:
  explicit TryCatch(Isolate* isolate) {}
};
}  // namespace v8

namespace other {
// A different class with the same unqualified name.
class TryCatch {
 public:
  explicit TryCatch(v8::Isolate* isolate) {}
};
}  // namespace other

// GOOD: a reference to a TryCatch that the caller declared.
bool reference(v8::TryCatch& tryCatch) {
  return true;
}

// GOOD: a pointer to one.
bool pointer(v8::TryCatch* tryCatch) {
  return tryCatch != nullptr;
}

void unrelated(v8::Isolate* isolate) {
  // GOOD: not v8::TryCatch.
  other::TryCatch tryCatch(isolate);
}

void suppressed(v8::Isolate* isolate) {
  // GOOD: suppressed, with the reason.
  // NOLINTNEXTLINE(custom-raw-v8-trycatch): only reads HasCaught().
  v8::TryCatch tryCatch(isolate);
}
