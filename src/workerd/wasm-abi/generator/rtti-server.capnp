# Serves JSG RTTI dumps for the generator (see dump_rtti.py, which passes the listening socket
# with --socket-fd):
#   GET /<compat-date>[+flag...]  -> rtti.exportTypes(date, flags), capnp StructureGroups, unpacked
using Workerd = import "/workerd/workerd.capnp";

const config :Workerd.Config = (
  services = [ ( name = "main", worker = .worker ) ],
  sockets = [ ( name = "http", address = "127.0.0.1:18799", http = (), service = "main" ) ],
);

const worker :Workerd.Worker = (
  compatibilityDate = "2024-01-01",
  compatibilityFlags = ["nodejs_compat", "rtti_api"],
  modules = [ ( name = "rtti-server.mjs", esModule = embed "rtti-server.mjs" ) ],
);
