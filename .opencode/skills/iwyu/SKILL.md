---
name: iwyu
description: Follow IWYU guidelines to improve build caching. This entails adding/removing C++
includes as well as Bazel deps and implementation_deps when appropriate. Load when making non-
trivial changes to C++ includes or Bazel target declarations.
---

## IWYU FOR C++ TARGETS

Adapted from [include-what-you-use](https://github.com/include-what-you-use/include-what-you-use/blob/master/docs/WhyIWYU.md).
Small targets with accurate deps mean a change rebuilds and retests fewer targets.

- Don't add `#include`s a file doesn't use or `deps` a target doesn't use. When a change removes
  the last use of a header, delete its `#include`, then the dep if it's now unused.
- Put deps whose headers are included only from `.c++` files in `implementation_deps`, so they
  don't propagate to dependents.
- Prefer direct includes and deps over relying on transitive ones, but don't add a long list just
  to spell out what you already get transitively. For instance, most workerd C++ sources depend on
  KJ but we don't declare this dependency for each target separately, or include kj/common.h,
  kj/string.h and so on each time. Direct includes that are already present and being used should
  generally stay in place.
- Give each `.h`/`.c++` pair its own `wd_cc_library`, named after the file (e.g.
  `:tracked-wasm-instance`). A header without a `.c++` gets its own target too, with the deps it
  needs to compile on its own.
- Keep each header in exactly one target, together with its `.c++` if it has one. Exposing a
  header from a target that doesn't build its `.c++` lets code call its functions without
  depending on their definitions, which shows up later as undefined-symbol link errors.
- If splitting a pair out would create a dependency cycle, leave it in its combined target (e.g.
  `//src/workerd/io`) instead of separating the header from its `.c++`. Don't add new files to
  such targets if they can have their own.