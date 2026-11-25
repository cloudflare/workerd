using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    (name = "main", worker = (
      modules = [(name = "index.mjs", esModule = embed "index.mjs")],
      # The UDP opt-in must supply its stream dependency even on the oldest compatibility date.
      compatibilityDate = "2000-01-01",
      compatibilityFlags = ["outbound_udp_sockets"],
      globalOutbound = "network",
      bindings = [(name = "OBJECT", durableObjectNamespace = (className = "Sender"))],
      durableObjectNamespaces = [(className = "Sender", uniqueKey = "udp-sender")],
      durableObjectStorage = (inMemory = void),
    )),
    (name = "network", network = (allow = ["local"])),
  ],
  sockets = [(name = "http", address = "127.0.0.1:0", http = (), service = "main")],
);
