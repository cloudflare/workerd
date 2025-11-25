# WebIDL to JSG Code Generator - Development Guide

## Overview

This tool generates C++ header and implementation files from WebIDL definitions for the workerd JSG (JavaScript Glue) binding system. It automates the creation of boilerplate code for exposing C++ APIs to JavaScript.

## Project Structure

```
webidl-codegen/
├── src/
│   ├── cli.js              # Command-line interface and summary reporting
│   ├── parser.js           # WebIDL parsing using webidl2 library
│   ├── generator.js        # C++ header generation (main logic)
│   ├── impl-generator.js   # C++ implementation stub generation
│   ├── type-mapper.js      # WebIDL → C++ type mapping
│   └── protected-regions.js # Code preservation during regeneration
├── examples/               # Example WebIDL files for testing
├── README.md              # User-facing documentation
└── DEVELOPMENT.md         # This file

```

## Core Architecture

### Type System

The generator distinguishes between different WebIDL constructs:

1. **Interfaces** → `class X : public jsg::Object` (wrapped in `jsg::Ref<X>`)
2. **Interface Mixins** → Plain C++ classes (used as base classes, not wrapped)
3. **Dictionaries** → C++ structs with `JSG_STRUCT`
4. **Enums** → C++ `enum class`
5. **Typedefs** → C++ `using` aliases
6. **Callbacks** → C++ `using` with `jsg::Function<...>`
7. **Namespaces** → NOT YET IMPLEMENTED (planned: `jsg::Object` with static methods)

### Forward Declarations

The generator creates forward declarations for:
- All interface types (both local and external)
- External interfaces are in `skipInterfaces` set (e.g., `Event`, `EventTarget`, `AbortSignal`)
- Local interfaces defined in the same file

Forward declarations are filtered to exclude:
- Primitive types
- External enums (configured in `externalEnums` set in type-mapper.js and generator.js)
- Local non-interfaces (dictionaries, enums, typedefs, mixins)

### External Types

**External Enums** (manually configured):
- Listed in `externalEnums` set in both `type-mapper.js` and `generator.js`
- Example: `ReferrerPolicy` from Referrer Policy spec
- Not wrapped in `jsg::Ref<>`, not forward declared
- Used directly as enum types

**External Interfaces** (skipInterfaces):
- Default set includes: `Event`, `EventTarget`, `EventListener`, `AbortSignal`, `AbortController`
- Can be extended via `--skip-interface` CLI option
- Forward declared but not generated
- Wrapped in `jsg::Ref<>` when referenced

### Extended Attributes (All Jsg-Prefixed)

Custom WebIDL extended attributes for JSG bindings (all prefixed with `Jsg` to avoid WebIDL spec conflicts):

- `[JsgCompatFlag=Name]` - Conditional compilation when flag is ON
- `[JsgCompatFlagOff=Name]` - Conditional compilation when flag is OFF (for mutually exclusive signatures)
- `[JsgMethodName=name]` - Custom C++ method name (for overloads, reserved keywords)
- `[JsgTsOverride="..."]` - TypeScript type override via `JSG_TS_OVERRIDE`
- `[JsgTsDefine="..."]` - TypeScript type definitions via `JSG_TS_DEFINE` or `JSG_STRUCT_TS_DEFINE`
- `[JsgTsRoot]` - Mark dictionary as TypeScript root type via `JSG_STRUCT_TS_ROOT()`
- `[JsgPropertyScope=instance|prototype]` - Control JSG_*_INSTANCE_PROPERTY vs JSG_*_PROTOTYPE_PROPERTY
- `[JsgInternal]` - Exclude dictionary field from `JSG_STRUCT` parameters
- `[JsgCode="..."]` - Custom C++ constructor/method declarations

**Note**: All attributes were prefixed with `Jsg` in November 2025 to protect against future WebIDL spec additions.

### Mixin Handling

**Current Implementation** (as of November 2025):
- Mixins are plain C++ classes (no `jsg::Object` base)
- Interfaces using mixins inherit from `jsg::Object` first, then mixin classes
- Mixin methods are registered in the interface's `JSG_RESOURCE_TYPE`
- `includes` statements are processed to build inheritance chain

