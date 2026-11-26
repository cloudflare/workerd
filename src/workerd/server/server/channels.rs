// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The channel objects: what the runtime reaches into the server through.
//!
//! A worker's bindings are channel numbers. When a request uses one, the runtime asks the
//! worker's [`ChannelFactory`] for the channel behind the number and gets a [`SubrequestChannel`]
//! or an [`ActorClassChannel`]: a handle over an `Rc` of the object behind it, cheap to clone and
//! safe to hand to C++, which wraps it in the KJ interface the runtime expects
//! (worker-factory.h's `SubrequestChannelHandle`). A facet's class comes back from the runtime
//! and is unwrapped to the same `Rc` (`ActorClassChannelHandle::tryUnwrap`).
//!
//! A channel can also be a C++ one, which the server only stores and hands back: what a dynamic
//! worker's source supplies (its global outbound and the capabilities in its `env`) that is not
//! one of the server's own. C++ gets the same object back, not a wrapper of it. A C++ channel
//! that wraps one of the server's is unwrapped as it comes in, as a facet's class is.
//!
//! Every type here is single-threaded (`Rc`): channels live and die on the loop thread, as the
//! runtime's do.

use std::any::Any;
use std::rc::Rc;
use std::rc::Weak;

use futures::future::LocalBoxFuture;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::KjRc;

use crate::Result;
use crate::actor::ActorContainer;
use crate::actor::ActorNamespace;
use crate::bindings::SPECIAL_SUBREQUEST_CHANNEL_COUNT;
use crate::bindings::len_u32;
use crate::bridge::ffi;
use crate::config::Server;
use crate::worker::LinkedChannels;
use crate::worker::WorkerService;

pub type WorkerInterface = ffi::WorkerInterface;
pub type Frankenvalue = ffi::Frankenvalue;
pub type RequestMetadata = ffi::RequestMetadata;
pub type ActorIdHandle = ffi::ActorIdHandle;
pub type DynamicSource = ffi::DynamicSource;
pub type FacetStart = ffi::FacetStart;
pub type PendingToken = ffi::PendingToken;
pub type TokenUsage = ffi::TokenUsage;

/// Whether a stub for the channel may outlive the process (`Persistent` in the runtime).
pub type Persistent = bool;

/// Something that starts requests: a worker entrypoint, an actor, an external server, the
/// network, a directory. The runtime's `IoChannelFactory::SubrequestChannel`.
pub trait Channel {
    /// Starts a request; the returned interface handles exactly one event.
    fn start_request(&self, metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>>;

    /// Whether the channel may be handed to another worker as a stub. The default allows it.
    fn require_allows_transfer(&self) -> Result<()> {
        Ok(())
    }

    /// The token that restores this channel later, as the runtime encodes it: its bytes are
    /// usually ready at once, which lets a stub be stored inline in Durable Object storage.
    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>>;

    /// This channel with `props` bound. Only channels that take props support it.
    fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Rc<dyn Channel>> {
        let _ = (props, persistent);
        Err(kj::failed!("this channel does not take props"))
    }

    /// Test hook: evicts the actor behind the channel.
    fn evict_for_test(&self, hibernate: bool) -> LocalBoxFuture<'_, Result<()>> {
        let _ = hibernate;
        Box::pin(async {
            Err(kj::failed!(
                "jsg.Error: evict() can only be used on a Durable Object stub."
            ))
        })
    }

    /// The worker entrypoint behind the channel, for a worker starting its tail workers: a
    /// worker whose tail is one of its own entrypoints starts that tail as a tracer, so that the
    /// tail's own request is not tailed in turn. `None` for a channel that is not a worker
    /// entrypoint.
    fn worker_entrypoint(&self) -> Option<WorkerEntrypoint<'_>> {
        None
    }
}

/// A worker entrypoint as a [`Channel`] reports it: enough to start the same request as a tracer.
pub struct WorkerEntrypoint<'a> {
    pub worker: &'a WorkerService,
    pub entrypoint: Option<&'a str>,
    pub props: Option<&'a Frankenvalue>,
}

