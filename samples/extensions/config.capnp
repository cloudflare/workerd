using Workerd = import "/workerd/workerd.capnp";
using BurritoShop = import "burrito-shop.capnp";

const helloWorldExample :Workerd.Config = (
  services = [ (name = "main", worker = .helloWorld) ],
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ],
  extensions = [ BurritoShop.extension ],
);

const helloWorld :Workerd.Worker = (
  modules = [ (name = "worker", esModule = embed "worker.js") ],
  compatibilityDate = "2022-09-16",
  bindings = [
    ( name = "shop",
      # wrapped bindings provide specialized API access to inner bindings.
      # inner binding will typically include internet service binding,
      # but could be arbitrary, including configuration and data.
      wrapped = (
        moduleName = "burrito-shop:binding",
        innerBindings = [(
          name = "recipes",
          json = embed "recipes.json"
        )],
      ))
  ],
);
