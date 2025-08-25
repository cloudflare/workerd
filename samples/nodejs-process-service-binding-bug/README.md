# process.env service binding issue

Reproduction of an issue with the new `process.env` implementation.

Service bindings will appear on the `process.env` because the property on the imported `env` object will contain an object that will return an `RpcStub` for `env.SERVICE_BINDING.toJSON`.

Proposal is to remove these lines from the `isJsonSerializable()` function

```ts
if (typeof value.toJSON === 'function') {
  // This type is explicitly designed to be JSON-serialized so we'll accept it.
  return true;
}
```

since we only want vanilla JSON objects to be included in `process.env` and not other objects that happen to support JSON serialization.

Requests to this service will return `{}` which represents `process.env.SERVICE_BINDING` and it should be returning `undefined`.
