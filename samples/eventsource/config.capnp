using Workerd = import "/workerd/workerd.capnp";

const eventSourceExample :Workerd.Config = (

  services = [
    (name = "main", worker = .eventsource),
    (name = "internet", network = (allow = ["private"]))
  ],

  sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const eventsource :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityDate = "2023-02-28",
  compatibilityFlags = ["experimental"],
);
