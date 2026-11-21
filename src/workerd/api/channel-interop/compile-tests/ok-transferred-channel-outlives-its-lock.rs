// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The channel that `require_allows_transfer` returns can be kept past its request, as C++ keeps a
// channel once `requireAllowsTransfer()` allows it, and wrapped again in a later one.
use std::cell::RefCell;

use channel_interop::Fetcher;
use channel_interop::ScopedChannel;
use channel_interop::SubrequestChannel;
use io_context::IoContext;
use kj_rs::KjRc;

thread_local! {
    static KEPT: RefCell<Option<KjRc<SubrequestChannel>>> = const { RefCell::new(None) };
}

pub fn keep<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    fetcher: &Fetcher,
) -> jsg::Result<()> {
    let channel = fetcher
        .subrequest_channel(lock, io_context)?
        .require_allows_transfer(lock)?;
    KEPT.set(Some(channel));
    Ok(())
}

pub fn rewrap<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
) -> jsg::Result<Option<jsg_cpp::Ref<Fetcher>>> {
    KEPT.take()
        .map(|channel| {
            Fetcher::from_channel(lock, io_context, ScopedChannel::new(io_context, channel))
        })
        .transpose()
}
