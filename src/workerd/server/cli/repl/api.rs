// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The JavaScript API of `workerd repl`: the `workerd` global ([`ReplControl`]), the handle it
//! returns for each config it loads ([`ReplServer`]), and the handle of each environment it
//! starts ([`ReplEnvironmentHandle`]).
//!
//! Only the REPL's environments have a `workerd` global through which to reach them. They are not
//! part of the Workers API and have no TypeScript types.
//!
//! A handle holds what it stands for through its environment's `IoContext` ([`IoOwnedLocal`]), not
//! by itself: a handle can be the last to hold a server or an environment, and a
//! garbage-collected JavaScript object must not take KJ I/O apart. Closing a server or an
//! environment, not the last handle's garbage collection, ends it. What starts, reloads or closes
//! isolates runs as I/O, outside the lock of the REPL's isolate.

// `#[jsg_struct]` binds each field to a local of the same name in the code it generates.
#![expect(
    non_snake_case,
    reason = "the fields of the option structs are the property names JavaScript uses"
)]

use std::rc::Rc;

use channel_interop::Fetcher;
use channel_interop::ScopedChannel;
use cxx::KjError;
use cxx::KjException;
use frankenvalue::Frankenvalue;
use io_context::EvictWebSocketMode;
use io_context::IoContext;
use io_context::IoOwnedLocal;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;
use workerd_server::channels::SubrequestChannel;

use super::bridge::ffi;
use super::environment::EnvironmentState;
use super::environment::Nested;
use super::server::ReplConfigServer;
use super::server::Watching;

/// The `workerd` global of a REPL environment.
#[jsg_resource]
pub struct ReplControl {
    state: IoOwnedLocal<Rc<EnvironmentState>>,
    version: String,
    /// Every server `loadConfig()` returned and not yet seen closed. Closed ones are dropped as
    /// `servers` finds them.
    servers: jsg::RefCell<Vec<jsg::Rc<ReplServer>>>,
}

#[jsg_struct]
pub struct LoadConfigOptions {
    /// The constant that holds a Cap'n Proto config, as for `workerd serve`.
    pub constName: Option<String>,
    /// Reload the config whenever a file it was loaded from changes.
    pub watch: Option<bool>,
}

#[jsg_struct]
pub struct EnvironmentOptions {
    /// Today's if none.
    pub compatibilityDate: Option<String>,
    pub compatibilityFlags: Option<Vec<String>>,
}

/// The options of `evict()`, as `cloudflare:unsafe`'s `evict()` takes them.
#[jsg_struct(ts_override = r#"{ webSockets?: "hibernate" | "close"; }"#)]
pub struct EvictOptions {
    /// What happens to the object's WebSockets: `"hibernate"`, the default, or `"close"`.
    pub webSockets: Option<String>,
}

