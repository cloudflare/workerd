// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The server run inside a test process, for `server-test.c++`.
//!
//! The test builds the worker factory itself (its V8, timer, network and filesystem) and reaches
//! the server through the `loopback:` registry: the config's addresses are loopback names, the
//! test connects to the server's sockets by name and accepts, by name, the connections the
//! server makes, the `network` services' included (`Loopback::mock_internet`). Config errors and
//! warnings queue for the test to take as they are reported.

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use kj_rs::KjOwn;
use tokio::sync::Notify;
use tokio::sync::mpsc;

use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;
use crate::config::Reporter;
use crate::listen::loopback::LoopbackListener;
use crate::run::RunOptions;

pub struct InProcessServer {
    factory: Rc<Factory>,
    reports: mpsc::UnboundedSender<ffi::ConfigReport>,
    reported: RefCell<mpsc::UnboundedReceiver<ffi::ConfigReport>>,
    drain: Notify,
    /// The names the test accepts connections for.
    listeners: RefCell<HashMap<String, Rc<LoopbackListener>>>,
}

pub fn new_in_process_server(factory: KjOwn<ffi::WorkerFactory>) -> Box<InProcessServer> {
    crate::log::install();
    let factory = Rc::new(Factory::new(factory));
    factory.loopback().enable();
    factory.loopback().mock_internet();
    let (reports, reported) = mpsc::unbounded_channel();
    Box::new(InProcessServer {
        factory,
        reports,
        reported: RefCell::new(reported),
        drain: Notify::new(),
        listeners: RefCell::default(),
    })
}

/// Ends the server's background tasks and drops the factory; a factory something still holds
/// is logged as an error.
pub async fn close_in_process_server(server: Box<InProcessServer>) -> Result<()> {
    let InProcessServer { factory, .. } = *server;
    crate::entry::shutdown(factory).await
}

impl InProcessServer {
    fn reporter(&self) -> Reporter {
        let report = |error| {
            let reports = self.reports.clone();
            // The receiver lives as long as the sender: both are the server's.
            move |message| drop(reports.send(ffi::ConfigReport { error, message }))
        };
        Reporter::new(Box::new(report(true)), Box::new(report(false)), true)
    }

    /// The next config error or warning the server reports.
    pub async fn next_report(&self) -> ffi::ConfigReport {
        // The borrow lasts one poll. The sender is this server's, so the channel never closes.
        match std::future::poll_fn(|cx| self.reported.borrow_mut().poll_recv(cx)).await {
            Some(report) => report,
            None => std::future::pending().await,
        }
    }

    /// `workerd serve`, until [`Self::drain`]; `debug_port` is `--debug-port`, if not empty.
    pub async fn run(&self, debug_port: &str) -> Result<()> {
        let options = RunOptions {
            debug_port: (!debug_port.is_empty()).then(|| debug_port.to_owned()),
            ..RunOptions::default()
        };
        let factory = Rc::clone(&self.factory);
        crate::run::run(factory, options, self.reporter(), self.drain.notified()).await
    }

    /// `workerd test`.
    pub async fn test(&self, service_pattern: &str, entrypoint_pattern: &str) -> Result<bool> {
        let passed = crate::run::test(
            Rc::clone(&self.factory),
            RunOptions::default(),
            self.reporter(),
            service_pattern,
            entrypoint_pattern,
        )
        .await?;
        Ok(passed == Some(true))
    }

    pub fn drain(&self) {
        self.drain.notify_one();
    }

    /// A connection to the loopback name `name`, queued for whoever listens on it.
    pub fn connect(&self, name: &str) -> Result<KjOwn<ffi::AsyncIoStream>> {
        let stream = self.factory.loopback().connect(name)?;
        Ok(kj_hyper::into_kj_stream(stream))
    }

    /// The next connection made to the loopback name `name`.
    pub async fn accept(&self, name: &str) -> Result<KjOwn<ffi::AsyncIoStream>> {
        let listener = match self.listeners.borrow_mut().entry(name.to_owned()) {
            std::collections::hash_map::Entry::Occupied(entry) => Rc::clone(entry.get()),
            std::collections::hash_map::Entry::Vacant(entry) => {
                let listener = self.factory.loopback().listen(name)?;
                Rc::clone(entry.insert(Rc::new(listener)))
            }
        };
        Ok(kj_hyper::into_kj_stream(listener.accept().await?))
    }
}
