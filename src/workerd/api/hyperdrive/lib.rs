// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Hyperdrive`, the JavaScript API of a Hyperdrive binding, as `src/workerd/api/hyperdrive.c++`
//! implements it: the connection parameters of a database, and `connect()`, which opens a TCP
//! socket to it through the binding's subrequest channel.
//!
//! The host is a random name under `.hyperdrive.local`. Reading it, or anything built from it,
//! registers connect overrides in the request's global scope, so that `connect()` from
//! `cloudflare:sockets` to the host, or to a synthetic IPv4 address that `node:dns` resolves it
//! to, reaches the database too.

mod ffi;

use std::cell::RefCell;
use std::fmt::Write;

use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

pub use crate::ffi::Socket;

/// The ports of the databases Hyperdrive supports: the one for `mysql`, and the one for any other
/// scheme, which is taken to be `PostgreSQL`.
const MYSQL_PORT: u16 = 3306;
const POSTGRES_PORT: u16 = 5432;

#[jsg_resource(name = "Hyperdrive", memory_info)]
pub struct Hyperdrive {
    /// The subrequest channel of the binding.
    channel: u32,
    database: String,
    user: String,
    password: String,
    scheme: String,
    random_host: String,
    /// The synthetic IPv4 address, once the connect overrides are registered.
    random_ip: RefCell<Option<String>>,
}

#[jsg_resource]
impl Hyperdrive {
    #[jsg_property(lazy_instance, readonly)]
    pub fn get_database(&self) -> String {
        self.database.clone()
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_user(&self) -> String {
        self.user.clone()
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_password(&self) -> String {
        self.password.clone()
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_host(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        self.register_overrides(lock)?;
        Ok(self.random_host.clone())
    }

    /// A synthetic IPv4 address in the reserved 240.0.0.0/4 block that routes to this binding, for
    /// drivers that require an IP literal rather than a hostname.
    #[jsg_property(lazy_instance, readonly)]
    pub fn get_ip(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        self.register_overrides(lock)
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_port(&self) -> u16 {
        port_for_scheme(&self.scheme)
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_connection_string(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        self.register_overrides(lock)?;
        Ok(connection_string(
            &self.scheme,
            &self.user,
            &self.password,
            &self.random_host,
            &self.database,
        ))
    }

    /// Connects to the database, returning a TCP socket.
    #[jsg_method]
    pub fn connect(&self, lock: &mut jsg::Lock<'_>) -> Result<jsg_cpp::Ref<Socket>, jsg::Error> {
        self.register_overrides(lock)?;
        ffi::connect(lock, self.channel, &self.random_host, self.get_port())
    }
}

impl Hyperdrive {
    fn new(
        channel: u32,
        database: &str,
        user: &str,
        password: &str,
        scheme: &str,
    ) -> Result<Self, jsg::Error> {
        let mut random = [0_u8; 16];
        ffi::fill_entropy(&mut random)?;
        Ok(Self {
            channel,
            database: database.to_owned(),
            user: user.to_owned(),
            password: password.to_owned(),
            scheme: scheme.to_owned(),
            random_host: random_host(&random),
            random_ip: RefCell::new(None),
        })
    }

    /// Registers the connect and DNS overrides for the host and a synthetic IPv4 address, unless
    /// they are registered, and returns the address. Needs the current request's global scope,
    /// so it runs on first use rather than when the binding is created.
    fn register_overrides(&self, lock: &mut jsg::Lock<'_>) -> Result<String, jsg::Error> {
        if let Some(ip) = self.random_ip.borrow().as_ref() {
            return Ok(ip.clone());
        }
        let port = self.get_port();
        let mut random = [0_u8; 2];
        ffi::fill_entropy(&mut random)?;
        let ip = find_free_ip(u16::from_be_bytes(random), |candidate| {
            ffi::has_connect_override(lock, &format!("{candidate}:{port}"))
        })?;
        ffi::register_overrides(lock, self.channel, &self.random_host, &ip, port)?;
        *self.random_ip.borrow_mut() = Some(ip.clone());
        Ok(ip)
    }
}

fn port_for_scheme(scheme: &str) -> u16 {
    if scheme == "mysql" {
        MYSQL_PORT
    } else {
        POSTGRES_PORT
    }
}

/// `<hex of random>.hyperdrive.local`.
fn random_host(random: &[u8]) -> String {
    let mut host = String::with_capacity(random.len() * 2 + ".hyperdrive.local".len());
    for byte in random {
        let _ = write!(host, "{byte:02x}");
    }
    host.push_str(".hyperdrive.local");
    host
}

/// The synthetic IPv4 address `240.0.x.y` whose low 16 bits are `suffix`.
fn synthetic_ip(suffix: u16) -> String {
    let [high, low] = suffix.to_be_bytes();
    format!("240.0.{high}.{low}")
}

/// The first synthetic address, from the one for `start` on, that `is_taken` says no other
/// binding has claimed. Within 240.0.0.0/16, which is in the reserved, non-routable 240.0.0.0/4
/// block, so it can never collide with a real host the Worker might connect to.
fn find_free_ip(
    start: u16,
    mut is_taken: impl FnMut(&str) -> Result<bool, jsg::Error>,
) -> Result<String, jsg::Error> {
    let mut suffix = start;
    loop {
        let candidate = synthetic_ip(suffix);
        if !is_taken(&candidate)? {
            return Ok(candidate);
        }
        suffix = suffix.wrapping_add(1);
        if suffix == start {
            return Err(jsg::Error::new_error(
                "no free Hyperdrive address available",
            ));
        }
    }
}

/// The connection string, with TLS disabled as each database spells it: the connection is to
/// Hyperdrive, which runs locally.
fn connection_string(
    scheme: &str,
    user: &str,
    password: &str,
    host: &str,
    database: &str,
) -> String {
    let port = port_for_scheme(scheme);
    let ssl_parameter = if scheme == "mysql" {
        "?ssl-mode=disabled"
    } else {
        "?sslmode=disable"
    };
    format!("{scheme}://{user}:{password}@{host}:{port}/{database}{ssl_parameter}")
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
