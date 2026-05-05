Detailed code examples for idiomatic KJ API usage patterns in workerd.

### Unwrapping `kj::Maybe`

**Always** use `KJ_IF_SOME`. **Never** dereference directly.

```cpp
// Correct:
KJ_IF_SOME(value, maybeValue) {
  use(value);  // value is a reference to the contained T
}

// Correct:
auto& value = KJ_ASSERT_NONNULL(maybeValue);  // asserts if maybeValue is none, otherwise gives T&
auto& value = KJ_REQUIRE_NONNULL(maybeValue);  // same but for preconditions
auto& value = JSG_REQUIRE_NONNULL(maybeValue, ErrorType, "message");  // same but with JavaScript exception type/message

// Wrong
auto& value = *maybeValue;      // doesn't exist
auto& value = maybeValue.value(); // not how KJ works
```

**Maybe** use `maybe.map(fn)` and `maybe.orDefault(val)` are useful for simple transforms/fallbacks.

When moving a value out of a `kj::Maybe`, use `kj::mv()` and remember to set the `kj::Maybe` to `kj::none` to avoid dangling references:

```cpp
KJ_IF_SOME(value, maybeValue) {
  auto movedValue = kj::mv(value);  // move out of the Maybe
  maybeValue = kj::none;            // set Maybe to none to avoid dangling reference
  use(movedValue);
}
```

### Unwrapping `kj::OneOf`

**Always** use `KJ_SWITCH_ONEOF` / `KJ_CASE_ONEOF`:

```cpp
KJ_SWITCH_ONEOF(variant) {
  KJ_CASE_ONEOF(s, kj::String) { handleString(s); }
  KJ_CASE_ONEOF(i, int) { handleInt(i); }
}
```

### Building Strings

**Always** use `kj::str()`, never `std::to_string` or `+` concatenation:

```cpp
kj::String msg = kj::str("count: ", count, ", name: ", name);
```

Use `kj::hex(n)` for hex output. Extend with `KJ_STRINGIFY(MyType)` or a `.toString()` method.
Use `"literal"_kj` suffix for `kj::StringPtr` literals (can be `constexpr`).

### Scope-Exit Cleanup

Use `KJ_DEFER` or `kj::defer()`:

```cpp
KJ_DEFER(close(fd));                          // block scope
auto cleanup = kj::defer([&]() { close(fd); }); // movable, for non-block lifetimes
```

Also: `KJ_ON_SCOPE_SUCCESS(...)` and `KJ_ON_SCOPE_FAILURE(...)` for conditional cleanup.

### Syscall Error Checking

Use `KJ_SYSCALL`, never manual errno checks:

```cpp
int n;
KJ_SYSCALL(n = read(fd, buf, size));          // throws on error, retries EINTR
KJ_SYSCALL_HANDLE_ERRORS(fd = open(path, O_RDONLY)) {
  case ENOENT: return nullptr;                // handle specific errors
  default: KJ_FAIL_SYSCALL("open()", error);
}
```

### Downcasting

Use `kj::downcast<T>` (debug-checked), not `static_cast` or `dynamic_cast`:

```cpp
auto& derived = kj::downcast<DerivedType>(baseRef);  // asserts in debug builds
```

Use `kj::dynamicDowncastIfAvailable<T>` only for optimizations (returns null without RTTI).

### Iteration Helpers

```cpp
for (auto i: kj::zeroTo(n)) { ... }        // 0..n-1
for (auto i: kj::range(a, b)) { ... }      // a..b-1
for (auto i: kj::indices(array)) { ... }    // 0..array.size()-1
```
