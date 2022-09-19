All files in this folder will be parsed to find types and methods to expose publicly in
TypeScript and/or Rust. Methods/types that shouldn't be exposed should be prefixed with "internal"
(or reference such types in the argument list for methods); the string is case insensitive.

Additionally, the public visibility of such types may be further adjusted/tweaked in
tools/autodecl/overrides/internal.d.ts

Even better is to hide such APIs in the internal-api folder when possible.

The internal type synchronization can be forced on by building `internal-type-declarations` with the
intention that we'll have a separate private repo for that.
