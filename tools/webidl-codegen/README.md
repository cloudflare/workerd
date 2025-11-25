# WebIDL to JSG Code Generator

A tool for generating JSG C++ bindings from WebIDL interface definitions, with support for both header declarations and implementation stubs.

## Quick Start

```bash
# Install dependencies
cd tools/webidl-codegen
npm install

# Generate header and implementation stubs
node src/cli.js -o api.h --impl api.c++ example.webidl

# Edit api.c++ with your logic, then regenerate header as needed
node src/cli.js -o api.h example.webidl
```

## Installation

```bash
cd tools/webidl-codegen
npm install
```

## Usage

### CLI Options

```
Usage: webidl-codegen [options] <input.webidl>

Options:
  -o, --output <file>       Output header file (default: stdout)
  --impl <file>             Generate implementation stub file (.c++)
  --header <file>           Header file path to include in implementation
                            (auto-detected from -o if not specified)
  --skip-interface <name>   Skip generation for specific interface or mixin
                            (can be used multiple times)
  --update                  Update mode: preserve manual sections in existing files
  --incremental             Incremental mode: only generate stubs for new methods (requires --update)
  -n, --namespace <ns>      C++ namespace (default: workerd::api)
  -h, --help                Show help message
```

### Command Line Examples

```bash
# Generate header only to stdout
node tools/webidl-codegen/src/cli.js tools/webidl-codegen/examples/text-encoder.webidl

# Generate header to file
node tools/webidl-codegen/src/cli.js -o output.h tools/webidl-codegen/examples/text-encoder.webidl

# Generate both header and implementation stubs
node tools/webidl-codegen/src/cli.js -o output.h --impl output.c++ tools/webidl-codegen/examples/text-encoder.webidl

# Update existing file while preserving manual sections
node tools/webidl-codegen/src/cli.js -o output.h --update example.webidl

# Incremental update: only add stubs for new methods
node tools/webidl-codegen/src/cli.js -o api.h --impl api.c++ --update --incremental api.webidl

# Custom namespace
node tools/webidl-codegen/src/cli.js -n workerd::api::streams -o streams.h --impl streams.c++ tools/webidl-codegen/examples/writable-stream.webidl

# Specify custom header include path for implementation
node tools/webidl-codegen/src/cli.js -o api.h --impl api.c++ --header "workerd/api/api.h" example.webidl

# Skip specific interfaces (e.g., Window that's defined elsewhere)
node tools/webidl-codegen/src/cli.js -o fetch.h --impl fetch.c++ --skip-interface Window --skip-interface WindowOrWorkerGlobalScope fetch.webidl
```

Or from within the tool directory:

```bash
cd tools/webidl-codegen

# Header only
node src/cli.js examples/text-encoder.webidl
node src/cli.js -o output.h examples/text-encoder.webidl

# Header and implementation stubs
node src/cli.js -o examples/simple.h --impl examples/simple.c++ examples/simple.webidl

# Update existing header preserving manual code
node src/cli.js -o output.h --update examples/text-encoder.webidl

# Skip interfaces that are defined elsewhere
node src/cli.js -o fetch.h --impl fetch.c++ --skip-interface Window examples/fetch.webidl
```

### Implementation Stubs

The `--impl` flag generates C++ implementation stub files (`.c++`) with placeholder implementations:

- **C++ constructors** - Always generated for jsg::Object instances
- **JavaScript constructors** - Static `constructor()` method (only if WebIDL declares constructor)
- **Method stubs** - Include TODO comments and type-appropriate return values
- **Attribute getters/setters** - Placeholder implementations with TODO comments
- **Dictionary custom methods** - Comment templates for `[JsgCode]` declarations

The generated stubs are **meant to be edited** - they provide a starting point for implementing the actual logic.

**Example:**

```bash
node src/cli.js -o calculator.h --impl calculator.c++ examples/simple.webidl
```

Generates:
- `calculator.h` - Interface declarations (read-only, regenerate as needed)
- `calculator.c++` - Implementation stubs (edit with your logic)

The implementation file includes helpful comments like:
```cpp
// C++ constructor - add parameters as needed for your implementation
Calculator::Calculator() {
  // TODO: Initialize member variables
}

// JavaScript constructor (static method for JS 'new' operator)
jsg::Ref<Calculator> Calculator::constructor(jsg::Lock& js) {
  // TODO: Implement JavaScript constructor
  // Create and return a new instance using js.alloc
  // The C++ constructor will be called automatically
  return js.alloc<Calculator>();
}

int32_t Calculator::add(jsg::Lock& js, int32_t a, int32_t b) {
  // TODO: Implement add
  // Return the result
  return 0;
}
```

### Running Tests

```bash
cd tools/webidl-codegen
node src/test.js
```

## Workflow

### Recommended Development Workflow

The generator supports flexible workflows for ongoing development:

#### Option 1: Update Mode with Protected Regions (Recommended)

Best for iterative development where you modify both headers and implementations:

1. **Initial generation** with protected regions:
   ```bash
   node src/cli.js -o api.h --impl api.c++ api.webidl
   ```

2. **Implement your logic** in both files:
   - Edit `api.c++` method implementations (inside `BEGIN/END MANUAL SECTION` markers)
   - Add custom fields/methods in `api.h` (inside protected regions)

3. **Add new methods** to WebIDL as you develop

4. **Regenerate with --update** to get new stubs while preserving work:
   ```bash
   node src/cli.js -o api.h --impl api.c++ --update api.webidl
   ```
   - Existing implementations preserved via protected regions
   - New methods get fresh stubs
   - Custom code in headers preserved

5. **Incremental mode** for cleaner diffs (optional):
   ```bash
   node src/cli.js -o api.h --impl api.c++ --update --incremental api.webidl
   ```
   - Only outputs stubs for brand new methods
   - Existing methods left untouched (cleaner git diffs)

