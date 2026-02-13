# Process 1: caller binds to proxy via service binding.
# Tests three patterns: direct method, Proxy constructor, Proxy with props.

using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "caller", worker = .callerWorker ),
    ( name = "proxy",  worker = .proxyWorker ),
  ],
  sockets = [
    ( name = "http", address = "*:8080", http = (), service = "caller" )
  ]
);

const callerWorker :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "caller-worker.js" ) ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["experimental", "service_binding_extra_handlers"],
  bindings = [
    ( name = "DIRECT", service = (name = "proxy", entrypoint = "DirectProxy") ),
    ( name = "PROXY",  service = (name = "proxy", entrypoint = "ProxyProxy") ),
    ( name = "PROPS",  service = (name = "proxy", entrypoint = "ProxyWithProps",
                                  props = (json = "{\"service\":\"test\"}")) ),
  ]
);

const proxyWorker :Workerd.Worker = (
  modules = [ ( name = "worker", esModule = embed "proxy-worker.js" ) ],
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["experimental", "service_binding_extra_handlers"],
  bindings = [ ( name = "DEBUG_PORT", workerdDebugPort = void ) ]
);
