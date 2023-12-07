# This example defines the configuration for a simple Gopher
# (https://en.wikipedia.org/wiki/Gopher_(protocol)) client, with an optional HTTP proxy
# configuration as well.
#
# The comments in this file focus on TCP-related configuration. For details about the rest check
# out the helloworld/config.capnp file (also under the samples directory).
# Also be sure to refer to the comments in /src/workerd/server/workerd.capnp.

using Workerd = import "/workerd/workerd.capnp";

const helloWorldExample :Workerd.Config = (

  # Every workerd instance consists of a set of named services. A worker, for instance,
  # is a type of service. Here we create a single worker service and an ExternalServer which
  # defines a local proxy. The configuration details for these are defined below.
  services = [
    (name = "main", worker = .worker),
  ],

  sockets = [
    ( name = "http", address = "*:8080", http = (), service = "main" ),
    ( name = "tcp", address = "*:8081", tcp = (), service = "main" )
  ]
);

# The definition of the actual worker exposed using the "main" service.
const worker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2023-02-28",
);
