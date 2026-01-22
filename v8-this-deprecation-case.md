# Case for PropertyCallbackInfo::This()

## Summary

Unfortunately, Cloudflare Workers (workerd) genuinely needs `PropertyCallbackInfo::This()` and cannot follow Chromium's pattern. We tested moving interceptors from prototype to instance template (like Chromium does) and it breaks RPC functionality.

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

Result: Our RPC tests fail:
```
TypeError: stub.foo is not a function
TypeError: stub.increment is not a function
```

### Why It Fails

Workerd uses prototype-level interceptors for RPC stubs. These are wrapper objects that intercept any property access and forward it over RPC:

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

## Backwards compatibility issue

In addition to the RPC issue we also have a backwards compatibility issue. Workers take
backwards compatibility very seriously - we can't break already-deployed scripts when
we run them with a newer version of V8.

In 2022 we moved to setting accessors on the prototype instead of on the instance.
This solved some compatibility issues, eg. people could not subclass and get the
expected behaviour. It also aligns better with IDL. However we still have a
compatibility flag for this since it is trivially observable from JS.

## What Workerd Needs

| Use Case | API Needed | Can Migrate? |
|----------|-----------|--------------|
| Prototype properties (new Workers) | `FunctionCallbackInfo::This()` | Already done (2022) |
| Named interceptors (RPC stubs) | `PropertyCallbackInfo::This()` | **No** - breaks RPC |
| Instance properties (legacy Workers) | `PropertyCallbackInfo::This()` | **No** - backwards compat |

## Conclusion

`PropertyCallbackInfo::This()` is needed for workerd's legitimate use case of prototype-level interceptors. This is not a case of something that can be fixed by following Chromium's pattern - we tested that and it breaks functionality.

Options:
1. Keep `This()` available for prototype-level interceptors
2. Provide alternative API to get the receiver in `PropertyCallbackInfo`
3. Clarify the migration path for embedders who need prototype-level interceptors

---

*Erik Corry, Cloudflare*
