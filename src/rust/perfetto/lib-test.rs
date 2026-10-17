use super::*;

#[test]
fn category_index_is_const() {
    const WORKERD: usize = __private::category_index("workerd");
    assert_eq!(CATEGORIES[WORKERD], "workerd");
}

#[test]
fn truncates_at_nul() {
    assert_eq!(truncate_at_nul("abc"), "abc");
    assert_eq!(truncate_at_nul("a\0bc"), "a");
    assert_eq!(truncate_at_nul(""), "");
}

// Trace points must compile and be callable in both build configurations, whether or not
// Perfetto has been initialized.
#[test]
fn macros_compile_without_session() {
    let value = 42i64;
    let name = String::from("name");
    trace_event!("workerd", "scoped");
    trace_event!("workerd", "scoped with args", |ctx| {
        ctx.add_arg("value", value)
            .add_arg("name", &name)
            .add_arg("flag", true)
            .add_arg("ratio", 0.5)
            .set_flow(Flow::from_ref(&value));
    });
    trace_event_begin!("workerd", "begin", |ctx| {
        ctx.set_track(Track::from_ref(&value));
    });
    trace_event_end!("workerd", |ctx| {
        ctx.set_track(Track::from_ref(&value))
            .set_terminating_flow(Flow::global(1));
    });
    trace_event_instant!("workerd", "instant");
    trace_counter!("workerd", "counter", value);
    trace_counter!("workerd", "counter", 1.5);
    assert!(!trace_event_category_enabled!("workerd"));
}
