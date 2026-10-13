//! The FFI between the Rust server and the C++ worker factory (worker-factory.h).
//!
//! The factory owns everything that needs the isolate: compiling a worker, starting a request on
//! it, constructing actors and their storage, the RPC bootstrap, channel tokens and the inspector.
//! The server owns everything else and reaches the factory through the `extern "C++"` block; the
//! factory reaches back through the `extern "Rust"` block, whose types are the server's channel
//! objects and its I/O channel factory.
//!
//! Ownership rules: a `KjOwn` moves ownership across the boundary in the direction of the call;
//! a `Box` of a Rust type handed to C++ is a cheap handle (an `Rc` inside), so C++ may hold it
//! for as long as the wrapping KJ object lives and clone it through `*_clone`.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue, and `rewriting_response`, the safe wrapper over the bridge's `unsafe fn new_rewriting_response`"
)]

pub use crate::channels::AbortReason;
pub use crate::channels::ActorClassChannel;
pub use crate::channels::ActorHooks;
pub use crate::channels::ActorNamespaceHandle;
pub use crate::channels::ChannelFactory;
pub use crate::channels::KeepAlive;
pub use crate::channels::ServerHandle;
pub use crate::channels::SubrequestChannel;
pub use crate::channels::WorkerInterfaceList;
pub use crate::channels::WorkerStub;
pub use crate::entry::PendingCommand;
pub use crate::entry::run_pending_command;
pub use crate::in_process::InProcessServer;
pub use crate::in_process::close_in_process_server;
pub use crate::in_process::new_in_process_server;
pub use crate::listen::udp::UdpFlow;
pub use crate::tasks::SpawnedTask;
pub use crate::tasks::task_run;

