// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Negative cases for the `custom-iocontext-run-manual-capture` query-based
// clang-tidy check defined in the workerd `.clang-tidy` config. None of the
// calls below should be flagged: they either use the two-argument lambda form
// or do not capture the IoContext at all.

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

// A different class that also has a `run` method taking a lambda; calling it
// must not be flagged even if the lambda captures an IoContext, because the
// method does not belong to IoContext.
class NotAnIoContext {
 public:
  template <typename Func>
  void run(Func&& func) {}
};

void twoArgumentForm(IoContext& ctx) {
  // GOOD: IoContext is received as a parameter, not captured.
  ctx.run([](Worker::Lock& lock, IoContext& ioContext) { (void)ioContext; });
}

void noCapture(IoContext& ctx) {
  // GOOD: nothing captured.
  ctx.run([](Worker::Lock& lock) {});
}

void unrelatedCapture(IoContext& ctx, int value) {
  // GOOD: captures an unrelated variable, not the IoContext.
  ctx.run([value](Worker::Lock& lock) { (void)value; });
}

void runOnOtherClass(NotAnIoContext& other, IoContext& ctx) {
  // GOOD: `run` is not IoContext::run, so capturing the IoContext is fine here.
  other.run([&ctx](Worker::Lock& lock) { (void)ctx; });
}

}  // namespace workerd
