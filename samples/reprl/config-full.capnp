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
    # Standard bindings
    (name = "secret", text = "thisisasecret"),
    
    # Cache API
    (name = "CACHE", memoryCache = (
      id = "abc123",
      limits = (
        maxKeys = 100,
        maxValueSize = 1024 * 1024, # 1MB
        maxTotalValueSize = 10 * 1024 * 1024, # 10MB
      ),
    )),
    
    # KV namespace for KV API
    (name = "KV_NAMESPACE", kvNamespace = (
      id = "kv123",
    )),
    
    # R2 bucket for R2 API
    (name = "R2_BUCKET", r2Bucket = (
      name = "test-bucket",
      jurisdiction = "eu",
    )),
    
    # R2 Admin for R2 administration API
    (name = "R2_ADMIN", r2Admin = (
      jurisdiction = "eu",
    )),
    
    # Queue for Queue API
    (name = "QUEUE", queue = (
      name = "test-queue"
    )),
    
    # Analytics Engine binding
    (name = "ANALYTICS", analyticsEngine = (
      dataset = "test_dataset"
    )),
    
    # D1 Database
    (name = "DB", d1Database = (
      id = "d1123",
    )),
    
    # Durable Object namespace
    (name = "DURABLE_OBJECT", durableObjectNamespace = (
      className = "TestDurableObject",
      uniqueKey = "test_unique_key"
    )),
    
    # Service binding for Worker-to-Worker communication
    (name = "SERVICE", service = "main"),
    
    # Hyperdrive binding
    (name = "HYPERDRIVE", hyperdrive = (
      id = "hyperdrive123",
      localScheme = "mysql",
      localUsername = "user",
      localPassword = "password",
      localHostname = "localhost",
      localHostport = 3306,
      localDatabase = "test_db",
    )),
    
    # Email binding for email API
    (name = "SMTP", outgoingEmail = (
      type = "smtp",
      hostname = "localhost",
      port = 25,
      username = "user",
      password = "password",
    )),
    
    # Add a binding for websocket (for testing WebSocket API)
    (name = "WEBSOCKET_SERVICE", webSocket = (
      url = "wss://localhost:8080"
    )),
  ],
  
  # Durables
  durableObjects = [
    (className = "TestDurableObject", uniqueKey = "test_unique_key")
  ],
  
  # Latest compatibility date
  compatibilityDate = "2024-01-01",
  
  # Enable all compatibility flags for maximum API coverage
  compatibilityFlags = [
    "nodejs_compat", 
    "experimental", 
    "unsafe_module",
    "nodejs_buffer",
    "nodejs_diagnostics_channel",
    "nodejs_dns_module",
    "web_socket_compression",
    "http_stream_sse",
    "transformstream_enable_standard_constructor",
    "fetcher_no_resource_timing",
    "durable_object_alarms", 
    "transformstream_enable_standard_constructor"
  ]
);

# Define the Durable Object class
const durableObjects = (
  classes = [
    (name = "TestDurableObject", scriptId = "main")
  ]
);
