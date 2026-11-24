using Workerd = import "/workerd/workerd.capnp";

# The front of `workerd serve --threads`: one Worker (`front-worker.mjs`) behind every kind of
# stream socket the front accepts from, so that each can be compared on one thread and on several.
const config :Workerd.Config = (
  services = [
    ( name = "front",
      worker = (
        modules = [(name = "worker", esModule = embed "front-worker.mjs")],
        compatibilityDate = "2025-06-01",
        compatibilityFlags = ["enable_request_signal"],
        bindings = [(name = "LEDGER", durableObjectNamespace = "Ledger")],
        durableObjectNamespaces = [(className = "Ledger", uniqueKey = "ledger")],
        durableObjectStorage = (inMemory = void),
      )
    ),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:0", http = (), service = "front"),
    # A second plain HTTP socket, for the tests that give the server a socket of their own
    # (`--socket-addr`, `--socket-fd`).
    (name = "alt", address = "127.0.0.1:0", http = (), service = "front"),
    (name = "tcp", address = "127.0.0.1:0", tcp = (), service = "front"),
    ( name = "https", address = "127.0.0.1:0",
      https = (
        options = (),
        tlsOptions = (
          keypair = (
            privateKey = embed "../../../api/tests/connect-handler-server.key",
            certificateChain = embed "../../../api/tests/connect-handler-server.pem",
          ),
        ),
      ),
      service = "front",
    ),
    ( name = "rewrite", address = "127.0.0.1:0",
      http = (
        forwardedProtoHeader = "X-Forwarded-Proto",
        cfBlobHeader = "CF-Blob",
        injectRequestHeaders = [(name = "X-Injected", value = "by the socket")],
        injectResponseHeaders = [(name = "X-Injected-Response", value = "by the socket")],
      ),
      service = "front",
    ),
    (name = "proxy", address = "127.0.0.1:0", http = (style = proxy), service = "front"),
  ],
);