/// A Durable Object class, from which actors are made. The runtime's
/// `IoChannelFactory::ActorClassChannel`.
pub trait ActorClass {
    /// Resolves once the class can make actors (a dynamic worker's class waits for its worker to
    /// start).
    fn when_ready(&self) -> LocalBoxFuture<'_, Result<()>> {
        Box::pin(async { Ok(()) })
    }

    /// Constructs an actor of this class.
    fn new_actor(&self, request: NewActor<'_>) -> Result<KjOwn<ffi::ActorHandle>>;

    /// Starts a request on an actor of this class.
    fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
        actor: &ffi::ActorHandle,
    ) -> Result<KjOwn<WorkerInterface>>;

    /// Runs the captured onHibernate handler with this class's I/O channels and tails, and the
    /// self token in `metadata`.
    fn run_on_hibernate<'a>(
        &'a self,
        metadata: KjOwn<RequestMetadata>,
        actor: &'a ffi::ActorHandle,
    ) -> LocalBoxFuture<'a, Result<()>>;

    fn require_allows_transfer(&self) -> Result<()> {
        Ok(())
    }

    /// As `Channel::token`.
    fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>>;

    fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Rc<dyn ActorClass>> {
        let _ = (props, persistent);
        Err(kj::failed!("this actor class does not take props"))
    }
}

/// Everything an actor needs to be constructed.
pub struct NewActor<'a> {
    pub id: KjOwn<ActorIdHandle>,
    /// The namespace's storage, shared by its actors.
    pub storage: &'a ffi::ActorStorage,
    pub spec: ffi::ActorStorageSpec,
    pub hooks: Box<ActorHooks>,
    /// Where the config has the class's `container` options, if it has any.
    pub container: Option<&'a ffi::ContainerRef>,
    /// The hibernation manager of the actor this one replaces after an eviction, with the
    /// WebSockets it kept alive.
    pub hibernation_manager: Option<kj_rs::KjRc<ffi::HibernationManager>>,
}

// =====================================================================================
// The bridge types: the handles C++ holds. Each is a newtype or an enum so that cxx can name it;
// the methods the bridge declares on one are written beside the type it holds.

/// A subrequest channel: the server's own, or a C++ one it stores.
///
/// C++ wraps only the server's own in a `SubrequestChannelHandle`, and takes a C++ one back as
/// itself (`cxx_channel`), so the methods the handle calls serve only the server's own.
#[derive(Clone)]
pub enum SubrequestChannel {
    Rust(Rc<dyn Channel>),
    Cxx(io_channels::SubrequestChannel),
}

/// An actor class: the server's own, or a C++ one it stores, as [`SubrequestChannel`].
#[derive(Clone)]
pub enum ActorClassChannel {
    Rust(Rc<dyn ActorClass>),
    Cxx(io_channels::ActorClassChannel),
}

/// A dynamic worker's channels from its source, numbered as its `env` refers to them.
///
/// `indexDynamicChannels()` (worker-factory.c++) adds them as it rewrites the capabilities in
/// the `env` into channel numbers. The worker's `ctx.exports` is numbered after them. Empty for a
/// config worker, whose bindings the server resolves itself.
#[derive(Default)]
pub struct DynamicChannels {
    /// The source's global outbound, or a channel that refuses every request when it gave none,
    /// so that the worker may not reach the internet.
    pub global_outbound: Option<SubrequestChannel>,
    /// Numbered after the global outbound's `SPECIAL_SUBREQUEST_CHANNEL_COUNT` slots.
    pub subrequest: Vec<SubrequestChannel>,
    pub actor_class: Vec<ActorClassChannel>,
    pub rpc: Vec<io_channels::RpcChannel>,
}

/// A worker's I/O channel table: what its bindings reach. The runtime's `IoChannelFactory`, one
/// per worker, handed to every request.
pub struct ChannelFactory(pub Rc<LinkedChannels>);

/// A loaded dynamic worker. The runtime's `WorkerStubChannel`.
pub struct WorkerStub(pub Rc<crate::loader::WorkerStub>);