#### Option 2: Header-Only Regeneration

Simpler workflow if you don't need to modify headers after initial generation:

1. **Generate once** with implementation stubs:
   ```bash
   node src/cli.js -o api.h --impl api.c++ api.webidl
   ```

2. **Edit implementation** file (`api.c++`) with your logic

3. **Regenerate header only** when WebIDL changes:
   ```bash
   node src/cli.js -o api.h api.webidl
   ```
   Your implementation file stays untouched!

4. **Manually add new implementations** when methods are added

### Protected Regions

Both header and implementation files support protected regions for preserving manual code:

**Header Protected Regions** (`api.h`):
```cpp
class MyAPI: public jsg::Object {
 public:
  MyAPI();
  void generatedMethod(jsg::Lock& js);

  // BEGIN MANUAL SECTION: MyAPI::public
  // Add custom public methods, fields, or nested types here
  kj::String customHelper(int value);
  struct CustomNested { int x; };
  // END MANUAL SECTION: MyAPI::public

 private:
  // BEGIN MANUAL SECTION: MyAPI::private
  // Add private member variables here
  kj::String state;
  int counter = 0;
  // END MANUAL SECTION: MyAPI::private
};
```

**Implementation Protected Regions** (`api.c++`):
```cpp
// BEGIN MANUAL SECTION: MyAPI::generatedMethod
void MyAPI::generatedMethod(jsg::Lock& js) {
  // Your implementation here - preserved during regeneration
  counter++;
  state = "active"_kj;
}
// END MANUAL SECTION: MyAPI::generatedMethod
```

Each method gets its own protected region: `ClassName::methodName`

### Update Modes Comparison

| Mode | Command | Behavior |
|------|---------|----------|
| **Full regeneration** | `--update` | Regenerates all method stubs, preserves manual code in protected regions |
| **Incremental** | `--update --incremental` | Only generates stubs for new methods, existing methods unchanged |
| **No update** | (default) | Complete regeneration, overwrites everything |

### Development Workflow with Implementation Stubs

1. **Write WebIDL definitions** (`api.webidl`)
   ```webidl
   [Exposed=*]
   interface MyAPI {
     constructor();
     DOMString process(DOMString input);
   };
   ```

2. **Generate header and implementation stubs**
   ```bash
   node src/cli.js -o api.h --impl api.c++ api.webidl
   ```

3. **Implement the logic** in `api.c++`
   ```cpp
   // Replace TODO comments with actual implementation
   kj::String MyAPI::process(jsg::Lock& js, kj::String input) {
     // TODO: Implement process  ← Replace this
     return kj::str("Processed: ", input);  // ← With this
   }
   ```

4. **Regenerate only the header** when WebIDL changes
   ```bash
   node src/cli.js -o api.h api.webidl
   ```
   Your implementation file (`api.c++`) is preserved!

5. **Add new methods/stubs** as needed
   - Update WebIDL
   - Regenerate header only: `node src/cli.js -o api.h api.webidl`
   - Manually add new method implementations to `api.c++`
   - Or regenerate stubs to a temp file and copy new methods

### Header-Only Regeneration

Once you've implemented your stubs, you can safely regenerate the header without `--impl`:

```bash
# Only regenerates api.h, leaves api.c++ untouched
node src/cli.js -o api.h api.webidl
```

This allows iterative development:
- WebIDL changes → regenerate header
- Implementation stays intact in separate `.c++` file

### Update Mode with Protected Regions

For iterative development where you need to customize generated interfaces (jsg::Object classes), use **protected regions**:

1. **Initial generation** creates protected region markers:
   ```bash
   node src/cli.js -o api.h api.webidl
   ```

2. **Add custom code** between the markers in `api.h`:
   ```cpp
   class MyAPI: public jsg::Object {
   public:
     MyAPI();
     static jsg::Ref<MyAPI> constructor(jsg::Lock& js);
     void processData(jsg::Lock& js, kj::String data);

     // BEGIN MANUAL SECTION: MyAPI::public
     // Add custom public methods, fields, or nested types here
     kj::String customMethod(int value);  // Your custom method
     struct CustomNested { int x; };      // Your nested type
     // END MANUAL SECTION: MyAPI::public

   private:
     // BEGIN MANUAL SECTION: MyAPI::private
     // Add private member variables here
     kj::String state;                    // Your private field
     int counter = 0;
     // END MANUAL SECTION: MyAPI::private

   public:
     JSG_RESOURCE_TYPE(MyAPI) { ... }
   };
   ```

3. **Regenerate with --update** when WebIDL changes:
   ```bash
   node src/cli.js -o api.h --update api.webidl
   ```

   The generator preserves your custom code between markers while updating the generated parts!

**Protected Regions:**
- `ClassName::public` - Custom public methods, fields, nested types
- `ClassName::private` - Private member variables

**When to use update mode:**
- Adding custom helper methods to interfaces
- Adding private member variables for state
- Adding nested types or enums within the class
- Need to modify generated classes but keep them regeneratable

## Features

### Code Generation Strategies

The generator supports multiple strategies for combining generated and manual code:

#### Strategy 1: Split Generation (Recommended)

Generated code is split into two files for clean separation:

**`foo-generated.h`** - Fully generated, never manually edited:
```cpp
// GENERATED CODE - DO NOT EDIT
// Auto-generated from foo.webidl

namespace workerd::api {
struct FooOptions {
  jsg::Optional<kj::String> mode;
  JSG_STRUCT(mode);
};
}
```

**`foo.h`** - Manual implementations:
```cpp
#include "foo-generated.h"

namespace workerd::api {
// Add validate(), constructors, JSG_MEMORY_INFO, etc.
inline void FooOptions::validate(jsg::Lock& js) { /* ... */ }
}
```

#### Strategy 2: Extension Pattern with `[ManualExtensions]`

Mark types needing manual code with `[ManualExtensions]`:

```webidl
[ManualExtensions]
dictionary FooOptions {
  DOMString mode;
};
```

Generates into `generated::` namespace for extension:
```cpp
namespace workerd::api::generated {
  struct FooOptionsGenerated { /* ... */ };
}

// In separate file:
namespace workerd::api {
  struct FooOptions: public generated::FooOptionsGenerated {
    // Manual additions: validate(), JSG_MEMORY_INFO,
    // JSG_STRUCT_TS_OVERRIDE_DYNAMIC, etc.
  };
}
```

#### Strategy 3: Protected Regions (Alternative)

Use markers to protect manual code:
```cpp
// BEGIN MANUAL SECTION
void validate(jsg::Lock& js) { /* preserved */ }
// END MANUAL SECTION
```

**Recommendation**: Use Strategy 1 (Split Generation) for simplicity and Strategy 2 (Extension Pattern) when you need C++ inheritance.

### Supported WebIDL Constructs

- ✅ Interfaces with operations and attributes
- ✅ Dictionaries (structs)
- ✅ Enums
- ✅ Callbacks (function types)
- ✅ Constructors
- ✅ Optional parameters
- ✅ Nullable types (`T?`)
- ✅ Promise types
- ✅ Sequence types
- ✅ Record types
- ✅ Union types
- ✅ Default values
- ✅ Interface inheritance
- ✅ Dictionary inheritance (flattened to avoid multiple inheritance)
- ✅ Compatibility flags (extended attributes)

### Compatibility Flags

Use the `[JsgCompatFlag=FlagName]` extended attribute to make methods or properties conditional on runtime feature flags:

```webidl
[Exposed=*]
interface ExperimentalAPI {
  // Always available
  undefined basicMethod();

  // Only available when WorkerdExperimental flag is enabled
  [JsgCompatFlag=WorkerdExperimental]
  undefined experimentalMethod();

  // Only available when ReplicaRouting flag is enabled
  [JsgCompatFlag=ReplicaRouting]
  Promise<undefined> enableReplicas();
};
```

Generates:

```cpp
JSG_RESOURCE_TYPE(ExperimentalAPI, CompatibilityFlags::Reader flags) {
  JSG_METHOD(basicMethod);

  if (flags.getWorkerdExperimental()) {
    JSG_METHOD(experimentalMethod);
  }

  if (flags.getReplicaRouting()) {
    JSG_METHOD(enableReplicas);
  }
}
```

### Custom Method Names

Use the `[JsgMethodName=name]` extended attribute to specify a custom C++ method name. This is useful for:
- Avoiding C++ reserved keywords (e.g., `delete`)
- Following C++ naming conventions
- Differentiating method overloads

```webidl
[Exposed=*]
interface Storage {
  // Maps JS 'delete' to C++ 'delete_' to avoid keyword conflict
  [JsgMethodName=delete_]
  undefined delete(DOMString key);

  // Method overloads with custom names
  DOMString processData(DOMString input);

  [JsgCompatFlag=WorkerdExperimental, JsgMethodName=processDataWithFormat]
  DOMString processData(DOMString input, optional DOMString format);
};
```

Generates:

```cpp
class Storage: public jsg::Object {
public:
  void delete_(jsg::Lock& js, kj::String key);
  kj::String processData(jsg::Lock& js, kj::String input);
  kj::String processDataWithFormat(jsg::Lock& js, kj::String input, jsg::Optional<kj::String> format);

  JSG_RESOURCE_TYPE(Storage, CompatibilityFlags::Reader flags) {
    JSG_METHOD_NAMED(delete, delete_);
    JSG_METHOD(processData);

    if (flags.getWorkerdExperimental()) {
      JSG_METHOD_NAMED(processData, processDataWithFormat);
    }
  }
};
```

### Mutually Exclusive Signatures

Use `[JsgCompatFlagOff=FlagName]` to define a method signature that should be used when a flag is **disabled**. This creates an `if/else` pattern for mutually exclusive implementations:

```webidl
[Exposed=*]
interface Storage {
  // Old signature (when NewApiSignature flag is OFF)
  [JsgCompatFlagOff=NewApiSignature, JsgMethodName=getOld]
  Promise<DOMString> get(DOMString key);

  // New signature (when NewApiSignature flag is ON)
  [JsgCompatFlag=NewApiSignature, JsgMethodName=getNew]
  Promise<any> get(DOMString key, optional boolean parseJson);
};
```

Generates:

```cpp
class Storage: public jsg::Object {
public:
  jsg::Promise<kj::String> getOld(jsg::Lock& js, kj::String key);
  jsg::Promise<jsg::JsValue> getNew(jsg::Lock& js, kj::String key, jsg::Optional<bool> parseJson);

  JSG_RESOURCE_TYPE(Storage, CompatibilityFlags::Reader flags) {
    if (flags.getNewApiSignature()) {
      JSG_METHOD_NAMED(get, getNew);
    } else {
      JSG_METHOD_NAMED(get, getOld);
    }
  }
};
```

### TypeScript Overrides

Use `[JsgTsOverride="..."]` to provide custom TypeScript type definitions via `JSG_TS_OVERRIDE`, and `[JsgTsDefine="..."]` for type aliases via `JSG_TS_DEFINE`:

```webidl
[Exposed=*,
 JsgTsDefine="type DataFormat = 'json' | 'text' | 'binary';",
 JsgTsOverride="{
  processData(input: string): string;
  processData(input: string, format: DataFormat): string;
}"]
interface DataProcessor {
  DOMString processData(DOMString input);

  [JsgCompatFlag=NewFormat, JsgMethodName=processDataWithFormat]
  DOMString processData(DOMString input, optional DOMString format);
};

[JsgTsOverride="{
  mode?: 'standard' | 'experimental';
  options?: Record<string, any>;
}"]
dictionary ProcessorOptions {
  DOMString mode = "standard";
  any options;
};
```

Generates:

```cpp
class DataProcessor: public jsg::Object {
public:
  kj::String processData(jsg::Lock& js, kj::String input);
  kj::String processDataWithFormat(jsg::Lock& js, kj::String input, jsg::Optional<kj::String> format);

  JSG_RESOURCE_TYPE(DataProcessor, CompatibilityFlags::Reader flags) {
    JSG_METHOD(processData);

    if (flags.getNewFormat()) {
      JSG_METHOD_NAMED(processData, processDataWithFormat);
    }

    JSG_TS_DEFINE(type DataFormat = 'json' | 'text' | 'binary';);

    JSG_TS_OVERRIDE({
      processData(input: string): string;
      processData(input: string, format: DataFormat): string;
    });
  }
};

struct ProcessorOptions {
  jsg::Optional<kj::String> mode;  // default: "standard"
  jsg::Optional<jsg::JsValue> options;
  JSG_STRUCT(mode, options);
  JSG_TS_OVERRIDE({
    mode?: 'standard' | 'experimental';
    options?: Record<string, any>;
  });
};
```

### Generated Code

The tool generates:

1. **Classes** for interfaces with method declarations
2. **JSG_RESOURCE_TYPE blocks** with proper registration macros
3. **Structs with JSG_STRUCT blocks** for dictionaries
4. **Type aliases** for callbacks
5. **Conditional registration** based on compatibility flags

### Type Mapping

#### Basic Types

| WebIDL Type | JSG C++ Type |
|-------------|--------------|
| `boolean` | `bool` |
| `long` | `int32_t` |
| `DOMString` | `kj::String` |
| `any` | `jsg::JsValue` |
| `Promise<T>` | `jsg::Promise<T>` |
| `sequence<T>` | `kj::Array<T>` |
| `record<K, V>` | `jsg::Dict<K, V>` |
| Interface | `jsg::Ref<Interface>` |
| Dictionary | `DictName` (direct) |

#### Optional and Nullable

| WebIDL | C++ (Parameters) | C++ (Dictionary Members) |
|--------|------------------|--------------------------|
| `T` | `T` | `T` (required) |
| `optional T` | `jsg::Optional<T>` | `jsg::Optional<T>` |
| `T?` | `kj::Maybe<T>` | `kj::Maybe<T>` |
| `optional T?` | `jsg::Optional<kj::Maybe<T>>` | `jsg::Optional<kj::Maybe<T>>` |

#### Collection Types

**Sequences:**
```webidl
sequence<DOMString> items;        // → kj::Array<kj::String>
sequence<long> numbers;           // → kj::Array<int32_t>
sequence<MyInterface> objects;    // → kj::Array<jsg::Ref<MyInterface>>
```

**Records (key-value maps):**
```webidl
record<DOMString, long> stringToNumber;              // → jsg::Dict<kj::String, int32_t>
record<DOMString, DOMString> headers;                // → jsg::Dict<kj::String, kj::String>
record<DOMString, sequence<DOMString>> multiMap;     // → jsg::Dict<kj::String, kj::Array<kj::String>>
record<DOMString, any> metadata;                     // → jsg::Dict<kj::String, jsg::JsValue>
```

`jsg::Dict<K, V>` is used for WebIDL `record<K, V>` types, representing JavaScript objects used as dictionaries/maps.

## Example

### Complete Example with Implementation Stubs

Input WebIDL (`calculator.webidl`):

```webidl
[Exposed=*]
interface Calculator {
  constructor();

  long add(long a, long b);
  long subtract(long a, long b);

  readonly attribute DOMString version;
};

dictionary Point {
  long x;
  long y;

  JSG_STRUCT(x, y);
};
```

Generate files:
```bash
node src/cli.js -o calculator.h --impl calculator.c++ calculator.webidl
```

**Generated Header** (`calculator.h` - regenerate as needed):

```cpp
#pragma once
// Generated from WebIDL - DO NOT EDIT

#include <workerd/jsg/jsg.h>
#include <kj/string.h>
#include <kj/array.h>

namespace workerd {
namespace api {

class Calculator: public jsg::Object {
public:
  static jsg::Ref<Calculator> constructor(jsg::Lock& js);

  int32_t add(jsg::Lock& js, int32_t a, int32_t b);
  int32_t subtract(jsg::Lock& js, int32_t a, int32_t b);
  kj::String getVersion(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Calculator) {
    JSG_READONLY_PROTOTYPE_PROPERTY(version, getVersion);
    JSG_METHOD(add);
    JSG_METHOD(subtract);
  }
};

struct Point {
  int32_t x;
  int32_t y;

  JSG_STRUCT(x, y);
};

}  // namespace api
}  // namespace workerd
```

**Generated Implementation Stubs** (`calculator.c++` - edit with your logic):

```cpp
// Generated implementation stubs - EDIT THIS FILE
// This file contains placeholder implementations.
// Replace TODO comments with actual logic.

#include "calculator.h"

namespace workerd {
namespace api {

// C++ constructor - add parameters as needed for your implementation
Calculator::Calculator() {
  // TODO: Initialize member variables
}

// JavaScript constructor (static method for JS 'new' operator)
jsg::Ref<Calculator> Calculator::constructor(jsg::Lock& js) {
  // TODO: Implement JavaScript constructor
  // Create and return a new instance using js.alloc
  // The C++ constructor will be called automatically
  return js.alloc<Calculator>();
}

int32_t Calculator::add(jsg::Lock& js, int32_t a, int32_t b) {
  // TODO: Implement add
  // Return the result
  return 0;  // ← Replace with: return a + b;
}

int32_t Calculator::subtract(jsg::Lock& js, int32_t a, int32_t b) {
  // TODO: Implement subtract
  // Return the result
  return 0;  // ← Replace with: return a - b;
}

kj::String Calculator::getVersion(jsg::Lock& js) {
  // TODO: Implement getter for version
  return kj::str("TODO");  // ← Replace with: return kj::str("1.0");
}

}  // namespace api
}  // namespace workerd
```

