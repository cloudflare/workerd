// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `channel-interop-test.c++`: `Probe`, a Rust resource that takes, returns and
//! holds `Fetcher`s and `DurableObjectClass`es, as a ported Worker Loader or Container would, and
//! the functions through which C++ creates it.

use std::cell::RefCell;

use channel_interop::DurableObjectClass;
use channel_interop::Fetcher;
use channel_interop::InHouse;
use channel_interop::RequiresHostAndProtocol;
use channel_interop::ScopedChannel;
use channel_interop::SubrequestChannel;
use io_context::IoContext;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[cxx::bridge(namespace = "workerd::api::channel_interop::test")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    extern "Rust" {
        /// A new `Probe`. C++ calls it with the isolate locked, a context entered and a
        /// `HandleScope` open.
        unsafe fn probe_new(isolate: *mut Isolate) -> MaybeLocal;

        /// How many things `Probe.stash()` holds.
        fn stashed_count() -> usize;

        /// Drops what `Probe.stash()` holds. C++ calls it without the isolate lock, once the
        /// request is gone.
        fn drop_stashed();
    }
}

/// What `Probe.stash()` holds outside any resource, as Rust code that holds a `Fetcher` and its
/// channel past the request does, once the channel's transfer is allowed. A channel belongs to its
/// thread, so this is thread-local.
#[derive(Default)]
struct Stash {
    fetchers: Vec<jsg_cpp::Ref<Fetcher>>,
    channels: Vec<SubrequestChannel>,
}

thread_local! {
    static STASH: RefCell<Stash> = RefCell::new(Stash::default());
}

fn stashed_count() -> usize {
    STASH.with_borrow(|stash| stash.fetchers.len() + stash.channels.len())
}

fn drop_stashed() {
    STASH.take();
}

/// What a dynamic Worker's code names, as `WorkerCode` does: an optional, nullable global outbound
/// and an optional list of tail Workers.
#[jsg_struct]
pub struct Outbounds {
    pub outbound: Option<jsg::Nullable<jsg_cpp::Ref<Fetcher>>>,
    pub tails: Option<Vec<jsg_cpp::Ref<Fetcher>>>,
}

#[jsg_resource]
pub struct Probe {
    /// A `Fetcher` held by a resource, traced as a C++ `jsg::Ref` field is.
    held: jsg::RefCell<Option<jsg_cpp::Ref<Fetcher>>>,
}

#[jsg_resource]
impl Probe {
    /// Takes a `Fetcher` argument, and nothing else.
    #[jsg_method]
    pub fn take_fetcher(&self, _fetcher: jsg_cpp::Ref<Fetcher>) -> bool {
        true
    }

    /// Takes a `DurableObjectClass` argument, and nothing else.
    #[jsg_method]
    pub fn take_class(&self, _class: jsg_cpp::Ref<DurableObjectClass>) -> bool {
        true
    }

    /// Whether an optional argument was given.
    #[jsg_method]
    pub fn has_fetcher(&self, fetcher: Option<jsg_cpp::Ref<Fetcher>>) -> bool {
        fetcher.is_some()
    }

