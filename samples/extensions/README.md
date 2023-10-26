# Extensions

This directory contains comprehensive samples of using workerd extensions.

This example defines a fictional burrito shop extension in
[burrito-shop.capnp](burrito-shop.capnp)
and demonstrates following features:

- using modules to provide new user-level api: [burrito-shop.js](burrito-shop.js) and
  [worker.js](worker.js)
- using internal modules to hide implementation details from the user: [kitchen.js](kitchen.js)

The sample will be extended as more functionality is implemented.

## Running

```
$ bazel run //src/workerd/server:workerd -- serve $(pwd)/samples/extensions/config.capnp
$ curl localhost:8080 -X POST -d 'veggie'
9
```

## Demonstrated Methods

### Importable Module

This method demonstrates a publicly importable module, with the initialization being handled by the worker in  [worker.js](worker.js)

#### Accessibility

The module `burrito-shop:burrito-shop`, which is defined in the config [burrito-shop.capnp](burrito-shop.capnp), is an importable module.

You can import it as demonstrated in [burrito-shop.js](burrito-shop.js)

```javascript
import { BurritoShop } from "burrito-shop:burrito-shop";
```

#### Definition

This import definition is demonstrated in [burrito-shop.capnp](burrito-shop.capnp).

```capnp
...
( name = "burrito-shop:burrito-shop", esModule = embed "burrito-shop.js" )
...
```

### Environment Variable (with internal initialization from environment variables)

#### Accessibility

The module `burrito-shop:binding` is defined in the config [burrito-shop.capnp](burrito-shop.capnp) and is provided as an environment variable in standard `workerd` fashion. As we're using an esmodule it's provided as an argument to the workers entrypoint.

You can access it as shown below in [worker.js](worker.js)

```javascript
return new Response(env.shop.makeBurrito(burritoType).price());
```

The initialization for this module is demonstrated in [binding.js](binding.js), the default behavior of worker uses the exported function for initialization, and is called by `workerd`.

#### Definition

- The environment name of `shop` is provided in [config.capnp](config.capnp) with the field `name` located in the binding definition used by the worker.

```capnp
bindings = [
( 
    ...
    name = "shop"
    ...
)]
```

- **Note** An environment bindings module must be marked as internal, this is demonstrated in [burrito-shop.capnp](burrito-shop.capnp)

#### Using a exported function that isn't default as the entrypoint

A default exported entrypoint is not required for binding as long as you define a different entrypoint with `entrypoint = "methodname"`, for example you can define a non-default function as the entrypoint with `entrypoint = "makeMagicBurritoBinding"` in [config.capnp](config.capnp).  An example is provided below on how you could change the entrypoint.

In [binding.js](binding.js)

```javascript
export function makeMagicBurritoBinding(env) {
    return new BurritoShop(env.recipes);
}
```

In [config.capnp](config.capnp)

```capnp
(
  name = "shop",
  wrapped = (
    ...
    entrypoint = "makeMagicBurritoBinding" # The new entrypoint name
    ...
)
```