#[cxx::bridge(namespace = "workerd::server")]
#[expect(
    clippy::missing_safety_doc,
    reason = "cxx bridge extern decls; safety is uniform"
)]
#[expect(
    clippy::too_many_arguments,
    reason = "the bridge declarations mirror the C++ signatures"
)]
#[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
pub mod ffi {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
        type HttpHeaderTable = kj::http::ffi::HttpHeaderTable;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpServiceResponse = kj::http::ffi::HttpServiceResponse;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
    }

    #[namespace = "workerd::rust::worker"]
    unsafe extern "C++" {
        type WorkerInterface = worker::ffi::bridge::WorkerInterface;
        type CustomEvent = worker::ffi::bridge::CustomEvent;
        type CustomEventResult = worker::ffi::bridge::CustomEventResult;
    }

    #[namespace = "workerd::rust::kj_hyper"]
    unsafe extern "C++" {
        type WebSocketErrorHandler = kj_hyper::ffi::WebSocketErrorHandler;
    }

    // =====================================================================================
    // Plain data

    /// A TCP socket bound before the services start, so a worker can learn its own address
    /// (`Worker::Api::getInboundListeners()`).
    struct InboundListener {
        protocol: String,
        address: String,
        port: u16,
    }

    /// An entrypoint the compiled worker exports. `name` is empty for the default export.
    struct EntrypointInfo {
        name: String,
        is_default: bool,
        handlers: Vec<String>,
    }

    /// What the factory learned while compiling a worker.
    struct WorkerInfo {
        entrypoints: Vec<EntrypointInfo>,
        actor_classes: Vec<String>,
        workflow_classes: Vec<String>,
        /// Config errors and warnings, without the `service <name>: ` prefix.
        errors: Vec<String>,
        warnings: Vec<String>,
        /// The `allow_irrevocable_stub_storage` compatibility flag: whether channel tokens the
        /// worker hands out may outlive the process.
        persistent_self_tokens: bool,
        /// A dynamic worker's env channel counts, which `ctx.exports` is numbered after: the
        /// subrequest channels its `env` held (not counting the global-outbound slots) and its
        /// actor classes. The factory serves these numbers itself. Zero for a config worker.
        env_subrequest_channels: u32,
        env_actor_classes: u32,
    }

    struct WorkerSpec {
        name: String,
        /// The worker's inbound TCP listeners, for `Worker::Api::getInboundListeners()`.
        inbound_listeners: Vec<InboundListener>,
        /// The `env` object: an encoded `Globals` message (compiled-bindings.capnp), in words.
        /// Unused for dynamic workers, whose env is a `Frankenvalue`.
        globals: Vec<u64>,
        /// `accessBlobHeader`, when set: requests carrying this header have it parsed into the
        /// request's access info.
        access_blob_header: KjMaybe<String>,
    }

    /// A header the rewriter sets to `value`, or removes when it has none. `name` is a header
    /// of the factory's table, as every header the config's `HttpOptions` name is.
    struct HeaderEdit {
        name: String,
        value: KjMaybe<String>,
    }

    /// One datagram of a UDP flow; `ended` once the flow is over (an idle timeout).
    struct UdpDatagram {
        ended: bool,
        data: Vec<u8>,
    }

    /// Where the config has an actor class's `container` options:
    /// `services[service_index].worker.durableObjectNamespaces[namespace_index]`.
    #[derive(Clone, Debug, PartialEq, Eq)]
    struct ContainerRef {
        service_index: u32,
        namespace_index: u32,
    }

    /// A facet's id and class, as the actor that owns the facet resolved them.
    struct FacetStartInfo {
        id: KjOwn<ActorIdHandle>,
        actor_class: Box<ActorClassChannel>,
    }

    /// A config error, or else a warning, as the in-process server (in_process.rs) reports it.
    struct ConfigReport {
        error: bool,
        message: String,
    }

    /// Where an actor keeps its data.
    struct ActorStorageSpec {
        /// Durable: keep state across requests. Ephemeral: no storage at all.
        durable: bool,
        enable_sql: bool,
        /// The key of the root actor whose storage this actor shares: its own key for a root
        /// actor, its root's for a facet.
        root_key: String,
        /// A facet's number within its root actor's storage; none for a root actor.
        facet_id: KjMaybe<u32>,
    }

    /// What a channel token is for (`IoChannelFactory::ChannelTokenUsage`): restoring a stub over
    /// RPC, or from Durable Object storage. The values are the C++ enum's.
    #[repr(u32)]
    enum TokenUsage {
        RPC,
        STORAGE,
    }

    // =====================================================================================
    // The command line (bootstrap.h): the options the C++ driver applies, and the driver itself.

    /// The options of `serve` and `test` the C++ driver applies: to the process (perfetto), or
    /// to the worker factory (experimental, Python).
    #[namespace = "workerd::server::cli"]
    struct ServeOrTestOptions {
        perfetto_trace_path: KjMaybe<String>,
        perfetto_trace_categories: KjMaybe<String>,
        experimental: bool,
        pyodide_package_disk_cache_dir: KjMaybe<String>,
        pyodide_bundle_disk_cache_dir: KjMaybe<String>,
        python_save_snapshot: bool,
        python_save_baseline_snapshot: bool,
        python_load_snapshot: KjMaybe<String>,
        python_snapshot_dir: KjMaybe<String>,
    }

    /// The options of `test` the C++ driver applies: logging, V8 modes, autogates, the
    /// compatibility date every worker gets.
    #[namespace = "workerd::server::cli"]
    struct TestOptions {
        no_verbose: bool,
        predictable: bool,
        gc_stress: bool,
        all_autogates: bool,
        compat_date: KjMaybe<String>,
    }

    #[namespace = "kj_rs_tokio"]
    unsafe extern "C++" {
        include!("kj-rs-tokio/tokio-event-port.h");

        /// The KJ event loop's context (`kj_rs_tokio::Runtime::context`): the
        /// `kj::Timer` and `kj::EventLoop` the process's C++ runs on.
        type TokioAsyncIoContext = kj_rs_tokio::TokioAsyncIoContext;
    }

    #[namespace = "workerd::server::cli"]
    unsafe extern "C++" {
        include!("workerd/server/factory/bootstrap.h");

        /// Runs `command` (`run_pending_command`) with the process's logging set up on the C++
        /// stack around it: the crash handler, info logging if `verbose`, and the JSON logger if
        /// `config` asks for structured logging (KJ requires a `kj::ExceptionCallback` to live
        /// on the stack of the thread that installs it). The command gets `config` and whether
        /// logging is structured. The command's result is the result.
        fn with_process_context(
            verbose: bool,
            config: Vec<u64>,
            command: Box<PendingCommand>,
        ) -> Result<i32>;

        /// Sets up what the process needs around the Rust server -- perfetto, autogates, V8
        /// (platform and `jsg::V8System`), a tokio-backed `kj::Network` on the loop's timer --
        /// and returns the worker factory over the config, which owns it all: dropping the
        /// factory tears the rest down after it, in reverse, flushing the perfetto trace.
        /// `config` is an encoded message (segment table, then segments) in 8-byte words.
        /// `test` is the `test` command's options, absent for `serve`. An error means the
        /// process failed to start.
        ///
        /// The factory dies before the Runtime (the timer and the loop must outlive everything
        /// on them).
        fn bootstrap(
            event_loop: Pin<&mut TokioAsyncIoContext>,
            config: Vec<u64>,
            options: &ServeOrTestOptions,
            test: KjMaybe<&TestOptions>,
        ) -> Result<KjOwn<WorkerFactory>>;

        /// Ends the process with `code` at once, without running destructors (what
        /// `kj::ProcessContext::exit()` does when `KJ_CLEAN_SHUTDOWN` is not set). Does not return.
        fn cli_exit(code: i32);

        /// A log line for KJ's logger, at a `kj::LogSeverity` (`log.rs` maps tracing
        /// levels onto it).
        fn kj_log(severity: u8, file: &str, line: u32, message: &str);

        /// One of the command line's own messages under structured logging, at a
        /// `kj::LogSeverity`: a line of the JSON logger's format on stderr, where the logger's
        /// own lines go to stdout. What supervises the process reads the reason of a failed
        /// start from stderr.
        fn json_log_to_stderr(severity: u8, file: &str, line: u32, message: &str);
    }

    // =====================================================================================
    // C++: the factory

    unsafe extern "C++" {
        include!("workerd/server/factory/worker-factory.h");

        /// Everything shared by the workers of one server: the V8 system, the HTTP header
        /// table, the capnp factories, the channel token handler, the inspector. Created by the
        /// C++ bootstrap (bootstrap.c++) for the lifetime of a run.
        type WorkerFactory;

        /// The config, as the bytes of an encoded `Config` message the factory and the server
        /// both read. Owned by the factory for the run; every reader into it stays valid.
        fn factory_config<'a>(factory: &'a WorkerFactory) -> &'a [u8];
        fn factory_experimental(factory: &WorkerFactory) -> bool;
        /// Registers the server as the resolver of channel tokens and debug-port requests.
        /// Tokens cannot be decoded before this.
        fn factory_set_server(factory: &WorkerFactory, server: Box<ServerHandle>);
        /// Adds a background task to the factory's task set; it starts on the next turn of the
        /// event loop. See `Factory::spawn`.
        fn factory_spawn(factory: &WorkerFactory, task: Box<SpawnedTask>);
        /// Drops every background task now. The server's drop does this before it unlinks its
        /// services, as a task may hold an actor and so its worker.
        fn factory_clear_tasks(factory: &WorkerFactory);
        /// Runs the tasks the server's drop spawned (and the tasks those spawn), then drops the
        /// rest. See `Factory::settle_tasks`.
        async fn factory_settle_tasks(factory: &WorkerFactory) -> Result<()>;
        /// The reading of the factory's `kj::Timer`, in nanoseconds since the timer's origin.
        fn factory_timer_now(factory: &WorkerFactory) -> u64;
        /// Completes `nanos` from now on the factory's `kj::Timer`.
        async fn factory_sleep(factory: &WorkerFactory, nanos: u64);
        /// The header table every `kj::HttpHeaders` handed to a worker is built against.
        fn factory_header_table<'a>(factory: &'a WorkerFactory) -> &'a HttpHeaderTable;

        /// Compiles a worker: its script, bindings and exports. Any config error is reported in
        /// the returned info rather than thrown; a worker with errors still exists so that the
        /// server can keep reporting the config's other errors.
        ///
        /// The code comes from the config's `services[config_service]` (a `worker` service) or,
        /// for a dynamic worker, from `dynamic_source`, which is fetched here; the capabilities
        /// in its `env` stay with the factory as the worker's own channel tables.
        async fn factory_new_worker(
            factory: &WorkerFactory,
            spec: &WorkerSpec,
            config_service: KjMaybe<u32>,
            dynamic_source: KjMaybe<KjOwn<DynamicSource>>,
        ) -> Result<KjOwn<CompiledWorker>>;

        /// A compiled worker: the isolate, script and `Worker` plus what was learned compiling
        /// them.
        type CompiledWorker;
        fn worker_info(worker: &CompiledWorker) -> WorkerInfo;
        /// Compiles `ctx.exports` from an encoded `Globals` message. Called once, after the
        /// server has numbered the worker's loopback channels.
        fn worker_set_ctx_exports(worker: &CompiledWorker, globals: &[u64]) -> Result<()>;
        /// Cancels the worker's requests still in flight (background work past the response: a
        /// `waitUntil()`, a tail worker's event), each of which holds the worker and its I/O
        /// channels; and drops a dynamic worker's env channel tables and tails, which may refer
        /// back to the worker that loaded it. Called when the server unlinks the worker.
        fn worker_unlink(worker: &CompiledWorker);
        /// Starts a request on the worker. `channels` is the worker's I/O channel table for this
        /// request; `tails` receive the request's trace once it completes (the server passes none
        /// for a request that is itself a tail worker's, so that tails do not trace themselves,
        /// and none for a dynamic worker, whose tails are its source's and the factory's own).
        /// A static worker's entrypoint mints its own self token, which restores a stub to it.
        fn worker_start_request(
            worker: &CompiledWorker,
            entrypoint: KjMaybe<&str>,
            props: KjMaybe<KjOwn<Frankenvalue>>,
            actor: KjMaybe<&ActorHandle>,
            channels: Box<ChannelFactory>,
            metadata: KjOwn<RequestMetadata>,
            tails: Box<WorkerInterfaceList>,
        ) -> Result<KjOwn<WorkerInterface>>;
        /// `inner` with `keep` attached: dropped once `inner` has been.
        fn worker_interface_attach(
            inner: KjOwn<WorkerInterface>,
            keep: Box<KeepAlive>,
        ) -> KjOwn<WorkerInterface>;
        /// Constructs an actor of `class_name` whose storage lives in `storage`. `hooks` is the
        /// actor's way back to the server; `hibernation_manager` is the evicted predecessor's,
        /// whose WebSockets the actor adopts. `container` gives the actor its container's Docker
        /// client: the one a live container with the actor's id already has, else a new one.
        fn worker_new_actor(
            worker: &CompiledWorker,
            class_name: &str,
            props: KjMaybe<KjOwn<Frankenvalue>>,
            id: KjOwn<ActorIdHandle>,
            storage: &ActorStorage,
            spec: &ActorStorageSpec,
            hooks: Box<ActorHooks>,
            hibernation_manager: KjMaybe<KjRc<HibernationManager>>,
            container: KjMaybe<&ContainerRef>,
        ) -> Result<KjOwn<ActorHandle>>;

        /// A live actor.
        type ActorHandle;
        /// An actor's `onBroken()`, which does not keep the actor alive: dropping the actor
        /// closes its storage at once, whoever still waits for this.
        type ActorBroken;
        fn actor_on_broken(actor: &ActorHandle) -> KjOwn<ActorBroken>;
        /// Resolves when the actor breaks (an uncaught error, an eviction request, a storage
        /// failure); the error is the reason.
        async fn actor_broken(broken: KjOwn<ActorBroken>) -> Result<()>;
        /// Aborts the actor with `reason`; every in-flight request fails. Without a reason the
        /// actor is shut down instead: its background work stops, and nothing is failed.
        fn actor_abort(actor: &ActorHandle, reason: &AbortReason);
        /// The isolate lock hibernating the actor's WebSockets takes, with no request to take
        /// it on behalf of.
        type ActorLock;
        async fn actor_lock(actor: &ActorHandle) -> Result<KjOwn<ActorLock>>;
        /// Shuts the actor down for eviction: hibernates its WebSockets (given `lock`, if it
        /// has a hibernation manager), then stops its background work with `reason` as the
        /// disconnect reason. Returns false without touching the actor if something still holds
        /// a strong reference to it (a request raced in); the caller retries later.
        fn actor_shutdown(
            actor: &ActorHandle,
            reason: &str,
            lock: KjMaybe<KjOwn<ActorLock>>,
        ) -> Result<bool>;
        /// Resets the actor's SQLite database while its connection is open (`deleteAllActors`).
        fn actor_reset_storage(actor: &ActorHandle);
        /// The actor's hibernation manager, to hand to the actor that replaces it after eviction.
        fn actor_hibernation_manager(actor: &ActorHandle) -> KjMaybe<KjRc<HibernationManager>>;
        type HibernationManager;

        /// A facet's start info, as the actor that owns the facet supplies it; resolving it
        /// yields the facet's id and class.
        type FacetStart;
        async fn facet_start_resolve(start: KjOwn<FacetStart>) -> Result<FacetStartInfo>;

        /// A Durable Object id: a 32-byte id for a durable namespace, or a name for an
        /// ephemeral one.
        type ActorIdHandle;
        fn actor_id_clone(id: &ActorIdHandle) -> KjOwn<ActorIdHandle>;
        /// The key the server files the actor under: the hex id, or the name.
        fn actor_id_key(id: &ActorIdHandle) -> String;
        fn actor_id_from_name(name: &str) -> KjOwn<ActorIdHandle>;
        /// The id of a durable namespace, from its hex text (a debug-port request).
        fn actor_id_from_hex(hex: &str) -> Result<KjOwn<ActorIdHandle>>;

        /// The storage of one actor namespace: its directory on disk (or none for in-memory
        /// storage), the SQLite VFS over it, and its alarm scheduler.
        type ActorStorage;
        /// `path` is the namespace's directory; empty for in-memory storage. `actors` answers
        /// the scheduler's requests for an actor to run an alarm on.
        fn factory_new_actor_storage(
            factory: &WorkerFactory,
            path: &str,
            unique_key: &str,
            actors: Box<ActorNamespaceHandle>,
        ) -> Result<KjOwn<ActorStorage>>;
        /// Deletes every actor's storage and every alarm. Callers abort the actors first.
        fn actor_storage_delete_all(storage: &ActorStorage) -> Result<()>;
        /// Deletes the storage of the facet `name` of the actor numbered `parent_facet_id` (none
        /// for the root), and its descendants'. Nothing for a root actor that never had facets.
        fn actor_storage_delete_facet(
            storage: &ActorStorage,
            root_key: &str,
            parent_facet_id: KjMaybe<u32>,
            name: &str,
        ) -> Result<()>;
        /// Replaces the storage of the facet `dst` (and its descendants') with a copy of the
        /// facet `src`'s, both facets of the actor numbered `parent_facet_id`.
        fn actor_storage_clone_facet(
            storage: &ActorStorage,
            root_key: &str,
            parent_facet_id: KjMaybe<u32>,
            src: &str,
            dst: &str,
        ) -> Result<()>;
        /// The number of a named facet of the root actor `root_key`, allocating one if new.
        /// None for in-memory storage (an `ActorCache` per actor), which numbers no facets.
        fn actor_storage_facet_id(
            storage: &ActorStorage,
            root_key: &str,
            parent_facet_id: KjMaybe<u32>,
            name: &str,
        ) -> Result<KjMaybe<u32>>;

        /// Shuts down the Docker client of every actor's container and resolves once Docker has
        /// removed the containers. No actor can start a container afterwards.
        async fn factory_shutdown_containers(factory: &WorkerFactory) -> Result<()>;

        /// The source of a dynamic worker, as the worker that loads it supplies it; handed
        /// straight to `factory_new_worker`.
        type DynamicSource;

        /// Metadata of one request: the cf blob, the client address, span parents, the token
        /// factory a restored stub should use.
        type RequestMetadata;
        fn new_request_metadata(
            cf_blob_json: KjMaybe<&str>,
            client_address: KjMaybe<&str>,
        ) -> KjOwn<RequestMetadata>;
        /// The cf blob the request carries, as JSON.
        fn request_metadata_cf_blob_json(metadata: &RequestMetadata) -> KjMaybe<String>;
        /// Marks the request as coming through a stub that was, or may be, stored durably.
        fn request_metadata_set_from_persistent_stub(
            metadata: Pin<&mut RequestMetadata>,
            persistent: bool,
        );
        /// Makes stubs the request creates for the actor itself restorable: they encode the
        /// actor's namespace and id.
        fn request_metadata_set_actor_self_token(
            factory: &WorkerFactory,
            metadata: Pin<&mut RequestMetadata>,
            unique_key: &str,
            id: &ActorIdHandle,
            persistent: bool,
        );

        /// A `kj::Exception` the factory hands the server as an abort reason. `exception_throw`
        /// throws it: the `Err` is how the server reads it, with its type, description,
        /// location and details.
        #[namespace = "kj"]
        type Exception;
        fn exception_throw(exception: &Exception) -> Result<()>;
        /// The error `error` holds, the way KJ prints an exception (`kj::str`): `file:line: type:
        /// description`, the location being the bridge's if the error was made in Rust. Without
        /// a stack trace, which a `KjError` does not carry.
        fn exception_text(error: &AbortReason) -> String;

        /// A JS value that can cross the RPC boundary: the props of an entrypoint, an actor
        /// class or a dynamic worker's env.
        #[namespace = "workerd"]
        type Frankenvalue;
        fn frankenvalue_from_json(json: &str) -> KjOwn<Frankenvalue>;
        fn frankenvalue_clone(value: &Frankenvalue) -> KjOwn<Frankenvalue>;
        fn frankenvalue_is_empty(value: &Frankenvalue) -> bool;
        /// An empty object.
        fn frankenvalue_new() -> KjOwn<Frankenvalue>;
        /// Sets the property `name` to a service stub (a `Fetcher` in JS) over `channel`.
        fn frankenvalue_set_service_stub(
            value: Pin<&mut Frankenvalue>,
            name: &str,
            channel: Box<SubrequestChannel>,
        );

        /// Serves a capnp-over-HTTP-CONNECT connection: the peer's `WorkerdBootstrap` dispatches
        /// events to `target`. Resolves when the connection closes.
        async fn factory_accept_bootstrap(
            factory: &WorkerFactory,
            stream: KjOwn<AsyncIoStream>,
            target: Box<SubrequestChannel>,
        ) -> Result<()>;
        /// Serves a debug-port connection, resolving entrypoints and actors through the server
        /// (`factory_set_server`).
        async fn factory_accept_debug_port(
            factory: &WorkerFactory,
            stream: KjOwn<AsyncIoStream>,
        ) -> Result<()>;
        /// The client side of a capnp-over-HTTP-CONNECT connection to an external server.
        type RpcClient;
        fn new_rpc_client(
            factory: &WorkerFactory,
            stream: KjOwn<AsyncIoStream>,
        ) -> KjOwn<RpcClient>;
        /// Sends a custom event (a trace, a JS RPC session, a tail stream, a UDP flow) to the
        /// peer's `WorkerdBootstrap`.
        async fn rpc_client_custom_event(
            client: &RpcClient,
            event: KjOwn<CustomEvent>,
            cf_blob_json: KjMaybe<&str>,
        ) -> Result<CustomEventResult>;
        async fn rpc_client_on_disconnect(client: &RpcClient);

        /// Starts the inspector on `address` on its own thread; every isolate the factory
        /// creates afterwards registers with it. Returns the bound port.
        fn factory_start_inspector(factory: &WorkerFactory, address: &str) -> Result<u16>;

        // The listeners' shims (worker-factory-listen.c++).

        /// A copy of `headers` with `edits` and then `injected` applied in order.
        fn edit_headers(
            table: &HttpHeaderTable,
            headers: &HttpHeaders,
            edits: &[HeaderEdit],
            injected: &[HeaderEdit],
        ) -> Result<KjOwn<HttpHeaders>>;
        /// A response that applies `edits` to the headers of every `send()` and
        /// `acceptWebSocket()` before forwarding to `inner`, which it borrows for its life.
        unsafe fn new_rewriting_response<'a>(
            inner: Pin<&'a mut HttpServiceResponse>,
            table: &HttpHeaderTable,
            edits: &[HeaderEdit],
        ) -> Result<KjOwn<HttpServiceResponse>>;
        /// The `ConnectResponse` of a raw TCP socket's `connect()`: accepting does nothing,
        /// rejecting discards the body.
        fn new_null_connect_response() -> KjOwn<ConnectResponse>;
        /// `JsgifyWebSocketErrors`: the WebSocket error handler whose exceptions reach JS as
        /// `Error`s, for every server and client the Rust server runs.
        fn new_jsgify_websocket_errors() -> KjOwn<WebSocketErrorHandler>;
        /// A `connect()` event delivering one UDP flow, addressed as `address`.
        fn new_udp_connect_event(address: &str, flow: Box<UdpFlow>) -> KjOwn<CustomEvent>;

        type TokenUsage;
        /// Channel tokens (channel-token.h): what restores a channel later, over RPC or from
        /// Durable Object storage. Encoding a channel whose target is not `persistent` for
        /// storage fails with a `DataCloneError`.
        ///
        /// A token is encoded at once; its bytes are ready at once too unless a channel in the
        /// props has a token of its own that is not (see `PendingToken`). The props are borrowed
        /// for the encoding only.
        fn factory_encode_subrequest_token(
            factory: &WorkerFactory,
            service_name: &str,
            entrypoint: KjMaybe<&str>,
            props: KjMaybe<&Frankenvalue>,
            persistent: bool,
            usage: TokenUsage,
        ) -> Result<KjOwn<PendingToken>>;
        fn factory_encode_actor_class_token(
            factory: &WorkerFactory,
            service_name: &str,
            class_name: &str,
            props: KjMaybe<&Frankenvalue>,
            persistent: bool,
            usage: TokenUsage,
        ) -> Result<KjOwn<PendingToken>>;
        fn factory_encode_actor_token(
            factory: &WorkerFactory,
            unique_key: &str,
            id: &ActorIdHandle,
            persistent: bool,
            usage: TokenUsage,
        ) -> Result<KjOwn<PendingToken>>;
        /// A channel token as `ChannelTokenHandler` encodes it: bytes that are ready at once,
        /// or a promise of them. `SubrequestChannel::getTokenMaybeSync()` hands the ready bytes
        /// out synchronously, which is what lets a stub be stored inline in Durable Object
        /// storage.
        type PendingToken;
    }

    // =====================================================================================
    // Rust: the server's channel objects
    //
    // An async function that borrows its arguments is declared `async unsafe fn f<'a>` with the
    // one lifetime on every reference: the form the bridge requires for a future that is not
    // `'static`. C++ keeps the arguments alive until the promise settles or is destroyed.

    extern "Rust" {
        /// The Rust half of a command, run by `with_process_context` (entry.rs).
        type PendingCommand;
        fn run_pending_command(
            command: Box<PendingCommand>,
            config: Vec<u64>,
            structured_logging: bool,
        ) -> Result<i32>;

        /// The server run inside a test process (in_process.rs), on a factory the test built.
        /// `run` is `workerd serve` until `drain`, with `--debug-port` if `debug_port` is not
        /// empty; `test` is `workerd test`. `next_report` is the next config error or warning.
        /// `connect` and `accept` are the two ends of a `loopback:` name.
        type InProcessServer;
        fn new_in_process_server(factory: KjOwn<WorkerFactory>) -> Box<InProcessServer>;
        async unsafe fn run<'a>(self: &'a InProcessServer, debug_port: &'a str) -> Result<()>;
        async unsafe fn test<'a>(
            self: &'a InProcessServer,
            service_pattern: &'a str,
            entrypoint_pattern: &'a str,
        ) -> Result<bool>;
        fn drain(self: &InProcessServer);
        async unsafe fn next_report<'a>(self: &'a InProcessServer) -> ConfigReport;
        fn connect(self: &InProcessServer, name: &str) -> Result<KjOwn<AsyncIoStream>>;
        async unsafe fn accept<'a>(
            self: &'a InProcessServer,
            name: &'a str,
        ) -> Result<KjOwn<AsyncIoStream>>;
        /// Ends the server's tasks and drops the factory, logging an error if a service still
        /// holds it.
        async fn close_in_process_server(server: Box<InProcessServer>) -> Result<()>;

        /// A background task of the server, run by the factory's task set (`factory_spawn`);
        /// `task_run` is the promise the task set holds.
        type SpawnedTask;
        async fn task_run(task: Box<SpawnedTask>) -> Result<()>;

        /// The server, for the factory's callbacks: resolving channel tokens and debug-port
        /// requests to channels. A weak handle: the server owns the factory, and every call
        /// fails once the server is gone.
        type ServerHandle;
        fn server_clone(self: &ServerHandle) -> Box<ServerHandle>;
        fn resolve_entrypoint(
            self: &ServerHandle,
            service_name: &str,
            entrypoint: KjMaybe<&str>,
            props: KjOwn<Frankenvalue>,
            persistent: bool,
        ) -> Result<Box<SubrequestChannel>>;
        fn resolve_actor_class(
            self: &ServerHandle,
            service_name: &str,
            class_name: KjMaybe<&str>,
            props: KjOwn<Frankenvalue>,
            persistent: bool,
        ) -> Result<Box<ActorClassChannel>>;
        fn resolve_actor(
            self: &ServerHandle,
            unique_key: &str,
            id: KjOwn<ActorIdHandle>,
            persistent: bool,
        ) -> Result<Box<SubrequestChannel>>;
        /// The debug port's `getEntrypoint`: a worker's entrypoint (the default without
        /// `entrypoint`), or a service that is not a worker, with `props` bound if given.
        fn resolve_debug_entrypoint(
            self: &ServerHandle,
            service_name: &str,
            entrypoint: KjMaybe<&str>,
            props: KjMaybe<KjOwn<Frankenvalue>>,
        ) -> Result<Box<SubrequestChannel>>;
        /// The debug port's `getActor`: an actor of `class_name` in the service, by its id (hex
        /// for a durable namespace, the name for an ephemeral one).
        fn resolve_debug_actor(
            self: &ServerHandle,
            service_name: &str,
            class_name: &str,
            actor_id: &str,
        ) -> Result<Box<SubrequestChannel>>;

        /// A worker's I/O channel table for one request: what its bindings reach.
        type ChannelFactory;
        fn subrequest_channel(
            self: &ChannelFactory,
            channel: u32,
            props: KjMaybe<KjOwn<Frankenvalue>>,
            persistent: bool,
        ) -> Result<Box<SubrequestChannel>>;
        fn global_actor(
            self: &ChannelFactory,
            channel: u32,
            id: KjOwn<ActorIdHandle>,
            persistent: bool,
        ) -> Result<Box<SubrequestChannel>>;
        fn colo_local_actor(
            self: &ChannelFactory,
            channel: u32,
            id: &str,
        ) -> Result<Box<SubrequestChannel>>;
        fn actor_class(
            self: &ChannelFactory,
            channel: u32,
            props: KjMaybe<KjOwn<Frankenvalue>>,
            persistent: bool,
        ) -> Result<Box<ActorClassChannel>>;
        /// The channel of the worker's cache API outbound, if configured.
        fn cache_channel(self: &ChannelFactory) -> Result<Box<SubrequestChannel>>;
        /// The channel of the worker's Cloudflare Access identity binding, whose props come from
        /// the request's access blob, if the worker has one.
        fn access_binding_channel(self: &ChannelFactory) -> KjMaybe<u32>;
        fn abort_all_actors(self: &ChannelFactory, reason: KjMaybe<&Exception>);
        fn delete_all_actors(self: &ChannelFactory, reason: KjMaybe<&Exception>) -> Result<()>;
        async unsafe fn evict_all_actors_for_test<'a>(
            self: &'a ChannelFactory,
            hibernate: bool,
        ) -> Result<()>;
        /// Aborts the isolate. A dynamic worker unloads; a static worker cannot be replaced, so
        /// the error is what ends the process.
        fn abort_isolate(self: &ChannelFactory, reason: &str) -> Result<()>;
        /// Loads a dynamic worker, reachable as the returned stub's entrypoints and classes.
        fn load_isolate(
            self: &ChannelFactory,
            loader_channel: u32,
            name: KjMaybe<&str>,
            source: KjOwn<DynamicSource>,
        ) -> Result<Box<WorkerStub>>;
        /// Whether this worker has the workerd debug port binding.
        fn has_debug_port(self: &ChannelFactory) -> bool;

        /// A loaded dynamic worker.
        type WorkerStub;
        fn entrypoint(
            self: &WorkerStub,
            name: KjMaybe<&str>,
            props: KjOwn<Frankenvalue>,
        ) -> Box<SubrequestChannel>;
        fn actor_class(
            self: &WorkerStub,
            name: KjMaybe<&str>,
            props: KjOwn<Frankenvalue>,
        ) -> Box<ActorClassChannel>;

        /// Something that can start a request: a worker entrypoint, an actor, an external
        /// server, the network, a directory.
        type SubrequestChannel;
        fn start_request(
            self: &SubrequestChannel,
            metadata: KjOwn<RequestMetadata>,
        ) -> Result<KjOwn<WorkerInterface>>;
        /// Whether the channel may be handed to another worker, as a stub.
        fn require_allows_transfer(self: &SubrequestChannel) -> Result<()>;
        /// The channel token that restores this channel.
        fn token(self: &SubrequestChannel, usage: TokenUsage) -> Result<KjOwn<PendingToken>>;
        async unsafe fn evict_for_test<'a>(
            self: &'a SubrequestChannel,
            hibernate: bool,
        ) -> Result<()>;

        /// A Durable Object class, from which actors are made.
        type ActorClassChannel;
        fn actor_class_channel_clone(self: &ActorClassChannel) -> Box<ActorClassChannel>;
        fn require_allows_transfer(self: &ActorClassChannel) -> Result<()>;
        fn token(self: &ActorClassChannel, usage: TokenUsage) -> Result<KjOwn<PendingToken>>;

        /// A Durable Object namespace, for its alarm scheduler.
        type ActorNamespaceHandle;
        /// The actor to run an alarm on, started if needed. The id carries the name the actor was
        /// created with, when the scheduler persisted one.
        fn actor_for_alarm(
            self: &ActorNamespaceHandle,
            id: KjOwn<ActorIdHandle>,
        ) -> Result<KjOwn<WorkerInterface>>;

        /// An actor's way back to the server: the requests it raises for itself, its facets
        /// (named child actors sharing its storage), and its transitions between idle and active.
        type ActorHooks;
        /// Starts a request the actor raises for itself: an alarm, a hibernated WebSocket's
        /// event.
        fn start_request(
            self: &ActorHooks,
            metadata: KjOwn<RequestMetadata>,
        ) -> Result<KjOwn<WorkerInterface>>;
        fn depth(self: &ActorHooks) -> u32;
        /// The channel of the named facet, starting it if needed once `start` resolves.
        fn facet(
            self: &ActorHooks,
            name: &str,
            start: KjOwn<FacetStart>,
        ) -> Result<Box<SubrequestChannel>>;
        fn abort_facet(self: &ActorHooks, name: &str, reason: &Exception);
        fn delete_facet(self: &ActorHooks, name: &str) -> Result<()>;
        fn clone_facet(self: &ActorHooks, src: &str, dst: &str) -> Result<()>;
        /// The actor's first request started.
        fn active(self: &ActorHooks);
        /// The actor's last request ended.
        fn inactive(self: &ActorHooks);

        /// The reason of an `actor_abort`. `raise` returns it as the `Err`, which C++ catches as
        /// the equivalent `kj::Exception`; `Ok` when there is none.
        type AbortReason;
        fn raise(self: &AbortReason) -> Result<()>;

        /// What a request's interface keeps alive (`worker_interface_attach`).
        type KeepAlive;

        /// One UDP flow (every datagram to and from one peer, until idle), as the
        /// `workerd::DatagramChannel` of a `UdpConnectCustomEvent`.
        type UdpFlow;
        /// The next datagram, or `ended`. One call at a time.
        async unsafe fn receive<'a>(self: &'a UdpFlow) -> Result<UdpDatagram>;
        async unsafe fn send<'a>(self: &'a UdpFlow, datagram: &'a [u8]) -> Result<()>;

        /// The `WorkerInterface`s of a request's tail workers.
        type WorkerInterfaceList;
        fn len(self: &WorkerInterfaceList) -> usize;
        fn is_streaming(self: &WorkerInterfaceList, index: usize) -> bool;
        fn take(self: &mut WorkerInterfaceList, index: usize) -> Result<KjOwn<WorkerInterface>>;
    }
}