### Simple Example (Header Only)

Input WebIDL:

```webidl
[Exposed=*]
interface TextEncoder {
  constructor();

  readonly attribute DOMString encoding;

  Uint8Array encode(optional DOMString input = "");
};
```

Generated C++:

```cpp
#pragma once
// Generated from WebIDL - DO NOT EDIT

#include <workerd/jsg/jsg.h>
#include <kj/string.h>
#include <kj/array.h>

namespace workerd {
namespace api {

class TextEncoder: public jsg::Object {
public:
  static jsg::Ref<TextEncoder> constructor();

  virtual jsg::BufferSource encode(jsg::Lock& js, jsg::Optional<kj::String> input) = 0;
  virtual kj::String getEncoding() = 0;

  JSG_RESOURCE_TYPE(TextEncoder) {
    JSG_READONLY_PROTOTYPE_PROPERTY(encoding, getEncoding);
    JSG_METHOD(encode);
  }
};

}  // namespace api
}  // namespace workerd
```

### Implementation Features

The implementation generator (`--impl` flag) creates starter code with protected regions:

#### Protected Region Markers

Every method implementation is wrapped in protected regions to enable safe regeneration:

```cpp
// BEGIN MANUAL SECTION: Calculator::add
int32_t Calculator::add(jsg::Lock& js, int32_t a, int32_t b) {
  // TODO: Implement add
  return a + b;  // Your implementation - preserved on regeneration!
}
// END MANUAL SECTION: Calculator::add
```

Region names follow the pattern: `ClassName::methodName`

Special regions:
- `ClassName::constructor` - C++ constructor
- `ClassName::constructor(js)` - JavaScript constructor
- `ClassName::getPropertyName` - Attribute getters
- `ClassName::setPropertyName` - Attribute setters

#### Type-Specific Return Values

| Return Type | Generated Stub | Notes |
|-------------|----------------|-------|
| `kj::String`, `jsg::DOMString` | `return "TODO"_kj;` | Uses `kj::StringPtr` for efficiency |
| `void` | No return statement | - |
| `jsg::Promise<T>` | `return js.resolvedPromise();` | Includes helpful comment |
| `jsg::Ref<T>` | `return js.alloc<T>();` | Allocates new instance |
| `bool` | `return false;` | - |
| Numeric types | `return 0;` | - |
| `jsg::Optional<T>` | `return kj::none;` | - |
| `kj::Maybe<T>` | `return kj::none;` | - |
| Dictionary types | Returns by value | Not wrapped in `jsg::Ref<>` |

**String Return Optimization**: Generated stubs use `"TODO"_kj` which returns `kj::StringPtr`. The stub includes a comment suggesting you can change the return type in the header to `kj::StringPtr` when returning string literals or fixed strings for better performance.

#### Constructor Separation

The generator creates two types of constructors:

**C++ Constructor** (always generated):
```cpp
// BEGIN MANUAL SECTION: MyAPI::constructor
MyAPI::MyAPI() {
  // Initialize member variables
  counter = 0;
}
// END MANUAL SECTION: MyAPI::constructor
```

**JavaScript Constructor** (only if WebIDL has `constructor()`):
```cpp
// BEGIN MANUAL SECTION: MyAPI::constructor(js)
jsg::Ref<MyAPI> MyAPI::constructor(jsg::Lock& js) {
  return js.alloc<MyAPI>();
}
// END MANUAL SECTION: MyAPI::constructor(js)
```

#### Dictionary vs Interface Types

The type mapper correctly distinguishes between:
- **Dictionaries (JSG_STRUCT)**: Referenced directly as `ConfigOptions`
- **Interfaces (jsg::Object)**: Wrapped in `jsg::Ref<MyAPI>`

Example:
```cpp
// Dictionary passed by value
void processConfig(jsg::Lock& js, ConfigOptions config);

// Interface passed by reference
void processAPI(jsg::Lock& js, jsg::Ref<MyAPI> api);
```

#### Dictionary Inheritance Flattening

When a dictionary extends another dictionary in WebIDL, the generator **flattens the inheritance** into a single C++ struct containing all fields from the entire hierarchy. This avoids multiple inheritance issues and simplifies the type system.

**WebIDL:**
```webidl
dictionary BaseOptions {
  DOMString mode = "read";
  boolean excludeAll = false;
};

dictionary FilePickerOptions : BaseOptions {
  DOMString id;
  boolean multiple = false;
};

dictionary OpenFilePickerOptions : FilePickerOptions {
  boolean allowMultipleFiles = true;
};
```

**Generated C++:**
```cpp
struct BaseOptions {
  jsg::Optional<jsg::DOMString> mode;
  jsg::Optional<bool> excludeAll;
  JSG_STRUCT(mode, excludeAll);
};

struct FilePickerOptions {
  // Inherited from BaseOptions
  jsg::Optional<jsg::DOMString> mode;
  jsg::Optional<bool> excludeAll;
  // Own fields
  jsg::Optional<jsg::DOMString> id;
  jsg::Optional<bool> multiple;

  JSG_STRUCT(mode, excludeAll, id, multiple);
  // TypeScript sees the inheritance relationship
  JSG_STRUCT_TS_OVERRIDE(FilePickerOptions extends BaseOptions);
};

struct OpenFilePickerOptions {
  // All inherited fields from BaseOptions and FilePickerOptions
  jsg::Optional<jsg::DOMString> mode;
  jsg::Optional<bool> excludeAll;
  jsg::Optional<jsg::DOMString> id;
  jsg::Optional<bool> multiple;
  // Own field
  jsg::Optional<bool> allowMultipleFiles;

  JSG_STRUCT(mode, excludeAll, id, multiple, allowMultipleFiles);
  JSG_STRUCT_TS_OVERRIDE(OpenFilePickerOptions extends FilePickerOptions);
};
```

