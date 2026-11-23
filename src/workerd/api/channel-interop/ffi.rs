// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(
    unsafe_code,
    reason = "the cxx bridge expands to unsafe FFI glue, the shims take C++'s raw isolate, and \
              `CppResource` is an unsafe trait"
)]

//! The bridge to `ffi.h`, and the Rust API over it.

use std::marker::PhantomData;

use io_context::IoContext;
use jsg::v8;
use kj_rs::KjMaybe;
use kj_rs::KjOwn;
use kj_rs::KjRc;

// Every function takes the isolate of the caller's lock, locked by this thread with a context
// entered and a `HandleScope` open, and handles in that scope.
#[cxx::bridge(namespace = "workerd::api::channel_interop")]
pub mod bridge {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    #[namespace = "workerd::rust::jsg_cpp"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg-cpp/jsg-cpp.h");
        type CppRef = jsg_cpp::CppRef;
    }

    #[namespace = "workerd::api"]
    unsafe extern "C++" {
        include!("workerd/api/channel-interop/ffi.h");

        /// C++ `api::Fetcher` (`workerd/api/http.h`), the JavaScript type of a service binding
        /// and of anything else that requests can be sent to. Rust holds one as a
        /// `jsg_cpp::Ref<Fetcher>`.
        type Fetcher;

        /// C++ `api::DurableObjectClass` (`workerd/api/actor.h`), a handle on a Durable Object
        /// class, such as `ctx.facets.get()` takes. Rust holds one as a
        /// `jsg_cpp::Ref<DurableObjectClass>`.
        type DurableObjectClass;
    }

    /// C++ `Fetcher::RequiresHostAndProtocol`, by its alias in `ffi.h`.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(i32)]
    enum RequiresHostAndProtocol {
        YES,
        NO,
    }

    /// C++ `InHouse` (`ffi.h`).
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    #[repr(i32)]
    enum InHouse {
        YES,
        NO,
    }

    unsafe extern "C++" {
        /// A C++ `IoChannelFactory::SubrequestChannel`: somewhere requests can be sent.
        ///
        /// C++ documents a channel as an I/O object, valid only in the `IoContext` it was made
        /// in, and keeps one past its request only once `requireAllowsTransfer()` has allowed it.
        /// Rust gets one for the current request as a [`ScopedChannel`](super::ScopedChannel),
        /// which can't outlive the lock it was obtained with, and keeps one past its request as
        /// the `KjRc<SubrequestChannel>` that
        /// [`ScopedChannel::require_allows_transfer`](super::ScopedChannel::require_allows_transfer)
        /// returns. A `KjRc` is a `kj::Rc`, whose clones share its count. Like the C++ channel,
        /// it belongs to the thread it was made on, so it is neither `Send` nor `Sync`, and can't
        /// be held by a resource; it may be dropped with or without the isolate lock, and after
        /// its `IoContext` is gone.
        type SubrequestChannel;

        /// The same, for a C++ `IoChannelFactory::ActorClassChannel`: a Durable Object class.
        type ActorClassChannel;

        type RequiresHostAndProtocol;
        type InHouse;

        // Per-type shims.
        unsafe fn fetcher_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn fetcher_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;
        unsafe fn durable_object_class_try_unwrap(
            isolate: *mut Isolate,
            value: Local,
        ) -> Result<KjMaybe<KjOwn<CppRef>>>;
        unsafe fn durable_object_class_wrap(isolate: *mut Isolate, cpp_ref: &CppRef) -> MaybeLocal;

        // Per-method shims. Each returns false with an exception pending if the C++ threw.
        unsafe fn fetcher_subrequest_channel(
            isolate: *mut Isolate,
            fetcher: &Fetcher,
            io_context_id: u64,
            result: &mut KjMaybe<KjRc<SubrequestChannel>>,
        ) -> bool;
        unsafe fn fetcher_new(
            isolate: *mut Isolate,
            io_context_id: u64,
            channel: KjRc<SubrequestChannel>,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        unsafe fn fetcher_new_for_channel_number(
            isolate: *mut Isolate,
            channel: u32,
            requires_host_and_protocol: RequiresHostAndProtocol,
            in_house: InHouse,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        unsafe fn fetcher_map_virtual_host(
            isolate: *mut Isolate,
            fetcher: &Fetcher,
            port: u16,
            result: &mut String,
        ) -> bool;
        unsafe fn subrequest_channel_require_allows_transfer(
            isolate: *mut Isolate,
            channel: &SubrequestChannel,
        ) -> bool;
        unsafe fn durable_object_class_channel(
            isolate: *mut Isolate,
            durable_object_class: &DurableObjectClass,
            io_context_id: u64,
            result: &mut KjMaybe<KjRc<ActorClassChannel>>,
        ) -> bool;
        unsafe fn durable_object_class_new(
            isolate: *mut Isolate,
            io_context_id: u64,
            channel: KjRc<ActorClassChannel>,
            result: &mut KjMaybe<KjOwn<CppRef>>,
        ) -> bool;
        unsafe fn actor_class_channel_require_allows_transfer(
            isolate: *mut Isolate,
            channel: &ActorClassChannel,
        ) -> bool;
    }
}

use bridge::ActorClassChannel;
use bridge::DurableObjectClass;
use bridge::Fetcher;
use bridge::SubrequestChannel;

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with api::Fetcher.
unsafe impl jsg_cpp::CppResource for Fetcher {
    fn class_name() -> &'static str {
        "Fetcher"
    }
    fn fully_qualified_name() -> &'static str {
        "workerd::api::Fetcher"
    }
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::fetcher_try_unwrap(isolate, value) }
    }
    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::fetcher_wrap(isolate, cpp_ref) }
    }
}

