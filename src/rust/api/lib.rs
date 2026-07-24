// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use hmac::Mac;
use hmac::SimpleHmac;
use hmac::digest::KeyInit;
use jsg::ToJS;
use sha3::Digest;
use sha3::Sha3_224;
use sha3::Sha3_256;
use sha3::Sha3_384;
use sha3::Sha3_512;

use crate::dns::DnsUtil;

pub mod dns;

#[cxx::bridge(namespace = "workerd::rust::api")]
mod ffi {
    #[derive(Clone, Copy)]
    enum Sha3Algorithm {
        Sha3_224,
        Sha3_256,
        Sha3_384,
        Sha3_512,
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }
    extern "Rust" {
        type Sha3Hash;
        type Sha3Hmac;

        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);

        #[expect(clippy::unnecessary_box_returns)]
        fn new_sha3_hash(algorithm: Sha3Algorithm) -> Box<Sha3Hash>;
        fn sha3_hash_update(hash: &mut Sha3Hash, data: &[u8]);
        #[expect(clippy::unnecessary_box_returns)]
        fn sha3_hash_clone(hash: &Sha3Hash) -> Box<Sha3Hash>;
        fn sha3_hash_digest_size(hash: &Sha3Hash) -> usize;
        fn sha3_hash_digest(hash: &Sha3Hash) -> Vec<u8>;

        #[expect(clippy::unnecessary_box_returns)]
        fn new_sha3_hmac(algorithm: Sha3Algorithm, key: &[u8]) -> Box<Sha3Hmac>;
        fn sha3_hmac_update(hmac: &mut Sha3Hmac, data: &[u8]);
        fn sha3_hmac_digest(hmac: &Sha3Hmac) -> Vec<u8>;
    }
}

#[derive(Clone)]
pub enum Sha3Hash {
    Sha3_224(Sha3_224),
    Sha3_256(Sha3_256),
    Sha3_384(Sha3_384),
    Sha3_512(Sha3_512),
}

pub enum Sha3Hmac {
    Sha3_224(SimpleHmac<Sha3_224>),
    Sha3_256(SimpleHmac<Sha3_256>),
    Sha3_384(SimpleHmac<Sha3_384>),
    Sha3_512(SimpleHmac<Sha3_512>),
}

impl Clone for Sha3Hmac {
    fn clone(&self) -> Self {
        match self {
            Self::Sha3_224(hmac) => Self::Sha3_224(hmac.clone()),
            Self::Sha3_256(hmac) => Self::Sha3_256(hmac.clone()),
            Self::Sha3_384(hmac) => Self::Sha3_384(hmac.clone()),
            Self::Sha3_512(hmac) => Self::Sha3_512(hmac.clone()),
        }
    }
}

pub fn new_sha3_hash(algorithm: ffi::Sha3Algorithm) -> Box<Sha3Hash> {
    Box::new(match algorithm {
        ffi::Sha3Algorithm::Sha3_224 => Sha3Hash::Sha3_224(Sha3_224::new()),
        ffi::Sha3Algorithm::Sha3_256 => Sha3Hash::Sha3_256(Sha3_256::new()),
        ffi::Sha3Algorithm::Sha3_384 => Sha3Hash::Sha3_384(Sha3_384::new()),
        ffi::Sha3Algorithm::Sha3_512 => Sha3Hash::Sha3_512(Sha3_512::new()),
        _ => unreachable!(),
    })
}

pub fn sha3_hash_update(hash: &mut Sha3Hash, data: &[u8]) {
    match hash {
        Sha3Hash::Sha3_224(hash) => hash.update(data),
        Sha3Hash::Sha3_256(hash) => hash.update(data),
        Sha3Hash::Sha3_384(hash) => hash.update(data),
        Sha3Hash::Sha3_512(hash) => hash.update(data),
    }
}

pub fn sha3_hash_clone(hash: &Sha3Hash) -> Box<Sha3Hash> {
    Box::new(hash.clone())
}

