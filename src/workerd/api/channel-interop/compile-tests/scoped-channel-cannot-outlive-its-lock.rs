// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A channel is valid only in the request it was obtained in, so the `ScopedChannel` a `Fetcher` or
// a `DurableObjectClass` returns cannot outlive the lock it was obtained with, as for a stash that
// a later request takes it from. Only the channel that `require_allows_transfer` returns can.
use std::cell::RefCell;
use std::rc::Rc;

use channel_interop::ActorClassChannel;
use channel_interop::DurableObjectClass;
use channel_interop::Fetcher;
use channel_interop::ScopedChannel;
use channel_interop::SubrequestChannel;
use io_context::IoContext;

pub fn fetcher<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    fetcher: &Fetcher,
    stash: &Rc<RefCell<Option<ScopedChannel<'static, SubrequestChannel>>>>,
) -> jsg::Result<()> {
    let channel = fetcher.subrequest_channel(lock, io_context)?;
    *stash.borrow_mut() = Some(channel); //~ ERROR E0521
    Ok(())
}

pub fn class<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    class: &DurableObjectClass,
    stash: &Rc<RefCell<Option<ScopedChannel<'static, ActorClassChannel>>>>,
) -> jsg::Result<()> {
    let channel = class.channel(lock, io_context)?;
    *stash.borrow_mut() = Some(channel); //~ ERROR E0521
    Ok(())
}
