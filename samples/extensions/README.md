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

#### Usage
The module `burrito-shop:burrito-shop`, which is defined in the config [burrito-shop.capnp](burrito-shop.capnp), is an importable module.

You can import it as shown below:

```javascript
import { BurritoShop } from "burrito-shop:burrito-shop";
```

#### Definition
This import definition is demonstrated in [burrito-shop.js](burrito-shop.js).
As shown in 

### Environment Variable (with internal initialization from environment variables)

#### Usage

The module `burrito-shop:binding` is defined in the config [burrito-shop.capnp](burrito-shop.capnp) and is provided as an environment variable.

You can access it as shown below in [worker.js](worker.js) provided the modules name is shop(as defined in the bindings section of [config.capnp](config.capnp)

```javascript
env.shop
```

The initialization is demonstrated in [binding.js](binding.js).

##### Definition

- The environment name of `shop` is provided in [config.capnp](config.capnp) with the field `name` located under bindings.
```
  bindings = [
    ( 
        ...
        name = "shop"
        ...
    )]
```
- An environment binding must be marked as internal.

#### Using a exported function that isn't default as the entrypoint
A default exported entrypoint is not required for binding as long as you define a different entrypoint with `entrypoint = "methodname"`, for example you can define a non-default function as the entrypoint in [config.capnp](config.capnp).
An example is provided below

In [binding.js](binding.js)
```javascript
export function makeMagicBurritoBinding(env) {
    return new BurritoShop(env.recipes);
}
```

In [config.capnp](config.capnp)
```
    (
      name = "shop",
      wrapped = (
        ...
        entrypoint = "makeMagicBurritoBinding"
        ...
    )
```


