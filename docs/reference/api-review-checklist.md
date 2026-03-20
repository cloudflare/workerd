These checkists are to be used when performing code reviews for API design, performance optimization, security vulnerabilities, and standards compliance. They are not exhaustive but cover common patterns and issues to look for. Always consider the specific context of the code being reviewed and apply judgment accordingly.

### Performance Optimization

End-to-end, real-world performance is the priority over micro-optimizations.

- **Always** identify unnecessary allocations and copies (keeping in mind that we're using tcmalloc)
- **Always** identify opportunities for move semantics that avoid unnecessary copies
- **Always** identify hot code paths that could benefit from optimization
- **Always** review data structure choices that would improve performance (e.g. using `workerd::RingBuffer` instead of `kj::Vector` for bounded queues)
- **Always** analyze and suggest improvements for cache locality and memory layout
- **Always** identify lock contention and synchronization overhead
- **Always** identify inefficient string operations or repeated parsing or copying
- **Always** avoid premature optimization; focus on clear evidence of performance issues
- **Never** make vague or grandiose claims of performance improvements without clear reasoning or evidence
- **Always** suggest improvements for better use of KJ library features for performance
- **Always** consider overall system performance, including interactions between components, I/O, and network latency.
- **Always** avoid optimizations that improve microbenchmarks but do not translate to real-world gains.
- **Always** evaluate trade-offs: an optimization that benefits one workload may degrade another. Aim for broad benefits.
- **Always** consider scalability: solutions should maintain or improve performance as workloads increase.
- It's ok to suggest low-risk micro-optimizations as low-hanging fruit, but they are not the primary focus.

### API Design & Compatibility

- **Always** evaluate API ergonomics and usability
- **Always** review backward compatibility implications
- **Always** check for proper use of compatibility flags (`compatibility-date.capnp`) and
  autogates (`util/autogate.h/c++`). Use the `compat-date-at` tool to look up flag details.
  Use the `next-capnp-ordinal` tool when adding new flags.
- Use the `cross-reference` tool to look up JSG registration, type groups, and test
  coverage for API classes under review.
- Use the `jsg-interface` tool to extract the full structured JS API (methods,
  properties, constants, nested types, inheritance) for a class under review.
- **Always** identify breaking changes that need feature flags or autogates
- **Error type changes are generally not breaking.** Changing the type of error thrown (e.g., from a
  generic `kj::Exception` to a JS `TypeError`, or between JS error types) is not normally considered
  a breaking change because well-written user code should not depend on specific error types. The
  exception is when the change removes properties that code could reasonably depend on — for
  instance, changing from a `DOMException` to a `TypeError` is breaking because `DOMException` has
  properties like `code` and `name` with specific values that `TypeError` does not. Use judgment:
  if the error type change could plausibly break real user code in a substantial way, treat it as
  breaking and gate it behind a compat flag.
- **Never** recommend or approve the removal of an existing compatibility flag.
- **Never** recommend or approve changing/inverting the meaning of an existing compatibility flag.
- **Never** suggest the compatibility flag checks are "dead code" that can be removed. Compatibility flags are permanent and must be maintained indefinitely, even if there is nothing apparently depending on them.
- **Always** review consistency with existing API patterns

### Security Vulnerabilities

- **Always** identify memory safety issues using the `docs/reference/cpp-safety-review-checklist.md` checklist.
- **Always** identify injection vulnerabilities
- **Always** review input validation and sanitization.
- **Always** identify potential timing side channels
- **Always** analyze privilege boundaries and capability checks
- **Always** look for information disclosure risks
- **Always** review cryptographic usage patterns
- **Always** identify TOCTOU (time-of-check-time-of-use) issues
- workerd favors use of capability-based security

### Standards Spec Compliance

- **Always** review adherence to relevant web standards (Fetch, Streams, WebCrypto, etc.).
  Variation from spec behavior is allowed but must be justified and documented.
- **Always** identify deviations from spec behavior and suggest improvements for better alignment
- **Always** review documentation accuracy against specs and implementation
- **Always** identify missing features required by specs
- **Always** identify interoperability issues with other implementations
- **Always** identify edge cases not handled per specs
- **Always** reference specific spec sections when flagging deviations

### Runtime-Specific Notes

- **KJ event loop**: workerd uses kj's single-threaded event loop, not Node.js-style libuv.

