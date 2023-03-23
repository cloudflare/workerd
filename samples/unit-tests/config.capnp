# This is an example configuration for running unit tests.
#
# (If you're not already familiar with the basic config format, check out the hello world example
# first.)
#
# Most projects will probably use some sort of script or tooling to auto-generate this
# configuration (such as Wrangler), and instead focus on the contents of the JavaScript file.
#
# To run the tests, do:
#
#     workerd test config.capnp

using Workerd = import "/workerd/workerd.capnp";

const unitTests :Workerd.Config = (
  services = [
    # Define the service to be tested.
    (name = "main", worker = .testWorker),

    # Not required, but we can redefine the special "internet" service so that it disallows any
    # outgoing connections. This prohibits the test from talking to the network.
    (name = "internet", network = (allow = []))
  ],

  # For running tests, we do not need to define any sockets, since tests do not accept incoming
  # connections.
);

const testWorker :Workerd.Worker = (
  # Just a regular old worker definition.
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2023-02-28",
);
