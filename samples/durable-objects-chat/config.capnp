# This is the edge chat demo found at:
#
#     https://github.com/cloudflare/workers-chat-demo

using Workerd = import "/workerd/workerd.capnp";

# A constant of type `Workerd.Config` will be recognized as the top-level configuration.
const config :Workerd.Config = (
  # We have one nanoservice: the chat worker.
  services = [ (name = "chat", worker = .chatWorker) ],

  # We export it via HTTP on port 8080.
  sockets = [ ( name = "http", address = "*:8080", http = (), service = "chat" ) ],
);

# For legibility we define the Worker's config as a separate constant.
const chatWorker :Workerd.Worker = (
  # All Workers must declare a compatibility date, which ensures that if `workerd` is updated to
  # a newer version with breaking changes, it will emulate the API as it existed on this date, so
  # the Worker won't break.
  compatibilityDate = "2022-09-16",

  # This worker is modules-based.
  modules = [
    # Our code is in an ES module (JavaScript).
    (name = "chat.js", esModule = embed "chat.js"),

    # We also have an HTML file containing the client side of the app. We embed this as a text
    # module, so that it can be served to the client.
    (name = "chat.html", text = embed "chat.html"),
  ],

  # The Worker has two Durable Object classes, each of which needs an attached namespace.
  # The `uniqueKey`s can be any string, and are used to generate IDs. Keep the keys secret if you
  # don't want clients to be able to forge valid IDs -- or don't, if you don't care about that.
  #
  # In the example here, we've generated 32-character random hex keys, but again, the string can
  # be anything. These were generated specifically for this demo config; we do not use these
  # values in production.
  durableObjectNamespaces = [
    (className = "ChatRoom", uniqueKey = "210bd0cbd803ef7883a1ee9d86cce06e"),
    (className = "RateLimiter", uniqueKey = "b37b1c65c4291f3170033b0e9dd30ee1"),
  ],

  # To use Durable Objects we must declare how they are stored.
  #
  # As of this writing, `workerd` supports in-memory-only Durable Objects -- so, not really
  # "durable", as all data is lost when workerd restarts. However, this still allows us to run the
  # chat demo for testing purposes. (We plan to add actual storage for Durable Objects eventually,
  # but the storage system behind Cloudflare Workers is inherently tied to our network so did not
  # make sense to release as-is.)
  durableObjectStorage = (inMemory = void),

  # We must declare bindings to allow us to call back to our own Durable Object namespaces. These
  # show up as properties on the `env` object passed to `fetch()`.
  bindings = [
    (name = "rooms", durableObjectNamespace = "ChatRoom"),
    (name = "limiters", durableObjectNamespace = "RateLimiter"),
  ],
);

