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
