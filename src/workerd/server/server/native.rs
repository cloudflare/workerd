// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Services a binary adds to the graph in code.
//!
//! Every service `workerd` serves is in its config. A binary built on this crate for a test or a
//! benchmark can add services of its own: it calls [`register`] on the thread that will run the
//! server, before the command line runs, and the server then has a service of that name, which
//! the config's sockets and bindings refer to like any other. There is no config surface for
//! these services, and the `workerd` binary registers none.
//!
//! Registered services are made after the config's, in the order they were registered, so each
//! can look up the config's services and the ones registered before it. A config service of the
//! same name takes the place of a registered one.

use std::cell::RefCell;
use std::rc::Rc;

use crate::Result;
use crate::channels::Channel;
use crate::config::Factory;

/// Resolves a service by name to its channel: a Worker's default entrypoint, or the service
/// itself.
pub type Lookup<'a> = &'a dyn Fn(&str) -> Option<Rc<dyn Channel>>;

/// Makes a registered service for one server.
pub type Make = Box<dyn FnOnce(&Rc<Factory>, Lookup<'_>) -> Result<Rc<dyn Channel>>>;

thread_local! {
    static REGISTERED: RefCell<Vec<(String, Make)>> = const { RefCell::new(Vec::new()) };
}

/// Adds a service named `name` to the next server started on this thread.
pub fn register(
    name: &str,
    make: impl FnOnce(&Rc<Factory>, Lookup<'_>) -> Result<Rc<dyn Channel>> + 'static,
) {
    REGISTERED.with_borrow_mut(|registered| registered.push((name.to_owned(), Box::new(make))));
}

/// The services registered on this thread, which are then registered no longer.
pub(crate) fn take() -> Vec<(String, Make)> {
    REGISTERED.take()
}

#[cfg(test)]
#[path = "native-test.rs"]
mod tests;
