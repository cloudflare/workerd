using Workerd = import "/workerd/workerd.capnp";

# Wrapped binding modules must be internal, so the mock ExtendedFetcher is registered via an
# extension.
const extension :Workerd.Extension = (
  modules = [
    ( name = "test:hyperdrive-extended-fetcher-mock",
      esModule = embed "extended-fetcher-mock.js",
      internal = true ),
  ]
);
