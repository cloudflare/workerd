use super::*;

fn description(err: error::KjIoError) -> String {
    cxx::KjError::from(err).description().to_owned()
}

#[test]
fn a_thread_without_a_port_is_refused_not_panicked() {
    let err = std::thread::spawn(|| ensure_loop_thread().unwrap_err())
        .join()
        .unwrap();
    assert!(description(err).contains("no TokioEventPort"));
    // A plain tokio runtime is not a TokioEventPort either.
    let runtime = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();
    let _entered = runtime.enter();
    assert!(description(ensure_loop_thread().unwrap_err()).contains("no TokioEventPort"));
}

#[test]
fn a_port_thread_passes_unless_another_runtime_is_entered_over_it() {
    let _port = kj_rs_tokio::TokioPort::new();
    ensure_loop_thread().unwrap();
    let auxiliary = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();
    {
        let _entered = auxiliary.enter();
        assert!(
            description(ensure_loop_thread().unwrap_err())
                .contains("other than this thread's TokioEventPort runtime")
        );
    }
    ensure_loop_thread().unwrap();
}