**Example**:
```webidl
interface mixin Body {
  readonly attribute boolean bodyUsed;
};
interface Request {};
Request includes Body;
```

Generates:
```cpp
class Body {  // No jsg::Object base
public:
  bool getBodyUsed(jsg::Lock& js);
};

class Request : public jsg::Object, public Body {
public:
  JSG_RESOURCE_TYPE(Request) {
    JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);
  }
};
```

### Protected Regions

The generator supports **update mode** to preserve manual code:

```cpp
// BEGIN MANUAL SECTION: ClassName::public
// User code here is preserved
// END MANUAL SECTION: ClassName::public
```

Protected regions exist in:
- Class public section
- Class private section
- Implementation file method bodies

**Incremental Mode**: Only generates stubs for new methods, preserving existing implementations.

## Generation Flow

### Header Generation (generator.js)

1. Parse WebIDL with `webidl2`
2. Process mixins and `includes` statements
3. Generate forward declarations
4. Sort definitions by dependency order:
   - Typedefs (may be used by other types)
   - Enums (simple value types)
   - Callbacks (function types)
   - Dictionaries (value types)
   - Interface mixins (base classes)
   - Interfaces (complex types)
5. For each definition, generate C++ code
6. Track what was generated in `summary` object

### Implementation Generation (impl-generator.js)

1. Parse same WebIDL definitions
2. Generate implementation stubs with TODO comments
3. Detect SelfRef, Unimplemented, WontImplement patterns
4. Smart return value generation (kj::StringPtr for string literals)
5. Respect protected regions if updating existing file

### CLI Summary (cli.js)

After generation, prints summary:
```
============================================================
Generation Summary
============================================================

Header:
  ✓ 4 interface(s): Headers, Request, Response, FetchLaterResult
  ✓ 1 mixin(s): Body
  ✓ 3 dictionar(y/ies): RequestInit, ResponseInit, DeferredRequestInit
  ...

Implementation:
  ✓ 4 implementation(s): Headers, Request, Response, FetchLaterResult

Skipped (--skip-interface):
  ⊘ mixin: WindowOrWorkerGlobalScope
  ⊘ interface: Window

Unsupported (not implemented):
  ✗ namespace: Console - Namespaces not yet implemented
  ✗ partial interface: Window - Partial interfaces not yet supported

------------------------------------------------------------
Total: 20 generated, 2 skipped, 2 unsupported
============================================================
```

## Current Limitations & Future Work

### Not Yet Implemented

