using Workerd = import "/workerd/workerd.capnp";

const extension :Workerd.Extension = (
  modules = [
    # this module will be directly importable by the user
    ( name = "burrito-shop:burrito-shop", esModule = embed "burrito-shop.js" ),
    # internal modules can be imported only by public extension modules
    ( name = "burrito-shop-internal:burrito-shop-impl", esModule = embed "burrito-shop-impl.js", internal = true ),
    ( name = "burrito-shop-internal:kitchen", esModule = embed "kitchen.js", internal = true ),
    # only modules marked as internal can be used for bindings
    ( name = "burrito-shop:binding", esModule = embed "binding.js", internal = true ),
  ]
);
