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
//! Every type here is single-threaded (`Rc`): channels live and die on the loop thread, as the
//! runtime's do.

use std::any::Any;
use std::rc::Rc;
use std::rc::Weak;

use futures::future::LocalBoxFuture;
use kj_rs::KjOwn;

use crate::Result;
use crate::actor::ActorContainer;
use crate::actor::ActorNamespace;
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
// The bridge types: the handles C++ holds. Each is a newtype so that cxx can name it; the
// methods the bridge declares on one are written beside the type it holds.

pub struct SubrequestChannel(pub Rc<dyn Channel>);
pub struct ActorClassChannel(pub Rc<dyn ActorClass>);

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

/// The reason an actor is aborted for (`actor_abort`); none when it is only shut down.
///
/// `raise` returns the reason as the `Err`, which the bridge throws as the equivalent
/// `kj::Exception`.
pub struct AbortReason(pub Option<crate::Error>);

impl AbortReason {
    pub(crate) fn raise(&self) -> Result<()> {
        self.0.clone().map_or(Ok(()), Err)
    }
}

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
        Box::new(Self(channel))
    }
    pub(crate) fn start_request(
        &self,
        metadata: KjOwn<RequestMetadata>,
    ) -> Result<KjOwn<WorkerInterface>> {
        self.0.start_request(metadata)
    }
    pub(crate) fn require_allows_transfer(&self) -> Result<()> {
        self.0.require_allows_transfer()
    }
    pub(crate) fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.0.token(usage)
    }
    pub(crate) async fn evict_for_test(&self, hibernate: bool) -> Result<()> {
        self.0.evict_for_test(hibernate).await
    }
}

impl ActorClassChannel {
    #[must_use]
    pub fn new(class: Rc<dyn ActorClass>) -> Box<Self> {
        Box::new(Self(class))
    }
    #[expect(clippy::unnecessary_box_returns, reason = "cxx requires a Box")]
    pub(crate) fn actor_class_channel_clone(&self) -> Box<Self> {
        Box::new(Self(Rc::clone(&self.0)))
    }
    pub(crate) fn require_allows_transfer(&self) -> Result<()> {
        self.0.require_allows_transfer()
    }
    pub(crate) fn token(&self, usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        self.0.token(usage)
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
