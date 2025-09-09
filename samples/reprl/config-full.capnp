using Workerd = import "/workerd/workerd.capnp";

const reprl :Workerd.Config = (
  services = [
    (name = "main", worker = .replServer),
    (name = "consumer", worker = .consumerWorker),
    # Add test services for Cloudflare API bindings
    (name = "test-kv", worker = .kvMockWorker),
    (name = "test-d1-mock", worker = .d1MockWorker),
    (name = "test-r2", worker = .r2MockWorker),
    (name = "test-analytics", worker = .analyticsMockWorker),
    (name = "test-queue", worker = .queueMockWorker),
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
    (name = "CONSUMER", service = "consumer"),
    (name = "volatileCache", memoryCache = (
      id = "abc123",
      limits = (
        maxKeys = 100,
        maxValueSize = 1024,
        maxTotalValueSize = 102400,
      ),
    )),
    # Cloudflare API bindings for testing
    (name = "MY_KV", kvNamespace = "test-kv"),
    (name = "MY_D1", wrapped = (
      moduleName = "cloudflare-internal:d1-api",
      innerBindings = [(name = "fetcher", service = "test-d1-mock")],
    )),
    (name = "MY_R2", r2Bucket = "test-r2"),
    (name = "ANALYTICS", analyticsEngine = "test-analytics"),
    (name = "MY_QUEUE", queue = "test-queue"),
  ],

  # Latest compatibility date
  compatibilityDate = "2024-12-01",

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
	compatibilityDate = "2024-12-01"
);

const kvMockWorker :Workerd.Worker = (
	modules = [
		( name = "kv", esModule = embed "kv-mock.js" )
	],
	compatibilityDate = "2024-12-01"
);

const d1MockWorker :Workerd.Worker = (
	modules = [
		( name = "d1", esModule = embed "d1-mock.js" )
	],
	compatibilityDate = "2024-12-01"
);

const r2MockWorker :Workerd.Worker = (
	modules = [
		( name = "r2", esModule = embed "r2-mock.js" )
	],
	compatibilityDate = "2024-12-01"
);

const analyticsMockWorker :Workerd.Worker = (
	modules = [
		( name = "analytics", esModule = embed "analytics-mock.js" )
	],
	compatibilityDate = "2024-12-01"
);

const queueMockWorker :Workerd.Worker = (
	modules = [
		( name = "queue", esModule = embed "queue-mock.js" )
	],
	compatibilityDate = "2024-12-01"
);
