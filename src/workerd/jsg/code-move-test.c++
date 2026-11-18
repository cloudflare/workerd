// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// V8 normally compacts code space only when that frees a whole page, which a test's small heap
// never reaches. --compact-on-every-full-gc makes every full GC compact it, so code moves. It
// applies to the whole process, so this test has its own binary.
// --expose-gc provides gc().
V8System v8System({"--allow-natives-syntax", "--compact-on-every-full-gc", "--expose-gc"},
    JitCodeEventTracking::YES);

struct EvalContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(EvalContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(EvalIsolate, EvalContext);

KJ_TEST("Code statistics count machine code the garbage collector moves") {
  Evaluator<EvalContext, EvalIsolate> e(v8System);
  auto before = e.getIsolate().getCodeStatistics();
  // The full GC runs inside the evaluation: each evaluation gets a new context, so afterwards
  // `square` and its optimized code would be dead, and there would be no code left to move.
  e.expectEval(R"(
    function square(x) { return x * x; }
    %PrepareFunctionForOptimization(square);
    square(4);
    %OptimizeFunctionOnNextCall(square);
    square(5);
    gc();
    square(6);
  )",
      "number", "36");
  auto after = e.getIsolate().getCodeStatistics();
  KJ_EXPECT(after.machineCodeMoveCount > before.machineCodeMoveCount, before.machineCodeMoveCount,
      after.machineCodeMoveCount);
}

}  // namespace
}  // namespace workerd::jsg::test