#[jsg_resource]
impl ReplControl {
    /// `loadConfig(path, options)`: loads the config file `path` (a Cap'n Proto config, or a
    /// `wrangler.json` / `wrangler.jsonc`, relative to the working directory) and starts serving
    /// it; this config's server. Rejects with an error listing the problems if the config does not
    /// load or has errors.
    #[jsg_method]
    pub fn load_config<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
        path: String,
        options: Option<LoadConfigOptions>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let state = this.get(lock).state(lock)?;
        let (const_name, watch) = match options {
            Some(options) => (options.constName, options.watch.unwrap_or(false)),
            None => (None, false),
        };
        let watch = if watch {
            Watching::Watched
        } else {
            Watching::Unwatched
        };
        let loaded = state.load_config(path, const_name, watch);
        pinned_thread_io::await_io(&context, lock, loaded, move |lock, loaded| {
            let server = loaded.map_err(js_error)?;
            let handle = ReplServer::new(lock, server)?;
            let held = handle.clone(lock);
            this.get(lock).servers.borrow_mut(lock).push(held);
            handle.to_js(lock)
        })
    }

    /// `environment(options)`: starts an environment like the REPL's, in an isolate of its own,
    /// with the compatibility settings `options` gives rather than the REPL's. Rejects with an
    /// error naming the problems if the settings are not valid.
    #[jsg_method]
    pub fn environment<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        options: Option<EnvironmentOptions>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let (compat_date, compat_flags) = match options {
            Some(options) => (
                options.compatibilityDate,
                options.compatibilityFlags.unwrap_or_default(),
            ),
            None => (None, Vec::new()),
        };
        let started = self
            .state(lock)?
            .start_environment(compat_date, compat_flags);
        pinned_thread_io::await_io(&context, lock, started, |lock, started| {
            let environment = started.map_err(js_error)?;
            ReplEnvironmentHandle::new(lock, environment)?.to_js(lock)
        })
    }

    /// `evict(stub, options)`: evicts the Durable Object behind `stub` (from a server's
    /// `durableObject()`) as `cloudflare:unsafe`'s `evict()` does, and resolves once it is gone.
    #[jsg_method]
    pub fn evict<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        stub: jsg_cpp::Ref<Fetcher>,
        options: Option<EvictOptions>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let channel = stub.get(lock).subrequest_channel(lock, &context)?;
        let eviction = web_socket_mode(options).map(|mode| channel.evict_for_test(mode));
        pinned_thread_io::await_io(
            &context,
            lock,
            async move {
                match eviction {
                    Ok(evicted) => evicted.await.map_err(jsg::Error::from),
                    Err(error) => Err(error),
                }
            },
            |lock, evicted| {
                evicted?;
                Ok(lock.undefined())
            },
        )
    }

    /// The servers loaded in this environment and not yet closed.
    #[jsg_property(prototype, readonly)]
    pub fn get_servers(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<Vec<jsg::Rc<ReplServer>>> {
        let servers: Vec<_> = self
            .servers
            .borrow()
            .iter()
            .map(|server| server.clone(lock))
            .collect();
        let mut open = Vec::with_capacity(servers.len());
        for server in servers {
            if !server.get(lock).config_server(lock)?.is_closed() {
                open.push(server);
            }
        }
        let held = open.iter().map(|server| server.clone(lock)).collect();
        *self.servers.borrow_mut(lock) = held;
        Ok(open)
    }

    /// The release of workerd running the REPL.
    #[jsg_property(prototype, readonly)]
    pub fn get_version(&self) -> String {
        self.version.clone()
    }
}

impl ReplControl {
    /// The `workerd` global of the environment `state` describes, whose request `context`
    /// belongs to.
    pub fn new(context: &IoContext<'_>, state: Rc<EnvironmentState>) -> jsg::Result<Self> {
        Ok(Self {
            state: pinned_thread_io::add_object(context, state)?,
            version: crate::release_version().to_owned(),
            servers: jsg::RefCell::new(Vec::new()),
        })
    }

    fn state(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<Rc<EnvironmentState>> {
        self.state
            .with(lock, |state| Rc::clone(state))?
            .ok_or_else(|| jsg::Error::new_internal_error("the REPL's environment is gone"))
    }
}

/// A config the REPL serves: `await workerd.loadConfig(path)`.
#[jsg_resource]
pub struct ReplServer {
    server: IoOwnedLocal<ReplConfigServer>,
    /// The config file, as the user named it.
    path: String,
}

#[jsg_struct]
pub struct ServiceOptions {
    /// The service's entrypoint; its default one if none.
    pub entrypoint: Option<String>,
    /// The entrypoint's `ctx.props`.
    pub props: Option<v8::TracedGlobal<v8::Object>>,
}

/// Names a Durable Object by one of its name, as `idFromName()` takes it, and its id, as
/// `idFromString()` does.
#[jsg_struct]
pub struct DurableObjectName {
    pub name: Option<String>,
    pub id: Option<String>,
}

/// A name, or a [`DurableObjectName`]. An object is never taken for a name: every object is a
/// `DurableObjectName`.
#[jsg_oneof]
pub enum DurableObjectKey {
    Name(String),
    Names(DurableObjectName),
}

#[jsg_resource]
impl ReplServer {
    /// The config file, as the user named it.
    #[jsg_property(prototype, readonly)]
    pub fn get_path(&self) -> String {
        self.path.clone()
    }