/// The server, as the factory's callbacks see it: it resolves channel tokens and debug-port
/// requests to channels. Weak: the server owns the factory that holds this, so it must not own
/// the server back.
pub struct ServerHandle(pub Weak<Server>);

/// A Durable Object namespace, as its alarm scheduler sees it. Weak: the scheduler lives in the
/// namespace's storage, which the namespace owns.
pub struct ActorNamespaceHandle(pub Weak<ActorNamespace>);

/// An actor's way back to its container: the requests it raises for itself (alarms, hibernated
/// WebSocket events), its facets, and its transitions between idle and active.
///
/// Weak: the actor owns this, the container the actor; and the actor can outlive its container,
/// while its last requests drain.
///
/// A request is active while it holds a reference to the actor, which outlasts its response by
/// as much as the actor's pending `waitUntil()` work, and no longer: the interface the request
/// was started through may live on with an open WebSocket.
pub struct ActorHooks(pub Weak<ActorContainer>);

/// What a request keeps alive until its interface is dropped.
pub struct KeepAlive {
    _keep: Box<dyn Any>,
}

/// `interface`, with `keep` dropped after it.
pub fn attach(interface: KjOwn<WorkerInterface>, keep: impl Any) -> KjOwn<WorkerInterface> {
    let keep = KeepAlive {
        _keep: Box::new(keep),
    };
    ffi::worker_interface_attach(interface, Box::new(keep))
}

/// A request's tail workers, started by the factory once it knows the request's tracer.
pub struct WorkerInterfaceList(pub Vec<Tail>);

pub struct Tail {
    pub streaming: bool,
    pub worker: Option<KjOwn<WorkerInterface>>,
}

impl SubrequestChannel {
    #[must_use]
    pub fn new(channel: Rc<dyn Channel>) -> Box<Self> {
        Box::new(Self::Rust(channel))
    }
    #[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
    pub(crate) fn subrequest_channel_clone(&self) -> Box<Self> {
        Box::new(self.clone())
    }
    /// The C++ channel, which C++ uses as itself; none for the server's own.
    pub(crate) fn cxx_channel(&self) -> KjMaybe<KjRc<ffi::CxxSubrequestChannel>> {
        match self {
            Self::Rust(_) => KjMaybe::None,
            Self::Cxx(channel) => KjMaybe::Some(channel.as_kj().clone()),
        }
    }
    /// This channel with `props` bound. Only the server's own channels can take props.
    pub(crate) fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Self> {
        match self {
            Self::Rust(channel) => Ok(Self::Rust(channel.for_props(props, persistent)?)),
            Self::Cxx(_) => Err(kj::failed!("can't override props for this service")),
        }
    }
    fn rust(&self) -> Result<&Rc<dyn Channel>> {
        match self {
            Self::Rust(channel) => Ok(channel),
            Self::Cxx(_) => Err(kj::failed!(
                "bug: a C++ channel reached the server's handle; C++ takes it back as itself"
            )),
        }
    }
    pub(crate) fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        self.rust()?.start_request(metadata)
    }
    pub(crate) fn require_allows_transfer(&self) -> Result<()> {
        self.rust()?.require_allows_transfer()
    }
    pub(crate) fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.rust()?.token(usage)
    }
    pub(crate) async fn evict_for_test(&self, hibernate: bool) -> Result<()> {
        self.rust()?.evict_for_test(hibernate).await
    }
}

