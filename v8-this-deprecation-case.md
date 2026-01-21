# Case for PropertyCallbackInfo::This() (or Equivalent)

## Summary

Cloudflare Workers needs `PropertyCallbackInfo::This()` (or an equivalent API) to support Workers deployed before 2022-01-31 that use instance-level properties. Without this, we cannot maintain backwards compatibility for these Workers.

## Background: How Workerd Handles Properties

Workerd has two patterns for exposing properties:

**1. Prototype Properties (modern, post-2022-01-31)**
```cpp
// Uses SetAccessorProperty with FunctionTemplate
auto getterFn = v8::FunctionTemplate::New(isolate, Gcb::callback);
prototype->SetAccessorProperty(v8Name, getterFn, ...);

// Callback uses FunctionCallbackInfo::This() - NOT deprecated
void callback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto obj = info.This();  // OK - not deprecated
}
```

**2. Instance Properties (legacy, pre-2022-01-31)**
```cpp
// Uses SetNativeDataProperty on instance template
instance->SetNativeDataProperty(v8Name, &Gcb::callback, ...);

// Callback uses PropertyCallbackInfo::This() - DEPRECATED
void callback(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto obj = info.This();  // Deprecated!
}
```

## The Compatibility Flag

In January 2022, we migrated properties from instance to prototype:

```capnp
jsgPropertyOnPrototypeTemplate @7 :Bool
    $compatEnableFlag("workers_api_getters_setters_on_prototype")
    $compatEnableDate("2022-01-31")
```

**Workers with compatibility date >= 2022-01-31**: Use prototype properties, `FunctionCallbackInfo::This()` - no problem.

**Workers with compatibility date < 2022-01-31**: Use instance properties, `PropertyCallbackInfo::This()` - affected by deprecation.

## Why We Can't Just Migrate Old Workers

The change from instance to prototype properties is **not purely internal**. It has observable behavioral differences:

### 1. Subclassing Behavior
```javascript
class MyRequest extends Request {
  get headers() {
    return "overridden";
  }
}
const r = new MyRequest("https://example.com");
r.headers;  // Instance: "overridden" (own property shadows)
            // Prototype: "overridden" (prototype chain)
```
This one happens to work the same, but...

### 2. Property Enumeration
```javascript
const r = new Request("https://example.com");
Object.keys(r);           // Instance: ["headers", "url", ...]
                          // Prototype: []
Object.getOwnPropertyNames(r);  // Instance: ["headers", "url", ...]
                                // Prototype: []
```

### 3. hasOwnProperty
```javascript
const r = new Request("https://example.com");
r.hasOwnProperty("headers");  // Instance: true
                              // Prototype: false
```

### 4. Property Descriptors
```javascript
Object.getOwnPropertyDescriptor(r, "headers");
// Instance: {value: ..., writable: false, enumerable: true, configurable: false}
// Prototype: undefined (property is on prototype, not instance)
```

Any Worker code that depends on these behaviors would break if we forcibly migrated them.

## Why HolderV2() Doesn't Work for Instance Properties

For instance properties via `SetNativeDataProperty`, when someone uses a native object as a prototype:

```javascript
function Evil() {}
Evil.prototype = new Request("https://example.com");
new Evil().headers;  // Should throw TypeError
```

- `This()` returns the `Evil` instance → `HasInstance` check fails → correctly throws
- `HolderV2()` returns the `Request` from prototype chain → `HasInstance` passes → **incorrectly succeeds**

We can add `HolderV2() == This()` check, but this still requires `This()`.

## Named Property Interceptors

For interceptors (`SetHandler` on prototype), the situation is different:

- `HolderV2()` returns the **prototype object** (where the handler is installed)
- `This()` returns the **actual instance**

Without `This()`, interceptors cannot access the correct object at all. This isn't a security issue—it's "the API doesn't work."

Chromium handles this with explicit checks:
```cpp
// WebIDL spec: https://webidl.spec.whatwg.org/#legacy-platform-object-set
if (info.Holder() == info.This()) {
  // Only proceed if they match
}
```

## What We Need

One of:

1. **Keep `PropertyCallbackInfo::This()` available** (even if "discouraged")

2. **Add `PropertyCallbackInfo::Receiver()`** - a new method that returns the receiver, for cases where `HolderV2()` is insufficient

3. **Document the migration path clearly** - if V8's position is "use `SetAccessorProperty` with `FunctionTemplate`", document that this is the only supported pattern going forward

## Questions for V8 Team

1. What is Chromium's plan for `SetNativeDataProperty` callbacks? Are they migrating everything to `SetAccessorProperty`?

2. For named property interceptors, what's the recommended way to get the actual receiver?

3. Is there a timeline for when `This()` will actually be removed (vs just deprecated)?

4. For embedders with backwards compatibility requirements, what's the recommended approach?

---

*Erik Corry, Cloudflare*
