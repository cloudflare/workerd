# Case for PropertyCallbackInfo::This()

## Summary

Cloudflare Workers (workerd) genuinely needs `PropertyCallbackInfo::This()` and **cannot follow Chromium's pattern**. We tested moving interceptors from prototype to instance template (like Chromium does) and it breaks RPC functionality.

## What We Tested

### Attempt: Move Interceptors to Instance Template

Chromium puts interceptors on `instance_template`, while workerd puts them on `prototype`:

```python
# Chromium (interface.py line 5815)
interceptor_template = "${instance_template}"
```

```cpp
// Workerd (resource.h)
prototype->SetHandler(...)  // On prototype, not instance
```

We tried changing workerd to match Chromium:

```cpp
instance->SetHandler(...)  // Changed to instance
```

**Result**: JSG unit tests pass, but **RPC tests fail**:
```
TypeError: stub.foo is not a function
TypeError: stub.increment is not a function
```

### Why It Fails

Workerd uses prototype-level interceptors for **RPC stubs**. These are wrapper objects that intercept any property access and forward it over RPC:

```javascript
const stub = env.MY_DURABLE_OBJECT.get(id);
stub.anyMethodName(args);  // Intercepted and sent over RPC
```

With interceptor on prototype:
- Works for any object inheriting from `JsRpcStub.prototype`
- Enables flexible RPC proxying

With interceptor on instance:
- Only works for objects created directly from the template
- Breaks RPC stub pattern

This is fundamentally different from Chromium's use of interceptors (HTMLCollection, Storage, etc.) where the interceptor is on the actual object being accessed.

## What Workerd Needs

| Use Case | API Needed | Can Migrate? |
|----------|-----------|--------------|
| Prototype properties (new Workers) | `FunctionCallbackInfo::This()` | Already done (2022) |
| Named interceptors (RPC stubs) | `PropertyCallbackInfo::This()` | **No** - breaks RPC |
| Instance properties (legacy Workers) | `PropertyCallbackInfo::This()` | **No** - backwards compat |

## Request for V8 Team

`PropertyCallbackInfo::This()` is needed for workerd's legitimate use case of prototype-level interceptors. This is not a case of doing something wrong that can be fixed by following Chromium's pattern - we tested that and it breaks functionality.

Options:
1. **Keep `This()` available** for prototype-level interceptors
2. **Provide alternative API** to get the receiver in `PropertyCallbackInfo`
3. **Clarify the migration path** for embedders who need prototype-level interceptors

## Test Evidence

```bash
# Without patch (interceptor on prototype):
bazel test '//src/workerd/api/tests:js-rpc-test@'  # PASSED

# With patch (interceptor on instance):
bazel test '//src/workerd/api/tests:js-rpc-test@'  # FAILED
# TypeError: stub.foo is not a function
# TypeError: stub.increment is not a function
```

---

*Erik Corry, Cloudflare*