    /// Whether closing has begun.
    #[jsg_property(prototype, readonly)]
    pub fn get_closed(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<bool> {
        Ok(self.config_server(lock)?.is_closed())
    }

    /// Whether a change to a file the config was loaded from reloads it.
    #[jsg_property(prototype, readonly)]
    pub fn get_watching(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<bool> {
        Ok(self.config_server(lock)?.is_watching())
    }

    /// The warnings the server reported as it loaded the config, or last reloaded it.
    #[jsg_property(prototype, readonly)]
    pub fn get_warnings(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<Vec<String>> {
        Ok(self.config_server(lock)?.warnings())
    }

    /// `service(name, options)`: a Fetcher for the config's service `name`, for `fetch()` and RPC.
    #[jsg_method]
    pub fn service(
        &self,
        lock: &mut jsg::Lock<'_>,
        name: String,
        options: Option<ServiceOptions>,
    ) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
        let (entrypoint, props) = match options {
            Some(options) => (options.entrypoint, options.props),
            None => (None, None),
        };
        let props = match props {
            Some(props) => {
                let props = props.as_local(lock);
                Some(Frankenvalue::from_js(lock, props.into())?)
            }
            None => None,
        };
        let channel = self
            .config_server(lock)?
            .service(&name, entrypoint.as_deref(), props)
            .map_err(js_error)?;
        fetcher(lock, channel)
    }

    /// `socket(name)`: a Fetcher whose `fetch()` sends the request to the config's socket `name`.
    /// Sockets listen on in-process names rather than on their addresses.
    #[jsg_method]
    pub fn socket(
        &self,
        lock: &mut jsg::Lock<'_>,
        name: String,
    ) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
        let channel = self.config_server(lock)?.socket(&name).map_err(js_error)?;
        fetcher(lock, channel)
    }

    /// `durableObject(service, className, key)`: a stub, for RPC and `fetch()`, of the Durable
    /// Object of class `className` that the config's service `service` exports, named by `key`.
    #[jsg_method]
    pub fn durable_object(
        &self,
        lock: &mut jsg::Lock<'_>,
        service: String,
        class_name: String,
        key: DurableObjectKey,
    ) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
        let (name, id) = match key {
            DurableObjectKey::Name(name) => (Some(name), None),
            DurableObjectKey::Names(names) => (names.name, names.id),
        };
        let channel = self
            .config_server(lock)?
            .durable_object(&service, &class_name, name.as_deref(), id.as_deref())
            .map_err(js_error)?;
        fetcher(lock, channel)
    }

    /// `reload()`: loads the config again in place of what was loaded before; this handle, once it
    /// has. Rejects with an error listing the problems, leaving the server as it was, if the
    /// config does not load, or with one if the server is closed.
    #[jsg_method]
    pub fn reload<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let reloaded = this.get(lock).config_server(lock)?.reload();
        pinned_thread_io::await_io(&context, lock, reloaded, move |lock, reloaded| {
            reloaded.map_err(js_error)?;
            this.to_js(lock)
        })
    }

    /// `close()`: drains the server and takes it apart. Idempotent.
    #[jsg_method]
    pub fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let closed = self.config_server(lock)?.close();
        await_done(lock, &context, closed)
    }
}

impl ReplServer {
    pub(super) fn new(
        lock: &mut jsg::Lock<'_>,
        server: ReplConfigServer,
    ) -> jsg::Result<jsg::Rc<Self>> {
        let context = IoContext::current(lock)?;
        let path = server.path().to_owned();
        let server = pinned_thread_io::add_object(&context, server)?;
        Ok(jsg::Rc::new(lock, Self { server, path }))
    }