pub fn sha3_hash_digest_size(hash: &Sha3Hash) -> usize {
    match hash {
        Sha3Hash::Sha3_224(_) => 28,
        Sha3Hash::Sha3_256(_) => 32,
        Sha3Hash::Sha3_384(_) => 48,
        Sha3Hash::Sha3_512(_) => 64,
    }
}

pub fn sha3_hash_digest(hash: &Sha3Hash) -> Vec<u8> {
    match hash {
        Sha3Hash::Sha3_224(hash) => hash.clone().finalize().to_vec(),
        Sha3Hash::Sha3_256(hash) => hash.clone().finalize().to_vec(),
        Sha3Hash::Sha3_384(hash) => hash.clone().finalize().to_vec(),
        Sha3Hash::Sha3_512(hash) => hash.clone().finalize().to_vec(),
    }
}

fn unwrap_hmac<T>(hmac: Result<T, hmac::digest::InvalidLength>) -> T {
    match hmac {
        Ok(hmac) => hmac,
        Err(_) => unreachable!("HMAC accepts keys of any length"),
    }
}

pub fn new_sha3_hmac(algorithm: ffi::Sha3Algorithm, key: &[u8]) -> Box<Sha3Hmac> {
    Box::new(match algorithm {
        ffi::Sha3Algorithm::Sha3_224 => {
            Sha3Hmac::Sha3_224(unwrap_hmac(SimpleHmac::<Sha3_224>::new_from_slice(key)))
        }
        ffi::Sha3Algorithm::Sha3_256 => {
            Sha3Hmac::Sha3_256(unwrap_hmac(SimpleHmac::<Sha3_256>::new_from_slice(key)))
        }
        ffi::Sha3Algorithm::Sha3_384 => {
            Sha3Hmac::Sha3_384(unwrap_hmac(SimpleHmac::<Sha3_384>::new_from_slice(key)))
        }
        ffi::Sha3Algorithm::Sha3_512 => {
            Sha3Hmac::Sha3_512(unwrap_hmac(SimpleHmac::<Sha3_512>::new_from_slice(key)))
        }
        _ => unreachable!(),
    })
}

pub fn sha3_hmac_update(hmac: &mut Sha3Hmac, data: &[u8]) {
    match hmac {
        Sha3Hmac::Sha3_224(hmac) => hmac.update(data),
        Sha3Hmac::Sha3_256(hmac) => hmac.update(data),
        Sha3Hmac::Sha3_384(hmac) => hmac.update(data),
        Sha3Hmac::Sha3_512(hmac) => hmac.update(data),
    }
}

pub fn sha3_hmac_digest(hmac: &Sha3Hmac) -> Vec<u8> {
    match hmac {
        Sha3Hmac::Sha3_224(hmac) => hmac.clone().finalize().into_bytes().to_vec(),
        Sha3Hmac::Sha3_256(hmac) => hmac.clone().finalize().into_bytes().to_vec(),
        Sha3Hmac::Sha3_384(hmac) => hmac.clone().finalize().into_bytes().to_vec(),
        Sha3Hmac::Sha3_512(hmac) => hmac.clone().finalize().into_bytes().to_vec(),
    }
}

pub fn register_nodejs_modules(registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry,
        "node-internal:dns",
        // SAFETY: isolate is valid and locked — called from C++ module registration.
        |isolate| unsafe {
            let mut lock = jsg::Lock::from_isolate_ptr(isolate);
            let dns_util = DnsUtil::new();
            dns_util.to_js(&mut lock).into_ffi()
        },
        jsg::modules::ModuleType::Internal,
    );
}

#[cfg(test)]
mod tests {
    use jsg_test::Harness;

    use super::*;

    #[test]
    fn test_wrap_resource_equality() {
        let harness = Harness::new();
        harness.run_in_context(|lock, _ctx| {
            let dns_util = DnsUtil::new();

            let lhs = dns_util.clone().to_js(lock);
            let rhs = dns_util.to_js(lock);

            assert_eq!(lhs, rhs);
            Ok(())
        });
    }
}
