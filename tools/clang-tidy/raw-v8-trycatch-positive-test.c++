// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive cases for the `custom-raw-v8-trycatch` query-based clang-tidy check
// defined in the workerd `.clang-tidy` config. Each declaration below should be
// flagged.

// Minimal stand-in for the real v8::TryCatch. The check only cares about the
// fully-qualified name `::v8::TryCatch`.
namespace v8 {
class Isolate;
class TryCatch {
 public:
  explicit TryCatch(Isolate* isolate) {}
};
}  // namespace v8

void local(v8::Isolate* isolate) {
  // BAD: a local TryCatch.
  v8::TryCatch tryCatch(isolate);
}

struct Holder {
  // BAD: a TryCatch member.
  v8::TryCatch tryCatch;
  explicit Holder(v8::Isolate* isolate): tryCatch(isolate) {}
};

void usingDeclaration(v8::Isolate* isolate) {
  using v8::TryCatch;
  // BAD: the same type, spelled without the namespace.
  TryCatch tryCatch(isolate);
}