    /// The server, a handle itself.
    fn config_server(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<ReplConfigServer> {
        self.server
            .with(lock, |server| server.clone())?
            .ok_or_else(|| jsg::Error::new_internal_error("the REPL server's handle is gone"))
    }
}

/// An environment the REPL started: `await workerd.environment({ compatibilityDate })`.
#[jsg_resource]
pub struct ReplEnvironmentHandle {
    environment: IoOwnedLocal<Rc<Nested>>,
    compat_date: String,
    compat_flags: Vec<String>,
}

#[jsg_resource]
impl ReplEnvironmentHandle {
    /// The compatibility date the environment has, today's if none was given.
    #[jsg_property(prototype, readonly)]
    pub fn get_compatibility_date(&self) -> String {
        self.compat_date.clone()
    }

    /// The compatibility flags it was given.
    #[jsg_property(prototype, readonly)]
    pub fn get_compatibility_flags(&self) -> Vec<String> {
        self.compat_flags.clone()
    }

    #[jsg_property(prototype, readonly)]
    pub fn get_closed(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<bool> {
        Ok(self.nested(lock)?.is_closed())
    }

    /// `eval(input)`: what the REPL would print for `input` typed at it, evaluated in this
    /// environment. Throws if the environment is closed, and rejects if it closes meanwhile.
    #[jsg_method]
    pub fn eval<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        input: String,
    ) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let evaluation = self.nested(lock)?.evaluate(&input).map_err(js_error)?;
        pinned_thread_io::await_io(&context, lock, evaluation, |lock, text| {
            text.map_err(js_error)?.as_str().to_js(lock)
        })
    }

    /// `close()`: closes the environment's servers and ends its JavaScript. Idempotent.
    #[jsg_method]
    pub fn close<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Promise>> {
        let context = IoContext::current(lock)?;
        let closed = self.nested(lock)?.close();
        await_done(lock, &context, closed)
    }
}

impl ReplEnvironmentHandle {
    pub(super) fn new(
        lock: &mut jsg::Lock<'_>,
        environment: Rc<Nested>,
    ) -> jsg::Result<jsg::Rc<Self>> {
        let context = IoContext::current(lock)?;
        let compat_date = environment.compat_date().to_owned();
        let compat_flags = environment.compat_flags().to_vec();
        let environment = pinned_thread_io::add_object(&context, environment)?;
        Ok(jsg::Rc::new(
            lock,
            Self {
                environment,
                compat_date,
                compat_flags,
            },
        ))
    }

    fn nested(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<Rc<Nested>> {
        self.environment
            .with(lock, |environment| Rc::clone(environment))?
            .ok_or_else(|| jsg::Error::new_internal_error("the REPL environment's handle is gone"))
    }
}

/// A Fetcher, bound to the current `IoContext`, for a channel of a loaded config's server.
fn fetcher(
    lock: &mut jsg::Lock<'_>,
    channel: Box<SubrequestChannel>,
) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
    let context = IoContext::current(lock)?;
    let channel = ffi::repl_channel_into_kj(channel);
    Fetcher::from_channel(lock, &context, ScopedChannel::new(&context, channel))
}

/// A promise for `done`, resolved with `undefined`.
fn await_done<'s>(
    lock: &mut jsg::Lock<'s>,
    context: &IoContext<'s>,
    done: impl Future<Output = Result<(), KjError>> + 'static,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    pinned_thread_io::await_io(context, lock, done, |lock, done| {
        done.map_err(js_error)?;
        Ok(lock.undefined())
    })
}

/// The JavaScript error of a failure, as C++ JSG converts a KJ exception: a tunneled JSG error
/// becomes that error.
fn js_error(error: KjError) -> jsg::Error {
    jsg::Error::from(KjException::from(error))
}

/// The WebSocket mode that `options` ask for.
fn web_socket_mode(options: Option<EvictOptions>) -> jsg::Result<EvictWebSocketMode> {
    match options.and_then(|options| options.webSockets).as_deref() {
        None | Some("hibernate") => Ok(EvictWebSocketMode::HIBERNATE),
        Some("close") => Ok(EvictWebSocketMode::CLOSE),
        Some(_) => Err(jsg::Error::new_type_error(
            "options.webSockets must be \"hibernate\" or \"close\".",
        )),
    }
}
