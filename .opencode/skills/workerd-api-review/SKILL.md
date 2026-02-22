---
name: workerd-api-review
description: Performance optimization, API design & compatibility, security vulnerabilities, and standards spec compliance for workerd code review. Covers tcmalloc-aware perf analysis, compat flags, autogates, web standards adherence, and security patterns. Load this skill when reviewing API changes, performance-sensitive code, security-relevant code, or standards implementations.
---

## API, Performance, Security & Standards Analysis

Load this skill when analyzing code for performance, API design, backward compatibility, security, or web standards compliance.

---

### Performance Optimization

- Identify unnecessary allocations and copies (keeping in mind that we're using tcmalloc)
- Find opportunities for move semantics
- Spot hot paths that could benefit from optimization
- Review data structure choices for access patterns
- Analyze cache locality and memory layout
- Identify lock contention and synchronization overhead
- Look for inefficient string operations or repeated parsing
- Avoid premature optimization; focus on clear evidence of performance issues
- Do not make vague or grandiose claims of performance improvements without clear reasoning or data
- Suggest improvements for better use of KJ library features for performance
- End-to-end, real-world performance is the priority over micro-optimizations.
  - Consider overall system performance, including interactions between components, I/O, and network latency.
  - Avoid optimizations that improve microbenchmarks but do not translate to real-world gains.
  - It's ok to suggest low-risk micro-optimizations as low-hanging fruit, but they are not the primary focus.
  - Validate claims with real-world testing, benchmarking, or evidence-backed analysis.
  - Evaluate trade-offs: an optimization that benefits one workload may degrade another. Aim for broad benefits.
  - Consider scalability: solutions should maintain or improve performance as workloads increase.

### API Design & Compatibility

- Evaluate API ergonomics and usability
- Review backward compatibility implications
- Check for proper use of compatibility flags (`compatibility-date.capnp`) and
  autogates (`util/autogate.h/c++`)
- Identify breaking changes that need feature flags or autogates
- Analyze public vs internal API boundaries
- Review consistency with existing API patterns

### Security Vulnerabilities

- Identify injection vulnerabilities
- Identify memory safety issues that could lead to exploits or crashes
- Review input validation and sanitization
- Check for and identify potential timing side channels
- Analyze privilege boundaries and capability checks
- Look for information disclosure risks
- Review cryptographic usage patterns
- Identify TOCTOU (time-of-check-time-of-use) issues
- Remember that workerd favors use of capability-based security

### Standards Spec Compliance

- Review adherence to relevant web standards (Fetch, Streams, WebCrypto, etc.)
- Identify deviations from spec behavior and suggest improvements for better alignment
- Review documentation accuracy against specs
- Identify missing features required by specs
- Suggest prioritization for spec compliance work
- Identify interoperability issues with other implementations
- Identify edge cases not handled per specs
- Reference specific spec sections when flagging deviations

---

### Runtime-Specific Notes

- **KJ event loop**: workerd uses kj's single-threaded event loop, not Node.js-style libuv. Blocking the event loop blocks all concurrent requests on that thread. Flag synchronous I/O, expensive computation, or unbounded loops on the event loop thread.
- **tcmalloc**: workerd uses tcmalloc. Thread-local caches reduce contention but increase per-thread memory overhead. Focus optimization on reducing allocation count (especially in hot paths) rather than individual allocation sizes. Do not suggest switching to standard malloc.
- **Cap'n Proto zero-copy**: Cap'n Proto messages are zero-copy and use arena allocation. Do not suggest copying data out of Cap'n Proto messages "for safety" unless there is a concrete lifetime issue. Suggest using Cap'n Proto's traversal limits to prevent resource exhaustion when processing untrusted messages.
