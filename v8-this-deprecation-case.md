# Case for Un-deprecating PropertyCallbackInfo::This()

## Summary

The deprecation of `PropertyCallbackInfo::This()` in favor of `HolderV2()` creates an **unsolvable security vulnerability** for embedders that cannot be mitigated without breaking backwards compatibility. We request that V8 either:

1. Un-deprecate `This()` for property callbacks, or
2. Provide an alternative API that gives access to the actual receiver

## The Security Problem

When `HolderV2()` is used instead of `This()`, embedders cannot distinguish between:

```javascript
// Legitimate access
const req = new Request("https://api.example.com", {
  headers: { "Authorization": "Bearer secret-token" }
});
req.headers.get("Authorization");  // Should work
```

and:

```javascript
// Prototype-as-this attack
function EvilRequest() {}
EvilRequest.prototype = new Request("https://api.example.com", {
  headers: { "Authorization": "Bearer secret-token" }
});
const evil = new EvilRequest();
evil.headers;  // With HolderV2() only, this returns the secret headers!
```

With `This()`: The embedder sees `evil` (an EvilRequest), fails `HasInstance`, rejects.
With `HolderV2()`: The embedder sees the Request from the prototype chain, passes `HasInstance`, **leaks data**.

## Why This Matters for Cloudflare Workers

Cloudflare Workers runs untrusted JavaScript from millions of customers in a shared environment. Security boundaries are critical.

### Real APIs Affected

**1. Request/Response Objects**
```javascript
// Attack: Extract body/headers from a Request used as prototype
function Wrapper() {}
Wrapper.prototype = realRequest;  // Request with sensitive headers
new Wrapper().headers;  // Leaks Authorization, Cookie, etc.
```

**2. Streams (ReadableStream/WritableStream)**
```javascript
// Attack: Access internal stream state
function FakeStream() {}
FakeStream.prototype = realReadableStream;
new FakeStream().locked;  // Accesses internal state of real stream
```

**3. WebSocket**
```javascript
// Attack: Access WebSocket properties
function FakeSocket() {}
FakeSocket.prototype = realWebSocket;
new FakeSocket().url;  // Leaks the WebSocket URL
new FakeSocket().readyState;  // Leaks connection state
```

**4. Crypto Keys (SubtleCrypto)**
```javascript
// Attack: Potentially extract key material
function FakeKey() {}
FakeKey.prototype = realCryptoKey;
new FakeKey().algorithm;  // Leaks key algorithm details
```

### The Backwards Compatibility Constraint

Cloudflare Workers has **strict backwards compatibility requirements**. Code deployed years ago must continue to work identically. We cannot:

- Change property semantics (data vs accessor)
- Change prototype chain behavior
- Add new error conditions that weren't there before

The *only* viable solution is to detect the attack at the native layer using `This()`.

## Why HolderV2() Is Insufficient

| Scenario | This() | HolderV2() | Correct Behavior |
|----------|--------|------------|------------------|
| Direct access: `obj.prop` | obj | obj | Allow |
| Prototype attack: `derived.prop` | derived | prototypeObj | **Reject** |
| Reflect.get: `Reflect.get(obj, 'prop', receiver)` | receiver | obj | Depends |

`HolderV2()` cannot distinguish case 1 from case 2. Only `This()` can.

## The Interceptor Problem

For interceptors (`SetHandler`), the situation is worse:

- Interceptors are registered on the **prototype**
- `HolderV2()` returns the prototype object itself
- `This()` returns the actual instance

Without `This()`, interceptors cannot access the correct object at all, not just for security—for basic functionality.

```cpp
// Current workerd code - REQUIRES This()
static v8::Intercepted getter(..., const v8::PropertyCallbackInfo<v8::Value>& info) {
  auto obj = info.This();  // Need the actual instance, not the prototype
  auto& self = extractInternalPointer<T>(context, obj);
  // ... access instance state
}
```

## Proposed Solutions

### Option A: Un-deprecate This()
The simplest solution. `This()` provides essential information that `HolderV2()` cannot.

### Option B: Add a New API
```cpp
// Something like:
v8::Local<v8::Object> PropertyCallbackInfo::Receiver() const;
```
That explicitly provides the receiver for security checks.

### Option C: Add a Flag/Mode
Allow embedders to opt-in to receiving `This()` semantics when they need it for security.

## Conclusion

The deprecation of `This()` forces embedders to choose between:

1. **Security vulnerability** - Use `HolderV2()` and allow prototype-as-this attacks
2. **Breaking changes** - Restructure APIs in backwards-incompatible ways
3. **Ignore deprecation** - Continue using `This()` despite warnings

None of these are acceptable for a production runtime serving millions of users.

We respectfully request that the V8 team reconsider this deprecation and provide a path forward that preserves both security and compatibility.

---

*Contact: Erik Corry, Cloudflare*