// SAFETY: the shims instantiate cppRefTryUnwrap and cppRefWrap with api::DurableObjectClass.
unsafe impl jsg_cpp::CppResource for DurableObjectClass {
    fn class_name() -> &'static str {
        "DurableObjectClass"
    }
    fn fully_qualified_name() -> &'static str {
        "workerd::api::DurableObjectClass"
    }
    unsafe fn try_unwrap(
        isolate: *mut v8::ffi::Isolate,
        value: v8::ffi::Local,
    ) -> Result<KjMaybe<KjOwn<jsg_cpp::CppRef>>, cxx::KjException> {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::durable_object_class_try_unwrap(isolate, value) }
    }
    unsafe fn wrap(
        isolate: *mut v8::ffi::Isolate,
        cpp_ref: &jsg_cpp::CppRef,
    ) -> v8::ffi::MaybeLocal {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::durable_object_class_wrap(isolate, cpp_ref) }
    }
}

/// Whether a [`Fetcher`] requires URLs to have a protocol and host, as C++
/// `Fetcher::RequiresHostAndProtocol` says.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequiresHostAndProtocol {
    /// URLs must be absolute, as for a service binding.
    Yes,
    /// URLs may lack a protocol and host, which default to `https://fake-host/`, as for the
    /// platform's own APIs, such as KV.
    No,
}

/// Whether a [`Fetcher`] reaches an API that the Workers platform provides, such as KV, as C++
/// `isInHouse` says.
///
/// In-house requests are not counted as subrequests in metrics, are not subject to the
/// subrequest limit, and in preview don't show up in the network tab (see
/// `IoContext::getSubrequestChannel()`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InHouse {
    /// An API of the platform.
    Yes,
    /// Anything else, such as a service binding.
    No,
}

impl RequiresHostAndProtocol {
    const fn into_ffi(self) -> bridge::RequiresHostAndProtocol {
        match self {
            Self::Yes => bridge::RequiresHostAndProtocol::YES,
            Self::No => bridge::RequiresHostAndProtocol::NO,
        }
    }
}

impl InHouse {
    const fn into_ffi(self) -> bridge::InHouse {
        match self {
            Self::Yes => bridge::InHouse::YES,
            Self::No => bridge::InHouse::NO,
        }
    }
}

/// A channel the code of the current request may use, as C++ uses the `kj::Rc` that
/// `Fetcher::getSubrequestChannel(ioctx)` or `DurableObjectClass::getChannel(ioctx)` returns.
///
/// A channel is an I/O object, valid only in the `IoContext` it was made in, so this can't
/// outlive the [`jsg::Lock`] it was obtained with, as a handle can't: it can't be returned from
/// the callback, kept in a `thread_local!` or a resource, or moved into a future. Code that keeps
/// a channel past its request, as Worker Loader keeps a dynamic Worker's global outbound, takes
/// it with [`require_allows_transfer`](Self::require_allows_transfer), as C++ does. Code that
/// keeps the `Fetcher` or `DurableObjectClass` instead keeps the channel only as C++ does, bound
/// to its `IoContext`.
pub struct ScopedChannel<'s, C: Channel> {
    channel: KjRc<C>,
    _lock: PhantomData<(&'s (), *const ())>,
}