**Key Points:**
- All parent fields are duplicated in child structs
- No C++ inheritance (`struct Child : public Parent`) is used
- `JSG_STRUCT` lists all fields including inherited ones
- `JSG_STRUCT_TS_OVERRIDE` preserves TypeScript inheritance semantics
- Field initialization order respects inheritance hierarchy (parent fields first)

### Helpful Comments

Each method includes context-aware comments:

```cpp
jsg::Promise<Data> fetch(jsg::Lock& js, kj::String url) {
  // TODO: Implement fetch
  // Return a promise that resolves with the result
  // Example: return js.resolvedPromise(...);
  return js.resolvedPromise();
}
```

### Dictionary Custom Methods

For dictionaries with `[JsgCode]`:

```cpp
// Circle custom method implementations
// Add implementations for methods declared in [JsgCode]

// Example:
// void Circle::validate(jsg::Lock& js) {
//   // Validate the struct members
//   JSG_REQUIRE(radius > 0, TypeError, "radius must be positive");
// }
```

## Mixins (Interface Mixins)

WebIDL interface mixins allow sharing attributes, operations, and constants across multiple interfaces using `includes` statements.

### Implementation: C++ Inheritance Approach

Following the pattern established in `workerd/api/http.h`, the generator uses **C++ inheritance** to implement mixins:

```webidl
// WebIDL
interface mixin Body {
  readonly attribute ReadableStream? body;
  readonly attribute boolean bodyUsed;
  Promise<ArrayBuffer> arrayBuffer();
  Promise<Blob> blob();
  Promise<FormData> formData();
  Promise<any> json();
  Promise<USVString> text();
};

interface Request {
  constructor(RequestInfo input, optional RequestInit init);
  readonly attribute USVString url;
  readonly attribute ByteString method;
  readonly attribute Headers headers;
};
Request includes Body;

interface Response {
  constructor(optional BodyInit? body = null, optional ResponseInit init = {});
  readonly attribute unsigned short status;
  readonly attribute ByteString statusText;
  readonly attribute Headers headers;
};
Response includes Body;
```

```cpp
// Generated C++
// Mixin: Body - No base class, no JSG_RESOURCE_TYPE
class Body {
public:
  jsg::Promise<jsg::BufferSource> arrayBuffer(jsg::Lock& js);
  jsg::Promise<jsg::Ref<Blob>> blob(jsg::Lock& js);
  jsg::Promise<jsg::Ref<FormData>> formData(jsg::Lock& js);
  jsg::Promise<jsg::JsValue> json(jsg::Lock& js);
  jsg::Promise<kj::String> text(jsg::Lock& js);
  kj::Maybe<jsg::Ref<ReadableStream>> getBody(jsg::Lock& js);
  bool getBodyUsed(jsg::Lock& js);
};

// Interface: Request - inherits from jsg::Object and Body mixin
class Request: public jsg::Object, public Body {
public:
  jsg::ByteString getMethod(jsg::Lock& js);
  jsg::USVString getUrl(jsg::Lock& js);
  jsg::Ref<Headers> getHeaders(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Request) {
    JSG_READONLY_PROTOTYPE_PROPERTY(method, getMethod);
    JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);
    JSG_READONLY_PROTOTYPE_PROPERTY(headers, getHeaders);
    // Body mixin members registered here
    JSG_READONLY_PROTOTYPE_PROPERTY(body, getBody);
    JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);
    JSG_METHOD(arrayBuffer);
    JSG_METHOD(blob);
    JSG_METHOD(formData);
    JSG_METHOD(json);
    JSG_METHOD(text);
  }
};

// Interface: Response - also inherits from jsg::Object and Body mixin
class Response: public jsg::Object, public Body {
public:
  uint16_t getStatus(jsg::Lock& js);
  jsg::ByteString getStatusText(jsg::Lock& js);
  jsg::Ref<Headers> getHeaders(jsg::Lock& js);

  JSG_RESOURCE_TYPE(Response) {
    JSG_READONLY_PROTOTYPE_PROPERTY(status, getStatus);
    JSG_READONLY_PROTOTYPE_PROPERTY(statusText, getStatusText);
    JSG_READONLY_PROTOTYPE_PROPERTY(headers, getHeaders);
    // Body mixin members registered here
    JSG_READONLY_PROTOTYPE_PROPERTY(body, getBody);
    JSG_READONLY_PROTOTYPE_PROPERTY(bodyUsed, getBodyUsed);
    JSG_METHOD(arrayBuffer);
    JSG_METHOD(blob);
    JSG_METHOD(formData);
    JSG_METHOD(json);
    JSG_METHOD(text);
  }
};
```

### Benefits

- **No member duplication**: Body methods defined once, inherited by Request and Response
- **Type safety**: C++ inheritance enforces the relationship at compile time
- **JSG compatibility**: JSG handles C++ inheritance naturally
- **Semantic clarity**: The inheritance clearly shows Request/Response include Body
- **Easier maintenance**: Changes to Body automatically apply to all including interfaces
- **No diamond inheritance**: Mixins don't inherit from `jsg::Object`, only interfaces do

### Key Design Decisions

1. **Mixins are plain C++ classes** (no `jsg::Object` base, no `JSG_RESOURCE_TYPE`)
2. **Interfaces inherit from `jsg::Object` first, then mixins** (e.g., `class Request: public jsg::Object, public Body`)
3. **Mixin members are registered in the interface's `JSG_RESOURCE_TYPE`** (not in the mixin itself)
4. **C++ keyword escaping**: Methods like `delete` become `delete_` with `JSG_METHOD_NAMED(delete, delete_)`
5. **Forward declarations**: All interface types (including local ones) are forward declared to handle typedef ordering issues

### Implementation Details

### Implementation Details

