// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Positive cases for the `custom-iocontext-run-manual-capture` query-based
// clang-tidy check defined in the workerd `.clang-tidy` config. Every call to
// IoContext::run() below passes a lambda that captures the IoContext manually,
// which the check should flag.

namespace workerd {

struct Worker {
  struct Lock {};
};

// Minimal stand-in for the real IoContext. The check only cares about the
// fully-qualified name `::workerd::IoContext` and a method named `run`.
class IoContext {
 public:
  template <typename Func>
  void run(Func&& func) {}
};

// A type with a non-trivial destructor. When captured, it makes the closure's
// own destructor non-trivial, so the lambda argument to run() is wrapped in a
// CXXBindTemporaryExpr in the AST. The matcher must still see through it.
struct OwnsResource {
  ~OwnsResource();
  OwnsResource clone();
};

void captureByReference(IoContext& ctx) {
  // Captures `ctx` (an IoContext&) explicitly by reference.
  ctx.run([&ctx](Worker::Lock& lock) { (void)ctx; });
}

void captureByPointer(IoContext* ctx) {
  // Captures `ctx` (an IoContext*) explicitly by value.
  ctx->run([ctx](Worker::Lock& lock) { (void)ctx; });
}

void captureByDefaultReference(IoContext& ctx) {
  // Default-capture-by-reference implicitly captures `ctx`.
  ctx.run([&](Worker::Lock& lock) { (void)ctx; });
}

void captureAlongsideNonTrivialClosure(IoContext& ctx, OwnsResource& resource) {
  // The init-capture gives the closure a non-trivial destructor, wrapping the
  // lambda argument in a CXXBindTemporaryExpr. The check must still flag the
  // manual IoContext capture (regression test for ignoringImplicit vs
  // ignoringParenImpCasts).
  ctx.run([&ctx, held = resource.clone()](Worker::Lock& lock) { (void)ctx; });
}

}  // namespace workerd
