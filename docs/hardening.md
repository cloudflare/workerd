# Hardening

Hardening process replaces unsafe C++ constructs with others that either completely eliminate
undefined behaviors or replace them by runtime checks.

## Runtime Checks

The set of `KJ_*` macros should be used to assert correct runtime behavior.

## Bounds/Null checking

We have `kj_enable_irequire` flag enabled in all configurations, which enables KJ bounds and null
checking in built-in data structures. There is no need to duplicate such checks in user code
unless more debug information is required.

## Raw References

Raw reference types are incredibly dangerous, because they don't offer any lifetime protection.

They are especially dangerous when used as data fields, or in asynchronous context (coroutines or
lambda captures).

The alternative is to use:

- `kj::Ptr` for references that never outlive the object and `kj::Weak` when you can't guarantee
  that. These smart pointers are obtained either from `kj::Pin` for inline storage or inheriting and
  exposing `kj::PtrTarget` methods for heap-allocated objects.

- `kj::Rc`/`kj::WeakRc` for objects that are (or should be) reference counted.

When using these, all references in all call paths ending on smart pointers need to be adjusted to
use them as well.

It could still be acceptable to use references as parameters when:

- function is synchronous
- its scope is very small and clear
- it _clearly_ can't result in indirect deallocation through the use of other objects or methods

When in doubt - use smart pointers.

## Raw Pointers

All Raw References advice applies to raw pointers as well. If `null` is a valid state, then weak
pointers should be used rather than wrapping smart pointers into `kj::Maybe`.