1. **Parse mixin definitions**: `interface mixin` declarations are stored separately from interfaces
2. **Track includes statements**: Build mapping of interface → mixins (e.g., `Request includes Body`)
3. **Generate mixin classes**: Create plain C++ classes for each `interface mixin` (no base class)
4. **Apply inheritance**: Generate interfaces with `: public jsg::Object, public MixinName` for each mixin
5. **Handle multiple mixins**: Use multiple inheritance when interface includes multiple mixins
6. **Register mixin members**: Include all mixin methods/properties in the interface's `JSG_RESOURCE_TYPE`
7. **Ordering**: `jsg::Object` is always first in the inheritance list to avoid diamond inheritance

### Handling Multiple Mixins

When an interface includes multiple mixins:

```webidl
interface mixin MixinA { void methodA(); };
interface mixin MixinB { void methodB(); };
interface Combined {};
Combined includes MixinA;
Combined includes MixinB;
```

Generate with multiple inheritance (jsg::Object first):

```cpp
// Mixin A - no base class
class MixinA {
public:
  void methodA(jsg::Lock& js);
};

// Mixin B - no base class
class MixinB {
public:
  void methodB(jsg::Lock& js);
};

// Combined - jsg::Object first, then mixins
class Combined: public jsg::Object, public MixinA, public MixinB {
public:
  JSG_RESOURCE_TYPE(Combined) {
    // Register methods from both mixins
    JSG_METHOD(methodA);
    JSG_METHOD(methodB);
  }
};
```

### Edge Cases

- **Partial mixins**: Merge partial mixin definitions before generating the mixin class
- **Name conflicts**: WebIDL disallows member name conflicts; generator validates
- **Cross-file mixins**: Support mixins and interfaces defined in separate WebIDL fragments (not yet implemented)
- **Mixin inheritance**: Interface mixins cannot inherit from other mixins (WebIDL constraint)

## Type System and Forward Declarations

### Namespaces

WebIDL namespaces are static collections of operations and attributes (no constructors or inheritance).

**WebIDL:**
```webidl
namespace Console {
  undefined log(any... data);
  undefined warn(DOMString message);
  readonly attribute DOMString version;
};
```

**C++ Mapping:**

Namespaces map to regular `jsg::Object` classes with static methods:

```cpp
class Console : public jsg::Object {
public:
  static void log(jsg::Lock& js, jsg::Varargs args);
  static void warn(jsg::Lock& js, kj::String message);
  static kj::StringPtr getVersion();

  JSG_RESOURCE_TYPE(Console) {
    JSG_STATIC_METHOD(log);
    JSG_STATIC_METHOD(warn);
    JSG_STATIC_READONLY_INSTANCE_PROPERTY(version, getVersion);
  }
};
```

**Status**: Not yet implemented. Implementation requirements:
- Parse `type: "namespace"` from webidl2
- Generate class extending `jsg::Object` (not a special namespace type)
- All operations → static methods
- All attributes → static getters
- Use `JSG_STATIC_METHOD` and `JSG_STATIC_*_PROPERTY` macros
- Note: Method destructuring safety will be handled separately in the future

### Partial Definitions

Partial interfaces/dictionaries/mixins allow splitting a definition across multiple files or specs. WebIDL uses `partial` keyword:

**WebIDL:**
```webidl
// In base spec
interface Window {
  readonly attribute DOMString location;
};

// In another spec (e.g., Fetch API)
partial interface Window {
  Promise<Response> fetch(RequestInfo input, optional RequestInit init);
};

// In yet another spec
partial interface Window {
  undefined alert(DOMString message);
};
```

**Strategy**: Merge all partial definitions before code generation:

1. **During parsing**: Collect all partial definitions separately
2. **Merge phase**: Combine all partials with the main definition
   - Main interface + all `partial interface X` → single merged interface X
   - Validate no member name conflicts
   - Preserve member order (main first, then partials in source order)
3. **Generate**: Treat merged result as single definition

**Example merge:**
```javascript
// After merging
interface Window {
  readonly attribute DOMString location;
  Promise<Response> fetch(RequestInfo input, optional RequestInit init);
  undefined alert(DOMString message);
}
```

**Partial Mixins/Dictionaries**: Same approach - merge before generation.

**Status**: Not yet implemented. Current behavior: parser rejects partials with error.

**Implementation Plan**:
1. Add `collectPartials()` method to parser that groups definitions by name
2. Add `mergePartials()` method that combines members
3. Validate no duplicate member names (WebIDL constraint)
4. Generate merged definitions normally

**Use Case**: Essential for multi-spec scenarios like:
- `partial interface mixin WindowOrWorkerGlobalScope` (added by Fetch, Streams, etc.)
- `partial interface Window` (extended by many Web APIs)
- Allows generator to work with isolated WebIDL fragments from different specs

### Type Mapping

The generator distinguishes between different WebIDL construct types and maps them appropriately to C++:

| WebIDL Type | C++ Mapping | Wrapped in jsg::Ref? | Forward Declared? |
|-------------|-------------|----------------------|-------------------|
| `interface` | Class inheriting from `jsg::Object` | Yes | Yes |
| `interface mixin` | Plain C++ class | No | No (just a base class) |
| `dictionary` | C++ struct with `JSG_STRUCT` | No | No |
| `enum` | C++ `enum class` | No | No |
| `typedef` | C++ `using` alias | Depends on target type | No |

### Forward Declarations

The generator automatically creates forward declarations for:
- **External interfaces** (e.g., `AbortSignal`, `Blob`) - defined elsewhere but referenced
- **Local interfaces** (e.g., `Request`, `Response`) - defined in the same file but may be used before definition (e.g., in typedefs)

Forward declarations are placed at the top of the file, after the namespace opening and before typedefs/enums/definitions.

**Example:**
```cpp
namespace workerd::api {

// Forward declarations for types defined elsewhere
class AbortSignal;
class Blob;
class Request;
class Response;

using RequestInfo = kj::OneOf<jsg::Ref<Request>, jsg::USVString>;

// ... actual class definitions follow
```

