using Workerd = import "/workerd/workerd.capnp";

const extension :Workerd.Extension = (
  modules = [
    ( name = "burrito-shop:burrito-shop", esModule = embed "burrito-shop.js" ),
    ( name = "burrito-shop-internal:kitchen", esModule = embed "kitchen.js", internal = true ),
  ]
);
