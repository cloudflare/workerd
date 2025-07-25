# Imports the base schema for workerd configuration files.

# Refer to the comments in /src/workerd/server/workerd.capnp for more details.

using Workerd = import "/workerd/workerd.capnp";

# A constant of type Workerd.Config defines the top-level configuration for an
# instance of the workerd runtime. A single config file can contain multiple
# Workerd.Config definitions and must have at least one.
const helloWorldExample :Workerd.Config = (

  # Every workerd instance consists of a set of named services. A worker, for instance,
  # is a type of service. Other types of services can include external servers, the
  # ability to talk to a network, or accessing a disk directory. Here we create a single
  # worker service. The configuration details for the worker are defined below.
  services = [
    (name = "other", worker = .other),
    ( name = "internet",
        network = (
          allow = ["public", "private"],
        )
      )
  ],

  # Each configuration defines the sockets on which the server will listen.
  # Here, we create a single socket that will listen on localhost port 8080, and will
  # dispatch to the "main" service that we defined above.
  sockets = [
    ( name = "http", address = "*:8081", http = (), service = "other" )
  ],

);

const other :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "other.js"),
    (name = "rpc.js", esModule = embed "rpc.js")
  ],
  compatibilityDate = "2025-01-01",
  bindings = [(service = (name = "other", entrypoint = "W"), name = "SELF")]

);