impl ActorClassChannel {
    #[must_use]
    pub fn new(class: Rc<dyn ActorClass>) -> Box<Self> {
        Box::new(Self::Rust(class))
    }
    #[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
    pub(crate) fn actor_class_channel_clone(&self) -> Box<Self> {
        Box::new(self.clone())
    }
    /// As [`SubrequestChannel::cxx_channel`].
    pub(crate) fn cxx_channel(&self) -> KjMaybe<KjRc<ffi::CxxActorClassChannel>> {
        match self {
            Self::Rust(_) => KjMaybe::None,
            Self::Cxx(class) => KjMaybe::Some(class.as_kj().clone()),
        }
    }
    /// As [`SubrequestChannel::for_props`].
    pub(crate) fn for_props(
        &self,
        props: KjOwn<Frankenvalue>,
        persistent: Persistent,
    ) -> Result<Self> {
        match self {
            Self::Rust(class) => Ok(Self::Rust(class.for_props(props, persistent)?)),
            Self::Cxx(_) => Err(kj::failed!("can't override props for this actor class")),
        }
    }
    /// The server's own class, which constructs actors; an error for a C++ one.
    pub(crate) fn rust(&self) -> Result<&Rc<dyn ActorClass>> {
        match self {
            Self::Rust(class) => Ok(class),
            Self::Cxx(_) => Err(kj::failed!(
                "bug: a C++ actor class reached the server's handle; C++ takes it back as itself"
            )),
        }
    }
    pub(crate) fn require_allows_transfer(&self) -> Result<()> {
        self.rust()?.require_allows_transfer()
    }
    pub(crate) fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.rust()?.token(usage)
    }
}

/// A C++ channel, as C++ hands it to the server to store (worker-factory.c++'s
/// `subrequestChannelFromKj()`).
#[must_use]
pub fn subrequest_channel_from_cxx(
    channel: KjRc<ffi::CxxSubrequestChannel>,
) -> Box<SubrequestChannel> {
    Box::new(SubrequestChannel::Cxx(
        io_channels::SubrequestChannel::from_kj(channel),
    ))
}

/// As [`subrequest_channel_from_cxx`], for an actor class.
#[must_use]
pub fn actor_class_channel_from_cxx(
    channel: KjRc<ffi::CxxActorClassChannel>,
) -> Box<ActorClassChannel> {
    Box::new(ActorClassChannel::Cxx(
        io_channels::ActorClassChannel::from_kj(channel),
    ))
}

#[expect(
    clippy::boxed_local,
    reason = "cxx passes a Rust type by value in a Box"
)]
impl DynamicChannels {
    pub(crate) fn set_global_outbound(&mut self, channel: Box<SubrequestChannel>) {
        self.global_outbound = Some(*channel);
    }
    /// Adds a channel of the `env`, returning its number.
    pub(crate) fn add_subrequest(&mut self, channel: Box<SubrequestChannel>) -> u32 {
        self.subrequest.push(*channel);
        len_u32(&self.subrequest) - 1 + SPECIAL_SUBREQUEST_CHANNEL_COUNT
    }
    pub(crate) fn add_actor_class(&mut self, channel: Box<ActorClassChannel>) -> u32 {
        self.actor_class.push(*channel);
        len_u32(&self.actor_class) - 1
    }
    pub(crate) fn add_rpc(&mut self, channel: KjRc<ffi::CxxRpcChannel>) -> u32 {
        self.rpc.push(io_channels::RpcChannel::from_kj(channel));
        len_u32(&self.rpc) - 1
    }
}

impl ServerHandle {
    #[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
    pub(crate) fn server_clone(&self) -> Box<Self> {
        Box::new(Self(Weak::clone(&self.0)))
    }
    pub(crate) fn server(&self) -> Result<Rc<Server>> {
        self.0
            .upgrade()
            .ok_or_else(|| kj::disconnected!("the server is shutting down"))
    }
}

impl WorkerInterfaceList {
    #[must_use]
    pub fn new(tails: Vec<Tail>) -> Box<Self> {
        Box::new(Self(tails))
    }
    pub(crate) fn len(&self) -> usize {
        self.0.len()
    }
    pub(crate) fn is_streaming(&self, index: usize) -> bool {
        self.0.get(index).is_some_and(|tail| tail.streaming)
    }
    pub(crate) fn take(&mut self, index: usize) -> Result<KjOwn<WorkerInterface>> {
        self.0
            .get_mut(index)
            .and_then(|tail| tail.worker.take())
            .ok_or_else(|| kj::failed!("tail worker {index} already taken"))
    }
}