impl<'s, C: Channel> ScopedChannel<'s, C> {
    /// `channel`, for the request of `io_context`: one that may be kept past its request, which
    /// [`require_allows_transfer`](Self::require_allows_transfer) returned, or one that C++ made
    /// for this request, such as a channel to a Worker Loader's dynamic Worker.
    #[must_use]
    pub const fn new(_io_context: &IoContext<'s>, channel: KjRc<C>) -> Self {
        Self {
            channel,
            _lock: PhantomData,
        }
    }

    /// C++ `requireAllowsTransfer()`: the channel, for keeping past its request, if it may be
    /// passed to another Worker, as a dynamic Worker's `globalOutbound` or tail Worker is, or a
    /// `DurableObjectClass` is by C++ `DurableObjectClass::serialize()`.
    ///
    /// # Errors
    /// Fails with the error C++ throws for a channel that can't be transferred.
    pub fn require_allows_transfer(&self, lock: &mut jsg::Lock<'_>) -> jsg::Result<KjRc<C>> {
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            C::require_allows_transfer(isolate, &self.channel)
        })?;
        Ok(self.channel.clone())
    }

    fn into_inner(self) -> KjRc<C> {
        self.channel
    }
}

impl<C: Channel> Clone for ScopedChannel<'_, C> {
    fn clone(&self) -> Self {
        Self {
            channel: self.channel.clone(),
            _lock: PhantomData,
        }
    }
}

mod sealed {
    pub trait Sealed {}
    impl Sealed for super::SubrequestChannel {}
    impl Sealed for super::ActorClassChannel {}
}

/// [`SubrequestChannel`] or [`ActorClassChannel`]: what a [`ScopedChannel`] holds.
pub trait Channel: sealed::Sealed + Sized {
    /// The shim of `requireAllowsTransfer()`.
    ///
    /// # Safety
    /// As for every shim; see the bridge.
    #[doc(hidden)]
    unsafe fn require_allows_transfer(isolate: *mut v8::ffi::Isolate, channel: &Self) -> bool;
}

impl Channel for SubrequestChannel {
    unsafe fn require_allows_transfer(isolate: *mut v8::ffi::Isolate, channel: &Self) -> bool {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::subrequest_channel_require_allows_transfer(isolate, channel) }
    }
}

impl Channel for ActorClassChannel {
    unsafe fn require_allows_transfer(isolate: *mut v8::ffi::Isolate, channel: &Self) -> bool {
        // SAFETY: forwarded from the caller.
        unsafe { bridge::actor_class_channel_require_allows_transfer(isolate, channel) }
    }
}

