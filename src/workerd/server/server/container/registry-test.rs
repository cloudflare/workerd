// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use futures::channel::oneshot;
use futures::executor::LocalPool;
use futures::executor::block_on;
use futures::task::LocalSpawnExt;

use super::*;

/// A client whose shutdown hands the registry the cleanup it was given.
struct Fake {
    registry: Rc<Registry<Self>>,
    id: String,
    cleanup: RefCell<Option<LocalBoxFuture<'static, ()>>>,
    shut_down: Cell<bool>,
}

impl Client for Fake {
    fn shutdown(&self) {
        if self.shut_down.replace(true) {
            return;
        }
        let cleanup = self
            .cleanup
            .take()
            .unwrap_or_else(|| futures::future::ready(()).boxed_local());
        // The server spawns the cleanup; here it runs when a test waits for it.
        drop(self.registry.cleanup_started(&self.id, cleanup));
    }
}

impl Drop for Fake {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// A new client of `id` whose cleanup is `cleanup`, and the cleanup it got to wait for.
fn make(
    registry: &Rc<Registry<Fake>>,
    id: &str,
    cleanup: LocalBoxFuture<'static, ()>,
) -> (Rc<Fake>, Cleanup) {
    let mut previous = None;
    let client = registry
        .make(id, |cleanup_before| {
            previous = Some(cleanup_before);
            Rc::new(Fake {
                registry: Rc::clone(registry),
                id: id.to_owned(),
                cleanup: RefCell::new(Some(cleanup)),
                shut_down: Cell::new(false),
            })
        })
        .unwrap();
    (client, previous.unwrap())
}

fn ready() -> LocalBoxFuture<'static, ()> {
    futures::future::ready(()).boxed_local()
}

fn never() -> LocalBoxFuture<'static, ()> {
    futures::future::pending().boxed_local()
}

#[test]
fn the_first_client_waits_for_nothing() {
    let registry = Rc::new(Registry::default());
    let (_client, previous) = make(&registry, "workerd-ns-a", ready());
    assert!(previous.now_or_never().is_some());
}

#[test]
fn a_live_client_is_found_until_it_is_dropped() {
    let registry = Rc::new(Registry::default());
    let (client, _) = make(&registry, "workerd-ns-a", ready());
    assert!(Rc::ptr_eq(&registry.live("workerd-ns-a").unwrap(), &client));
    assert!(registry.live("workerd-ns-b").is_none());
    drop(client);
    assert!(registry.live("workerd-ns-a").is_none());
}

#[test]
fn the_next_client_cancels_the_previous_cleanup() {
    let registry = Rc::new(Registry::default());
    // A removal that would otherwise never end, and would race the next client's container.
    let (client, _) = make(&registry, "workerd-ns-a", never());
    drop(client);
    let (_next, previous) = make(&registry, "workerd-ns-a", ready());
    assert!(previous.now_or_never().is_some());
}

#[test]
fn a_client_of_another_container_does_not_cancel_a_cleanup() {
    let registry = Rc::new(Registry::default());
    let (sender, receiver) = oneshot::channel::<()>();
    let (client, _) = make(
        &registry,
        "workerd-ns-a",
        async {
            let _ = receiver.await;
        }
        .boxed_local(),
    );
    drop(client);
    let (_other, previous) = make(&registry, "workerd-ns-b", ready());
    assert!(previous.now_or_never().is_some());
    let cleanup = registry.entries.borrow()["workerd-ns-a"].cleanup.clone();
    assert!(cleanup.now_or_never().is_none());
    let (_next, previous) = make(&registry, "workerd-ns-a", ready());
    // Cancelled now, so the sender's message is never read.
    assert!(previous.now_or_never().is_some());
    drop(sender);
}

#[test]
fn shutdown_shuts_clients_down_and_waits_for_their_cleanup() {
    let registry = Rc::new(Registry::default());
    let (sender, receiver) = oneshot::channel::<()>();
    let (client, _) = make(
        &registry,
        "workerd-ns-a",
        async {
            let _ = receiver.await;
        }
        .boxed_local(),
    );
    let mut pool = LocalPool::new();
    let done = Rc::new(Cell::new(false));
    {
        let registry = Rc::clone(&registry);
        let done = Rc::clone(&done);
        pool.spawner()
            .spawn_local(async move {
                registry.shutdown().await;
                done.set(true);
            })
            .unwrap();
    }
    pool.run_until_stalled();
    assert!(client.shut_down.get());
    assert!(!done.get());
    sender.send(()).unwrap();
    pool.run_until_stalled();
    assert!(done.get());
}

#[test]
fn no_client_is_made_after_shutdown() {
    let registry: Rc<Registry<Fake>> = Rc::new(Registry::default());
    block_on(registry.shutdown());
    let error = registry
        .make("workerd-ns-a", |_| unreachable!())
        .err()
        .unwrap();
    assert!(
        error
            .description()
            .contains("cannot acquire a container client after graceful shutdown has begun")
    );
}

#[test]
fn stop_shuts_down_only_the_actor_it_names() {
    let registry = Rc::new(Registry::default());
    let (a, _) = make(&registry, "workerd-ns-a", ready());
    let (b, _) = make(&registry, "workerd-ns-b", ready());
    let (other_ns, _) = make(&registry, "workerd-other-a", ready());
    block_on(registry.stop("ns", "a"));
    assert!(a.shut_down.get());
    assert!(!b.shut_down.get());
    assert!(!other_ns.shut_down.get());
    // A stopped client is no longer handed out, though something still holds it.
    assert!(registry.live("workerd-ns-a").is_none());
    assert!(registry.live("workerd-ns-b").is_some());
}
