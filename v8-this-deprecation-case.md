# Case for PropertyCallbackInfo::This() (or Equivalent)

## Summary

After investigation, we found that **most uses of `PropertyCallbackInfo::This()` in workerd can be eliminated** by aligning with Chromium's patterns. However, there's one remaining case for legacy Workers.

## What We Found

### 1. Prototype Properties (SOLVED)

Workerd already migrated to `SetAccessorProperty` with `FunctionTemplate` for prototype properties in 2022:

```cpp
auto getterFn = v8::FunctionTemplate::New(isolate, Gcb::callback);
prototype->SetAccessorProperty(v8Name, getterFn, ...);
```

This uses `FunctionCallbackInfo::This()` which is **NOT deprecated**. Workers with compatibility date >= 2022-01-31 use this pattern.

### 2. Named Property Interceptors (CAN BE SOLVED)

We discovered workerd puts interceptors on the **prototype**, while Chromium puts them on the **instance template**:

```python
# Chromium (interface.py line 5815)
interceptor_template = "${instance_template}"  # Not prototype!
```

When interceptor is on instance: `HolderV2() == This()` → can use `HolderV2()`
When interceptor is on prototype: `HolderV2() != This()` → need `This()`

**Fix**: Change `registerWildcardProperty()` from:
```cpp
prototype->SetHandler(...)
```
to:
```cpp
instance->SetHandler(...)
```

This matches Chromium and eliminates the need for `This()` in interceptors. See `interceptor-on-instance.patch`.

### 3. Legacy Instance Properties (NEEDS SOLUTION)

Workers with compatibility date < 2022-01-31 use instance properties via `SetNativeDataProperty`. These need `This()` to detect "prototype-as-this" attacks:

```cpp
auto obj = info.HolderV2();
if (obj != info.This()) {  // Still need This() for this check
  throwTypeError(isolate, kIllegalInvocation);
}
```

## Observable Differences

Moving interceptor from prototype to instance may affect:

| Operation | Prototype Interceptor | Instance Interceptor |
|-----------|----------------------|---------------------|
| `hasOwnProperty('x')` | Query callback NOT called | Query callback called (if defined) |
| `Object.keys(obj)` | Enumerator on prototype | Enumerator on instance |

However, workerd uses `kNonMasking` and has no query callback, so impact may be minimal.

V8 test `InterceptorHasOwnProperty` (test-api-interceptors.cc:1071) confirms:
- With interceptor on instance: `hasOwnProperty` returns false for non-intercepted properties
- After setting the property: `hasOwnProperty` returns true

## Questions for V8 Team

1. **For the legacy instance property case**: Is there a recommended alternative to `This()` for detecting when an object is accessed through the prototype chain vs directly?

2. **For interceptors**: V8 bug 455600234 says "For interceptors, using This() is always semantically correct." Does this mean `This()` will remain available for interceptors even after deprecation?

3. **Timeline**: When will `This()` actually be removed vs just deprecated with warnings?

## Summary

| Use Case | Current Status | Solution |
|----------|---------------|----------|
| Prototype properties (new Workers) | Uses `FunctionCallbackInfo::This()` | Already solved |
| Named interceptors | Uses `PropertyCallbackInfo::This()` | Move to instance template |
| Instance properties (legacy Workers) | Uses `PropertyCallbackInfo::This()` | Needs `This()` or alternative |

---

*Erik Corry, Cloudflare*
