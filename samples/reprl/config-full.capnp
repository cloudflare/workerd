using Workerd = import "/workerd/workerd.capnp";

const reprl :Workerd.Config = (
  services = [ (name = "main", worker = .replServer) ],
  
  # We don't need sockets for REPRL mode as it uses direct file descriptors
  # sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const replServer :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker-full.js")
  ],
  
  # Add a comprehensive set of bindings for all APIs
  bindings = [
  ],
  
  # Latest compatibility date
  compatibilityDate = "2025-08-08",
  
  # Enable all compatibility flags for maximum API coverage
  compatibilityFlags = [
    "nodejs_compat", 
    "experimental", 
    "unsafe_module",
    "enable_nodejs_fs_module",
  ]
);
