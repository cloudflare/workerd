use std::ptr::null_mut;

use aws_lc_rs::digest;
use aws_lc_sys::EVP_DigestFinalXOF;
use aws_lc_sys::EVP_DigestInit_ex;
use aws_lc_sys::EVP_DigestUpdate;
use aws_lc_sys::EVP_MD;
use aws_lc_sys::EVP_MD_CTX;
use aws_lc_sys::EVP_MD_CTX_free;
use aws_lc_sys::EVP_MD_CTX_new;
use aws_lc_sys::EVP_shake128;
use aws_lc_sys::EVP_shake256;

#[cxx::bridge(namespace = "workerd::rust::sha3")]
mod ffi {
    extern "Rust" {
        fn sha3_256(data: &[u8]) -> Vec<u8>;
        fn sha3_384(data: &[u8]) -> Vec<u8>;
        fn sha3_512(data: &[u8]) -> Vec<u8>;
        fn cshake128(data: &[u8], output_len: usize) -> Vec<u8>;
        fn cshake256(data: &[u8], output_len: usize) -> Vec<u8>;
    }
}

#[must_use]
pub fn sha3_256(data: &[u8]) -> Vec<u8> {
    digest::digest(&digest::SHA3_256, data).as_ref().to_vec()
}

#[must_use]
pub fn sha3_384(data: &[u8]) -> Vec<u8> {
    digest::digest(&digest::SHA3_384, data).as_ref().to_vec()
}

#[must_use]
pub fn sha3_512(data: &[u8]) -> Vec<u8> {
    digest::digest(&digest::SHA3_512, data).as_ref().to_vec()
}

struct EvpMdCtx(*mut EVP_MD_CTX);

impl EvpMdCtx {
    fn new(md: *const EVP_MD) -> Self {
        assert!(!md.is_null());

        // SAFETY: EVP_MD_CTX_new does not take arguments and returns an owned context pointer or null.
        let ctx = unsafe { EVP_MD_CTX_new() };
        assert!(!ctx.is_null());

        // SAFETY: ctx is a non-null context created above, md is a non-null AWS-LC digest descriptor,
        // and a null ENGINE pointer is accepted by EVP_DigestInit_ex.
        let ok = unsafe { EVP_DigestInit_ex(ctx, md, null_mut()) };
        assert_eq!(ok, 1);
        Self(ctx)
    }
}

impl Drop for EvpMdCtx {
    fn drop(&mut self) {
        // SAFETY: self.0 is exclusively owned by this wrapper and was returned by EVP_MD_CTX_new.
        unsafe {
            EVP_MD_CTX_free(self.0);
        }
    }
}

type EvpMdFactory = unsafe extern "C" fn() -> *const EVP_MD;

fn shake(data: &[u8], output_len: usize, md_factory: EvpMdFactory) -> Vec<u8> {
    if output_len == 0 {
        return Vec::new();
    }

    // SAFETY: md_factory is one of AWS-LC's EVP_shake* functions and returns a static descriptor.
    let md = unsafe { md_factory() };
    let ctx = EvpMdCtx::new(md);
    if !data.is_empty() {
        // SAFETY: ctx owns a valid initialized EVP_MD_CTX, and data.as_ptr() is valid for data.len()
        // bytes because data is non-empty.
        let ok = unsafe { EVP_DigestUpdate(ctx.0, data.as_ptr().cast(), data.len()) };
        assert_eq!(ok, 1);
    }

    let mut output = vec![0u8; output_len];
    // SAFETY: ctx owns a valid SHAKE EVP_MD_CTX, and output is non-empty and writable for output.len().
    let ok = unsafe { EVP_DigestFinalXOF(ctx.0, output.as_mut_ptr(), output.len()) };
    assert_eq!(ok, 1);
    output
}

#[must_use]
pub fn cshake128(data: &[u8], output_len: usize) -> Vec<u8> {
    shake(data, output_len, EVP_shake128)
}

#[must_use]
pub fn cshake256(data: &[u8], output_len: usize) -> Vec<u8> {
    shake(data, output_len, EVP_shake256)
}
