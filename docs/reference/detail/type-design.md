# Type Design, Inheritance & Thread Safety

Class design rules, inheritance patterns, and constness semantics in workerd.

---

### Two Kinds of Types

**Value types** (data):

- Copyable/movable, compared by value, can be serialized
- No virtual methods; use templates for polymorphism
- Always have move constructors

**Resource types** (objects with identity):

- Not copyable, not movable — use `kj::Own<T>` on the heap for ownership transfer
- Use `KJ_DISALLOW_COPY_AND_MOVE` to prevent accidental copying/moving
- Compared by identity, not value
- May use inheritance and virtual methods

### Inheritance

- A class is either an **interface** (no data members, only pure virtual methods) or an
  **implementation** (no non-final virtual methods). Never mix — this causes fragile base class problems.
- Interfaces should **NOT** declare a destructor.
- Multiple inheritance is allowed and encouraged (typically inheriting multiple interfaces).
- Implementation inheritance is acceptable for composition without extra heap allocations.

### Constness and Thread Safety

- Treat constness as **transitive** — a const pointer to a struct means its contained pointers
  are also effectively const.
- **`const` methods must be thread-safe** for concurrent calls. Non-const methods require
  exclusive access. `kj::MutexGuarded<T>` enforces this: `.lockShared()` returns `const T&`,
  `.lockExclusive()` returns `T&`.
- Copyable classes with pointer members: declare copy constructor as `T(T& other)` (not
  `T(const T& other)`) to prevent escalating transitively-const references. Or inherit
  `kj::DisallowConstCopy`.
- Only hold locks for the minimum duration necessary to access or modify the guarded data.

### Other Rules

- **No global constructors**: Don't declare static/global variables with dynamic constructors.
  Global `constexpr` constants are fine.
- **No `dynamic_cast` for polymorphism**: Don't use long if/else chains casting to derived types.
  Extend the base interface instead. `dynamic_cast` is OK for optimizations or diagnostics
  (test: if it always returned null, would the code still be correct?).
- **No function/method pointers**: Use templates over functors, or `kj::Function<T>`.
- **Prefer references over pointers**: Unambiguously non-null.
