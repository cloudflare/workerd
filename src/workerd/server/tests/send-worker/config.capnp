# The services `workerd-with-toys` registers in code (`kj-toy`, `send-toy`, `send-hello`,
# `send-fetch`, `send-fetch-copy`; see main.rs), each on a socket of its name, and JavaScript
# Workers beside and in front of them.
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [
    ( name = "js-hello", worker = .jsHello ),
    ( name = "js-fetch", worker = (
        modules = [ ( name = "fetch.mjs", esModule = embed "fetch.mjs" ) ],
        compatibilityDate = "2024-01-01",
        bindings = [ ( name = "backend", service = "kj-toy" ) ],
    ) ),
    ( name = "js-fetch-send-toy", worker = (
        modules = [ ( name = "fetch.mjs", esModule = embed "fetch.mjs" ) ],
        compatibilityDate = "2024-01-01",
        bindings = [ ( name = "backend", service = "send-toy" ) ],
    ) ),
    ( name = "js-fetch-send-fetch", worker = (
        modules = [ ( name = "fetch.mjs", esModule = embed "fetch.mjs" ) ],
        compatibilityDate = "2024-01-01",
        bindings = [ ( name = "backend", service = "send-fetch" ) ],
    ) ),
  ],
  sockets = [
    ( name = "kj-toy", address = "127.0.0.1:0", http = (), service = "kj-toy" ),
    ( name = "send-toy", address = "127.0.0.1:0", http = (), service = "send-toy" ),
    ( name = "send-hello", address = "127.0.0.1:0", http = (), service = "send-hello" ),
    ( name = "send-fetch", address = "127.0.0.1:0", http = (), service = "send-fetch" ),
    ( name = "send-fetch-copy", address = "127.0.0.1:0", http = (), service = "send-fetch-copy" ),
    ( name = "js-hello", address = "127.0.0.1:0", http = (), service = "js-hello" ),
    ( name = "js-fetch", address = "127.0.0.1:0", http = (), service = "js-fetch" ),
    ( name = "js-fetch-send-toy", address = "127.0.0.1:0", http = (),
      service = "js-fetch-send-toy" ),
    ( name = "js-fetch-send-fetch", address = "127.0.0.1:0", http = (),
      service = "js-fetch-send-fetch" ),
  ],
);

const jsHello :Workerd.Worker = (
  modules = [ ( name = "hello.mjs", esModule = embed "hello.mjs" ) ],
  compatibilityDate = "2024-01-01",
);