impl Fetcher {
    /// The channel requests to this `Fetcher` go to, as C++ `getSubrequestChannel(ioctx)` returns
    /// it, such as to hand it to another Worker (see [`ScopedChannel::require_allows_transfer`])
    /// or to make another `Fetcher` with [`Fetcher::from_channel`].
    ///
    /// # Errors
    /// Fails with what C++ throws, such as for a `Fetcher` whose kind has no channel, and with an
    /// internal error unless `io_context` is the current `IoContext`.
    pub fn subrequest_channel<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
    ) -> jsg::Result<ScopedChannel<'s, SubrequestChannel>> {
        let mut slot = KjMaybe::None;
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            bridge::fetcher_subrequest_channel(isolate, self, io_context.as_ffi(), &mut slot)
        })?;
        let channel = Option::from(slot)
            .unwrap_or_else(|| unreachable!("a shim that completed sets its result"));
        Ok(ScopedChannel::new(io_context, channel))
    }

    /// A new `Fetcher` that sends requests to `channel`, bound to `io_context` as a C++
    /// `Fetcher` made with `js.alloc<Fetcher>(ioctx.addObject(channel))` is: it requires a host
    /// and protocol in URLs, and is not in-house.
    ///
    /// # Errors
    /// Fails with an internal error unless `io_context` is the current `IoContext`.
    pub fn from_channel<'s>(
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
        channel: ScopedChannel<'s, SubrequestChannel>,
    ) -> jsg::Result<jsg_cpp::Ref<Self>> {
        let id = io_context.as_ffi();
        let channel = channel.into_inner();
        // SAFETY: the shim instantiates callCppForRef with api::Fetcher, and `call_cpp_for_ref`
        // calls it with the lock's isolate.
        let fetcher = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::fetcher_new(isolate, id, channel, result)
            })
        }?;
        Ok(fetcher.unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }

    /// A new `Fetcher` on the numbered channel `channel` of the Worker's `IoChannelFactory`, as
    /// C++ `js.alloc<Fetcher>(channel, requiresHost, isInHouse)` makes one for a binding, such as
    /// KV's or R2's. Unlike one made [`from_channel`](Self::from_channel), it is bound to no
    /// `IoContext`: each request it sends looks the channel up in the current one.
    ///
    /// # Errors
    /// Fails only with a pending error, as while execution is terminating.
    pub fn from_channel_number(
        lock: &mut jsg::Lock<'_>,
        channel: u32,
        requires_host_and_protocol: RequiresHostAndProtocol,
        in_house: InHouse,
    ) -> jsg::Result<jsg_cpp::Ref<Self>> {
        // SAFETY: the shim instantiates callCppForRef with api::Fetcher, and `call_cpp_for_ref`
        // calls it with the lock's isolate.
        let fetcher = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::fetcher_new_for_channel_number(
                    isolate,
                    channel,
                    requires_host_and_protocol.into_ffi(),
                    in_house.into_ffi(),
                    result,
                )
            })
        }?;
        Ok(fetcher.unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }

    /// C++ `mapVirtualHost(js, port)`, as `ctx.mapVirtualHost(fetcher, port)` and the
    /// `cloudflare:workers` module's `mapVirtualHost()` call it: the `host:port` that
    /// `connect()` of `cloudflare:sockets` routes to this `Fetcher`, under a random
    /// `.workers.alt` host. The first call registers the route on the global scope; later
    /// calls return the same `host:port`, whatever their `port`.
    ///
    /// # Errors
    /// Fails with what C++ throws.
    pub fn map_virtual_host(&self, lock: &mut jsg::Lock<'_>, port: u16) -> jsg::Result<String> {
        let mut result = String::new();
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            bridge::fetcher_map_virtual_host(isolate, self, port, &mut result)
        })?;
        Ok(result)
    }
}

impl DurableObjectClass {
    /// The class's channel, as C++ `getChannel(ioctx)` returns it, such as `ctx.facets.get()`
    /// (`api/actor-state.c++`) takes from the class it is given to start a facet of.
    ///
    /// # Errors
    /// Fails with what C++ throws, and with an internal error unless `io_context` is the current
    /// `IoContext`.
    pub fn channel<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
    ) -> jsg::Result<ScopedChannel<'s, ActorClassChannel>> {
        let mut slot = KjMaybe::None;
        // SAFETY: `call_cpp` calls the shim with the lock's isolate.
        jsg_cpp::call_cpp(lock, |isolate| unsafe {
            bridge::durable_object_class_channel(isolate, self, io_context.as_ffi(), &mut slot)
        })?;
        let channel = Option::from(slot)
            .unwrap_or_else(|| unreachable!("a shim that completed sets its result"));
        Ok(ScopedChannel::new(io_context, channel))
    }

    /// A new `DurableObjectClass` for `channel`, bound to `io_context` as a C++ one made with
    /// `js.alloc<DurableObjectClass>(ioctx.addObject(channel))` is, such as Worker Loader's
    /// `getDurableObjectClass()` and a loopback class's `call()` (`api/export-loopback.c++`)
    /// return.
    ///
    /// # Errors
    /// Fails with an internal error unless `io_context` is the current `IoContext`.
    pub fn from_channel<'s>(
        lock: &mut jsg::Lock<'s>,
        io_context: &IoContext<'s>,
        channel: ScopedChannel<'s, ActorClassChannel>,
    ) -> jsg::Result<jsg_cpp::Ref<Self>> {
        let id = io_context.as_ffi();
        let channel = channel.into_inner();
        // SAFETY: the shim instantiates callCppForRef with api::DurableObjectClass, and
        // `call_cpp_for_ref` calls it with the lock's isolate.
        let class = unsafe {
            jsg_cpp::call_cpp_for_ref(lock, |isolate, result| {
                bridge::durable_object_class_new(isolate, id, channel, result)
            })
        }?;
        Ok(class.unwrap_or_else(|| unreachable!("a shim that completed sets its result")))
    }
}