// =====================================================================================
/// A response whose headers are edited before they reach `inner`. Borrows `inner` for its life,
/// which the type carries.
pub struct RewritingResponse<'a> {
    response: kj_rs::KjOwn<ffi::HttpServiceResponse>,
    _inner: std::marker::PhantomData<&'a mut ffi::HttpServiceResponse>,
}

impl RewritingResponse<'_> {
    pub fn as_mut(&mut self) -> std::pin::Pin<&mut ffi::HttpServiceResponse> {
        self.response.as_mut()
    }
}

/// See `ffi::new_rewriting_response`.
pub fn rewriting_response<'a>(
    inner: std::pin::Pin<&'a mut ffi::HttpServiceResponse>,
    table: &ffi::HttpHeaderTable,
    edits: &[ffi::HeaderEdit],
) -> crate::Result<RewritingResponse<'a>> {
    // SAFETY: the returned wrapper carries `inner`'s lifetime, so it cannot outlive it.
    let response = unsafe { ffi::new_rewriting_response(inner, table, edits) }?;
    Ok(RewritingResponse {
        response,
        _inner: std::marker::PhantomData,
    })
}

/// See `ffi::exception_throw`.
impl From<&ffi::Exception> for crate::Error {
    fn from(exception: &ffi::Exception) -> Self {
        match ffi::exception_throw(exception) {
            Err(error) => error.into(),
            Ok(()) => kj::failed!("exception_throw() returned"),
        }
    }
}
