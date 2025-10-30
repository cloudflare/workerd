use std::cell::RefCell;
use std::iter::repeat_with;

thread_local! {
    static RNG: RefCell<fastrand::Rng> = RefCell::new(fastrand::Rng::new());
}

#[cxx::bridge(namespace = "kj::rust")]
pub mod ffi {
    extern "Rust" {
        /// Fill a buffer with random bytes using a non-cryptographic PRNG.
        /// The buffer can be any length.
        fn fill_random_bytes(buffer: &mut [u8]);
    }
}

/// Fills the provided buffer with random bytes using a fast non-cryptographic PRNG.
/// Uses a thread-local Rng instance for efficiency and thread-safety.
pub fn fill_random_bytes(buffer: &mut [u8]) {
    RNG.with(|rng| {
        let mut rng = rng.borrow_mut();
        for (dest, src) in buffer.iter_mut().zip(repeat_with(|| rng.u8(..))) {
            *dest = src;
        }
    });
}
