//! Rust-native access to workerd compatibility flags.
//!
//! ```ignore
//! if lock.feature_flags().get_node_js_compat() {
//!     // Node.js compatibility behavior
//! }
//! ```

use capnp::message::ReaderOptions;
pub use compatibility_date_capnp::compatibility_flags;

/// Provides access to the current worker's compatibility flags.
///
/// Parsed once from canonical Cap'n Proto bytes during Realm construction
/// and stored in the per-context [`Realm`](crate::Realm). Access via
/// [`Lock::feature_flags()`](crate::Lock::feature_flags).
pub struct FeatureFlags {
    message: capnp::message::Reader<Vec<Vec<u8>>>,
    /// Read once, like the flags C++ JSG copies into `IsolateBase`, as every `#[jsg_struct]`
    /// conversion to JavaScript checks it and `reader()` validates the root on each call.
    fast_jsg_struct: bool,
}

impl FeatureFlags {
    /// Create from canonical (single-segment, no segment table) Cap'n Proto bytes.
    ///
    /// On the C++ side, produce these via `capnp::canonicalize(reader)`.
    ///
    /// # Panics
    ///
    /// Panics if `data` is empty or not word-aligned.
    pub(crate) fn from_bytes(data: &[u8]) -> Self {
        assert!(!data.is_empty(), "FeatureFlags data must not be empty");
        assert!(
            data.len().is_multiple_of(8),
            "FeatureFlags data must be word-aligned (got {} bytes)",
            data.len()
        );
        let segments = vec![data.to_vec()];
        // A Reader charges every read against one traversal limit for as long as it lives, and
        // this one lives as long as the Realm, with every `reader()` call reading the root. The
        // bytes are C++'s own canonical encoding rather than untrusted input, so there is no limit.
        let mut options = ReaderOptions::new();
        options.traversal_limit_in_words(None);
        Self::new(capnp::message::Reader::new(segments, options))
    }

    /// Creates flags from a message built in Rust, such as for an RTTI export.
    pub(crate) fn from_message(
        message: &capnp::message::Builder<capnp::message::HeapAllocator>,
    ) -> Self {
        let segments = message
            .get_segments_for_output()
            .iter()
            .map(|segment| segment.to_vec())
            .collect();
        Self::new(capnp::message::Reader::new(segments, ReaderOptions::new()))
    }

    fn new(message: capnp::message::Reader<Vec<Vec<u8>>>) -> Self {
        let mut flags = Self {
            message,
            fast_jsg_struct: false,
        };
        flags.fast_jsg_struct = flags.reader().get_fast_jsg_struct();
        flags
    }

    /// Returns the `CompatibilityFlags` reader.
    ///
    /// The reader has a getter for each flag defined in `compatibility-date.capnp`
    /// (e.g., `get_node_js_compat()`).
    ///
    /// # Panics
    ///
    /// Panics if the stored message has an invalid capnp root (should never happen
    /// when constructed via `from_bytes`).
    #[expect(
        clippy::expect_used,
        reason = "the bytes come from C++'s `capnp::canonicalize`, so the root is valid"
    )]
    pub fn reader(&self) -> compatibility_flags::Reader<'_> {
        self.message
            .get_root::<compatibility_flags::Reader<'_>>()
            .expect("Invalid FeatureFlags capnp root")
    }

    /// Returns the `fast_jsg_struct` flag without reading the message.
    pub fn fast_jsg_struct(&self) -> bool {
        self.fast_jsg_struct
    }
}

#[cfg(test)]
#[path = "feature_flags-test.rs"]
mod tests;
