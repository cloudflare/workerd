using Workerd = import "/workerd/workerd.capnp";

const tcpIngressExample :Workerd.Config = (
  services = [
    (name = "main", worker = .worker),
  ],

  sockets = [
    ( name = "http", address = "*:8080", http = (), service = "main" ),
    ( name = "tcp", address = "*:8081", tcp = (), service = "main" )
  ]
);

const worker :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker.js")
  ],
  compatibilityFlags = ["nodejs_compat_v2", "experimental"],
  compatibilityDate = "2023-02-28",
);
