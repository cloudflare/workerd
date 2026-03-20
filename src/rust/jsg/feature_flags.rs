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
        let message = capnp::message::Reader::new(segments, ReaderOptions::new());
        Self { message }
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
    pub fn reader(&self) -> compatibility_flags::Reader<'_> {
        self.message
            .get_root::<compatibility_flags::Reader<'_>>()
            .expect("Invalid FeatureFlags capnp root")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Helper: build a `CompatibilityFlags` capnp message with the given flag setter,
    /// return the raw single-segment bytes (no wire-format header).
    fn build_flags<F>(setter: F) -> Vec<u8>
    where
        F: FnOnce(compatibility_flags::Builder<'_>),
    {
        let mut message = capnp::message::Builder::new_default();
        {
            let flags = message.init_root::<compatibility_flags::Builder<'_>>();
            setter(flags);
        }
        let output = message.get_segments_for_output();
        output[0].to_vec()
    }

    #[test]
    fn from_bytes_roundtrip() {
        let bytes = build_flags(|mut f| {
            f.set_node_js_compat(true);
        });
        let ff = FeatureFlags::from_bytes(&bytes);
        assert!(ff.reader().get_node_js_compat());
    }

    #[test]
    #[should_panic(expected = "FeatureFlags data must not be empty")]
    fn from_bytes_empty_panics() {
        FeatureFlags::from_bytes(&[]);
    }

    #[test]
    fn default_flags_are_false() {
        let bytes = build_flags(|_| {});
        let ff = FeatureFlags::from_bytes(&bytes);
        assert!(!ff.reader().get_node_js_compat());
        assert!(!ff.reader().get_node_js_compat_v2());
        assert!(!ff.reader().get_fetch_refuses_unknown_protocols());
    }

    #[test]
    fn multiple_flags() {
        let bytes = build_flags(|mut f| {
            f.set_node_js_compat(true);
            f.set_node_js_compat_v2(true);
            f.set_fetch_refuses_unknown_protocols(false);
        });
        let ff = FeatureFlags::from_bytes(&bytes);
        assert!(ff.reader().get_node_js_compat());
        assert!(ff.reader().get_node_js_compat_v2());
        assert!(!ff.reader().get_fetch_refuses_unknown_protocols());
    }

    #[test]
    fn reader_called_multiple_times() {
        let bytes = build_flags(|mut f| {
            f.set_node_js_compat(true);
        });
        let ff = FeatureFlags::from_bytes(&bytes);
        // Reader can be obtained multiple times from the same FeatureFlags.
        assert!(ff.reader().get_node_js_compat());
        assert!(ff.reader().get_node_js_compat());
    }
}
