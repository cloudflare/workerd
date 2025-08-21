using Workerd = import "/workerd/workerd.capnp";

const reprl :Workerd.Config = (
  services = [
    (name = "main", worker = .replServer),
    (name = "consumer", worker = .consumerWorker)
  ],
  
  # We don't need sockets for REPRL mode as it uses direct file descriptors
  # sockets = [ ( name = "http", address = "*:8080", http = (), service = "main" ) ]
);

const replServer :Workerd.Worker = (
  modules = [
    (name = "worker", esModule = embed "worker-full.js")
  ],
  
  # Add a comprehensive set of bindings for all APIs
  bindings = [
    (name = "CONSUMER", service = "consumer")
  ],
  
  # Latest compatibility date
  compatibilityDate = "2025-08-08",
  
  # Enable all compatibility flags for maximum API coverage
  compatibilityFlags = [
    "nodejs_compat", 
    "experimental", 
    "unsafe_module",
    "enable_nodejs_fs_module",
    "html_rewriter_treats_esi_include_as_void_tag",
    "durable_object_rename",
    "service_binding_extra_handlers",
    "expose_global_message_channel",
    "enable_web_file_system",
  ]
);

const consumerWorker :Workerd.Worker = (
	modules = [
		( name = "consumer", esModule = embed "worker-consume-request.js" )
	],
	compatibilityDate = "2025-08-08"
);