1. **Namespaces** - Planned approach:
   - Generate as `jsg::Object` class with static methods
   - Use `JSG_STATIC_METHOD` and `JSG_STATIC_*_PROPERTY` macros
   - No special `JSG_NAMESPACE` macro (doesn't exist)
   - Future: Handle method destructuring safety

2. **Partial Definitions** - Planned approach:
   - Merge all partials before generation
   - `collectPartials()` → `mergePartials()` → generate merged result
   - Essential for multi-spec scenarios (Window extended by many APIs)
   - Currently: Partials generate separately (duplicate names possible)

3. **Iterables/Maplike/Setlike** - Not supported

4. **Cross-file mixins** - Mixins must be in same file as interface

### Known Issues

- **External type configuration** requires manual source code changes
  - `externalEnums` set in type-mapper.js and generator.js
  - `skipInterfaces` set in generator.js and impl-generator.js
  - Future: Config file or CLI flags

- **Partial interfaces** treated as separate interfaces
  - Can result in duplicate names in summary
  - Need merging implementation before full support

## Type Mapping Details

See `type-mapper.js` for full mappings:

### Primitives
- `boolean` → `bool`
- `byte` → `int8_t`
- `octet` → `uint8_t`
- `short` → `int16_t`
- `unsigned short` → `uint16_t`
- `long` → `int32_t`
- `unsigned long` → `uint32_t`
- `long long` → `int64_t`
- `unsigned long long` → `uint64_t`
- `float` → `float`
- `double` → `double`
- `DOMString`, `ByteString`, `USVString` → `jsg::DOMString` (alias for `kj::String`)
- `any` → `jsg::JsValue`
- `object` → `jsg::JsObject`
- `undefined` → `void`

### Generic Types
- `Promise<T>` → `jsg::Promise<T>`
- `sequence<T>` → `kj::Array<T>`
- `FrozenArray<T>` → `kj::Array<T>`
- `record<K, V>` → `jsg::Dict<K, V>`

### Union Types
- `(A or B or C)` → `kj::OneOf<A, B, C>`

### Modifiers
- `optional T` → `jsg::Optional<T>`
- Nullable types (`T?`) → `jsg::Optional<T>`

### Buffer Types
- `ArrayBuffer` → `jsg::BufferSource`
- `BufferSource` → `jsg::BufferSource`
- `Uint8Array`, etc. → `jsg::BufferSource`

## Testing

### Example Files
All examples in `examples/` directory should generate successfully:
- `compat-flags.webidl` - Extended attributes showcase
- `fetch.webidl` - Real-world Fetch API subset
- `selfref.webidl` - Dictionary self-references and internal fields
- `mixin-test.webidl`, `multiple-mixins.webidl` - Mixin patterns
- `dict-inheritance.webidl` - Dictionary inheritance
- `union-types.webidl` - Union type handling
- And others...

### Quick Test
```bash
cd examples
for f in *.webidl; do
  node ../src/cli.js -o /tmp/test.h --impl /tmp/test.c++ "$f"
done
```

All should generate without errors.

## Key Files to Understand

### generator.js (Core Logic)
- `generate()` - Main entry point
- `generateInterface()` - Interface → class with JSG_RESOURCE_TYPE
- `generateMixin()` - Mixin → plain class
- `generateDictionary()` - Dictionary → struct with JSG_STRUCT
- `generateForwardDeclarations()` - Smart forward declaration logic
- `processMixinsAndIncludes()` - Build mixin inheritance chain
- Extended attribute extraction methods (getCompatFlag, getMethodName, etc.)

### type-mapper.js
- `mapType()` - Core type mapping logic
- `externalEnums` set - Manual configuration for external enums
- `isPrimitive()`, `isBufferType()`, etc. - Type classification

### impl-generator.js
- Similar structure to generator.js but for .c++ files
- `generateMethodStub()` - Smart stub generation with TODOs
- Auto-detection of SelfRef, Unimplemented, WontImplement

## Development Workflow

### Adding New Features

1. **Update parser.js** if new WebIDL constructs need validation
2. **Update type-mapper.js** if new type mappings needed
3. **Update generator.js** for header generation
4. **Update impl-generator.js** for implementation stubs
5. **Update README.md** with user-facing documentation
6. **Add example** in `examples/` directory
7. **Test** all examples still generate

### Adding Extended Attributes

1. Add getter method in generator.js (e.g., `getMyAttribute()`)
2. Use attribute in appropriate generation method
3. Update impl-generator.js if needed for stubs
4. Document in README.md under "Extended Attributes"
5. Add example in `examples/compat-flags.webidl`

### Debugging

- Set `DEBUG=1` environment variable for stack traces
- Check generated summary for what was skipped/unsupported
- Compare generated .h file against expected JSG patterns
- Test with real workerd builds if possible

## Code Style

- Use clear variable names (not abbreviated)
- Add comments explaining JSG-specific decisions
- Keep methods focused and single-purpose
- Generate idiomatic C++ code (proper spacing, naming)
- Preserve manual code through protected regions

## Dependencies

- `webidl2` - WebIDL parsing (mature, well-maintained)
- Node.js built-ins only (fs, path)
- No other external dependencies

## Important Constants

### Skip Lists
```javascript
// generator.js, impl-generator.js
this.skipInterfaces = new Set([
  'Event',
  'EventTarget',
  'EventListener',
  'AbortSignal',
  'AbortController',
]);
```

### External Enums
```javascript
// generator.js, type-mapper.js
this.externalEnums = new Set([
  'ReferrerPolicy',
]);
```

## Next Steps / TODO

Priority improvements:
1. Implement namespace support (jsg::Object with static methods)
2. Implement partial definition merging
3. Consider config file for external types
4. Add validation for duplicate member names
5. Support cross-file mixin references
6. Better error messages with line numbers
7. Optimize generated code (remove unnecessary includes)

## Questions to Consider

- Should we auto-detect more external enums?
- Better way to handle cross-file dependencies?
- Config file format for external types?
- Should partials merge automatically or require flag?
- How to handle namespace registration in workerd?