    /// Describes the `Fetcher`s of `outbounds`, each of which must be transferable, as Worker
    /// Loader requires of a dynamic Worker's global outbound and tails.
    #[jsg_method]
    pub fn describe_outbounds<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        outbounds: Outbounds,
    ) -> Result<String, jsg::Error> {
        let io_context = IoContext::current(lock)?;
        let mut require_transfer = |fetcher: &jsg_cpp::Ref<Fetcher>| {
            let channel = fetcher.get(lock).subrequest_channel(lock, &io_context)?;
            channel.require_allows_transfer(lock)
        };
        let outbound = match &outbounds.outbound {
            None => "default",
            Some(jsg::Nullable::Null) => "none",
            Some(jsg::Nullable::Some(fetcher)) => {
                require_transfer(fetcher)?;
                "fetcher"
            }
        };
        let tails = outbounds.tails.as_deref().unwrap_or_default();
        for tail in tails {
            require_transfer(tail)?;
        }
        Ok(format!("outbound: {outbound}, tails: {}", tails.len()))
    }

    /// A new `Fetcher` for the channel of `fetcher`, as Worker Loader's `getEntrypoint()` makes
    /// one for a channel.
    #[jsg_method]
    pub fn rewrap<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        fetcher: jsg_cpp::Ref<Fetcher>,
    ) -> Result<jsg_cpp::Ref<Fetcher>, jsg::Error> {
        let io_context = IoContext::current(lock)?;
        let channel = fetcher.get(lock).subrequest_channel(lock, &io_context)?;
        Fetcher::from_channel(lock, &io_context, channel)
    }

    /// `rewrap()`, for a channel that must be transferable, made from the channel as kept past
    /// its request.
    #[jsg_method]
    pub fn rewrap_transferable<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        fetcher: jsg_cpp::Ref<Fetcher>,
    ) -> Result<jsg_cpp::Ref<Fetcher>, jsg::Error> {
        let io_context = IoContext::current(lock)?;
        let channel = fetcher.get(lock).subrequest_channel(lock, &io_context)?;
        let kept = channel.require_allows_transfer(lock)?;
        Fetcher::from_channel(lock, &io_context, ScopedChannel::new(&io_context, kept))
    }

    /// A new `DurableObjectClass` for the channel of `class`, as Worker Loader's
    /// `getDurableObjectClass()` makes one for a channel.
    #[jsg_method]
    pub fn rewrap_class<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        class: jsg_cpp::Ref<DurableObjectClass>,
    ) -> Result<jsg_cpp::Ref<DurableObjectClass>, jsg::Error> {
        let io_context = IoContext::current(lock)?;
        let channel = class.get(lock).channel(lock, &io_context)?;
        channel.require_allows_transfer(lock)?;
        DurableObjectClass::from_channel(lock, &io_context, channel)
    }

    /// A new `Fetcher` on the numbered channel `channel`, as a binding is.
    #[jsg_method]
    pub fn fetcher_for_channel(
        &self,
        lock: &mut jsg::Lock<'_>,
        channel: u32,
        requires_host_and_protocol: bool,
        in_house: bool,
    ) -> Result<jsg_cpp::Ref<Fetcher>, jsg::Error> {
        let requires_host_and_protocol = if requires_host_and_protocol {
            RequiresHostAndProtocol::Yes
        } else {
            RequiresHostAndProtocol::No
        };
        let in_house = if in_house { InHouse::Yes } else { InHouse::No };
        Fetcher::from_channel_number(lock, channel, requires_host_and_protocol, in_house)
    }

    /// `fetcher.mapVirtualHost(port)`, as `ctx.mapVirtualHost(fetcher, port)` calls it.
    #[jsg_method]
    pub fn map_virtual_host(
        &self,
        lock: &mut jsg::Lock<'_>,
        fetcher: jsg_cpp::Ref<Fetcher>,
        port: u16,
    ) -> Result<String, jsg::Error> {
        fetcher.get(lock).map_virtual_host(lock, port)
    }

    /// Keeps `fetcher` in a traced field.
    #[jsg_method]
    pub fn hold(&self, lock: &mut jsg::Lock<'_>, fetcher: jsg_cpp::Ref<Fetcher>) {
        *self.held.borrow_mut(lock) = Some(fetcher);
    }

    /// The held `Fetcher`, the same JavaScript object `hold()` was given.
    #[jsg_method]
    pub fn held(&self, lock: &mut jsg::Lock<'_>) -> Option<jsg_cpp::Ref<Fetcher>> {
        self.held
            .borrow()
            .as_ref()
            .map(|fetcher| fetcher.clone(lock))
    }

    /// Keeps `fetcher` and its channel outside any resource, for `drop_stashed()`. The channel must
    /// be transferable.
    #[jsg_method]
    pub fn stash<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        fetcher: jsg_cpp::Ref<Fetcher>,
    ) -> Result<(), jsg::Error> {
        let io_context = IoContext::current(lock)?;
        let channel = fetcher
            .get(lock)
            .subrequest_channel(lock, &io_context)?
            .require_allows_transfer(lock)?;
        STASH.with_borrow_mut(|stash| {
            stash.fetchers.push(fetcher);
            stash.channels.push(channel);
        });
        Ok(())
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn probe_new(isolate: *mut ffi::Isolate) -> ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            jsg::Rc::new(
                lock,
                Probe {
                    held: jsg::RefCell::new(None),
                },
            )
            .to_js(lock)
        })
    }
}