### External Type Handling

Some types are defined in other Web Platform specs and need special handling:

#### External Enums (Manual Configuration Required)

External enums (like `ReferrerPolicy` from the Referrer Policy spec) must be **manually registered** in the generator:

**In `src/type-mapper.js`:**
```javascript
this.externalEnums = new Set([
  'ReferrerPolicy',  // From Referrer Policy spec
  // Add other external enums here
]);
```

**In `src/generator.js`:**
```javascript
const externalEnums = new Set([
  'ReferrerPolicy',  // From Referrer Policy spec
  // Add other external enums here
]);
```

This ensures they:
1. Are **not** wrapped in `jsg::Ref<>` (used directly as `ReferrerPolicy`)
2. Are **not** forward declared as classes (they're enums, need to be included)

#### External Interfaces (Automatic)

External interfaces (like `AbortSignal`, `Blob`, `FormData`) are automatically:
- Forward declared at the top of the file
- Wrapped in `jsg::Ref<>` when used as types
- Skipped for code generation (defined elsewhere)

Default external interfaces in `skipInterfaces`:
- `Event`, `EventTarget`, `EventListener`
- `AbortSignal`, `AbortController`

**Limitation**: Currently, external enums must be manually added to the source code. Future enhancement could support a configuration file (e.g., `.webidl-codegen.json`) for project-specific external types.

## Limitations

Current limitations:

- **Partial interfaces/dictionaries/mixins**: Not yet supported - parser rejects them with error
  - Implementation plan documented above (merge partials before generation)
- **Namespaces**: Not yet supported - WebIDL `namespace` construct needs JSG registration strategy
- **Cross-file mixins**: Mixins and interfaces must be in the same WebIDL file
- **Iterables**: No support for `iterable<T>` or `async iterable<T>`
- **Maplike/setlike**: Not supported
- **Limited extended attributes**: Only a subset is handled
- **External enum configuration**: External enums (like `ReferrerPolicy`) must be manually added to source code - no config file support yet
- **No cross-reference validation**: Types referenced but not defined are forward declared but not validated

## Next Steps

Potential improvements:

1. **Configuration file support** for external types (enums, interfaces, dictionaries)
2. Add Bazel build integration
3. Implement validation for WebIDL constraints
4. Support more extended attributes
5. Add documentation comment generation
6. Support partial interfaces
7. Support cross-file mixins
8. Smart signature detection for return type optimization
9. Iterator/async iterator support

## Summary of Features

### Supported WebIDL Constructs
* ✅ Interfaces with constructors, methods, attributes
* ✅ **Interface mixins** with C++ inheritance (mixin classes, `includes` statements)
* ✅ Dictionaries (structs) with inheritance flattening
* ✅ Enums
* ✅ Typedefs (union types with `using` aliases)
* ✅ Callbacks
* ✅ Inheritance (interface hierarchy and dictionary flattening)
* ✅ Optional and nullable types
* ✅ Union types (`(A or B)`)
* ✅ Generic types (Promise, sequence, record<K,V>)
* ✅ Collection types (`sequence<T>` → `kj::Array<T>`, `record<K,V>` → `jsg::Dict<K,V>`)
* ✅ **C++ keyword escaping** (e.g., `delete` → `delete_` with `JSG_METHOD_NAMED`)
* ✅ **Forward declarations** for both external and local interface types

### Extended Attributes
* ✅ `[JsgCompatFlag=Name]` - Conditional compilation when flag is ON
* ✅ `[JsgCompatFlagOff=Name]` - Conditional compilation when flag is OFF
* ✅ `[JsgMethodName=name]` - Custom C++ method name for overloads
* ✅ `[JsgTsOverride="..."]` - TypeScript type override
* ✅ `[JsgTsDefine="..."]` - TypeScript type definitions
* ✅ `[JsgTsRoot]` - Mark as TypeScript root type
* ✅ `[JsgPropertyScope=instance|prototype]` - Property registration scope
* ✅ `[JsgInternal]` - Exclude field from JSG_STRUCT
* ✅ `[JsgCode="..."]` - Custom C++ method/constructor declarations

### Code Generation
* ✅ Header generation (`.h`)
* ✅ Implementation stub generation (`.c++`)
* ✅ **Protected regions** for preserving manual code during regeneration
* ✅ **Incremental mode** to only generate stubs for new methods
* ✅ Update mode (`--update`) to regenerate while keeping custom modifications
* ✅ Proper JSG_STRUCT field ordering (exposed → JSG_STRUCT → internal)
* ✅ SelfRef, Unimplemented, WontImplement auto-detection
* ✅ Smart return value generation in stubs
* ✅ Helpful TODO comments in stubs
* ✅ Separate C++ and JavaScript constructor handling
* ✅ Dictionary vs interface type differentiation
* ✅ String return type optimization (`kj::StringPtr` for literals)  ### Type Mappings
* ✅ All WebIDL primitive types
* ✅ Optional/nullable variants
* ✅ JSG-specific types (SelfRef, Unimplemented, WontImplement)
* ✅ Union types via kj::OneOf
* ✅ Promises via jsg::Promise
* ✅ Sequences via kj::Array
* ✅ Records via jsg::Dict
* ✅ Interface references via jsg::Ref (dictionaries, enums, typedefs used directly)
* ✅ **External enum handling** (manual configuration for types like `ReferrerPolicy`)

## Next Steps

1. **Configuration file support** for external types (enums, interfaces, dictionaries)
2. Add Bazel build integration
3. Implement validation for WebIDL constraints
4. Support more extended attributes
5. Add documentation comment generation
6. Support partial interfaces
7. Support cross-file mixins
8. Smart stub updates (preserve existing implementations)
9. Incremental implementation regeneration
10. Iterator/async iterator support

