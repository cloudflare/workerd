use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;
use std::time::Duration;

use tokio::runtime::Builder;
use tokio::sync::oneshot;

use super::*;

/// Sets its flag when dropped: something a future given to `block_on` owns.
struct SetOnDrop(Rc<Cell<bool>>);

impl Drop for SetOnDrop {
    fn drop(&mut self) {
        self.0.set(true);
    }
}

#[test]
fn block_on_returns_the_output_and_can_be_called_again() {
    let mut runtime = Runtime::new().unwrap();
    for i in 0..3 {
        // The sleep is tokio's to run; the task's end reaches the wait as a KJ event.
        let output = runtime.block_on(async move {
            tokio::time::sleep(Duration::from_millis(1)).await;
            i
        });
        assert_eq!(output, Ok(i));
    }
}

#[test]
fn a_task_left_pending_resumes_in_the_next_block_on() {
    let mut runtime = Runtime::new().unwrap();
    let (resume, resumed) = oneshot::channel();
    #[expect(
        clippy::async_yields_async,
        reason = "the JoinHandle is the output, to await in the next block_on"
    )]
    let task = runtime
        .block_on(async move { crate::spawn(async move { resumed.await.unwrap() }) })
        .unwrap();
    assert!(!task.is_finished());
    resume.send(7).unwrap();
    assert_eq!(runtime.block_on(task).unwrap().unwrap(), 7);
}

#[test]
fn a_panic_in_the_future_resumes_out_of_block_on() {
    let mut runtime = Runtime::new().unwrap();
    let future = poll_fn(|_| -> Poll<()> { panic!("deliberate panic in the future") });
    let panic = catch_unwind(AssertUnwindSafe(|| runtime.block_on(future))).unwrap_err();
    assert_eq!(
        panic.downcast_ref::<&str>(),
        Some(&"deliberate panic in the future")
    );
    // The loop stays usable.
    assert_eq!(runtime.block_on(async { 1 }), Ok(1));
}

#[test]
fn a_kj_exception_out_of_the_wait_is_an_error_and_drops_the_future() {
    let mut runtime = Runtime::new().unwrap();
    let dropped = Rc::new(Cell::new(false));
    let guard = SetOnDrop(Rc::clone(&dropped));
    // Under another runtime's `block_on` the port cannot sleep: its own `block_on` panics, and
    // the port's wait() throws that as a kj::Exception.
    let foreign = Builder::new_current_thread().build().unwrap();
    foreign.block_on(async {
        let error = runtime
            .block_on(async move {
                let _guard = guard;
                std::future::pending::<()>().await;
            })
            .unwrap_err();
        assert!(error.description().contains("wait_forever"), "{error:?}");
        assert!(dropped.get());
    });
    drop(foreign);
    // The loop stays usable.
    assert_eq!(runtime.block_on(async { 1 }), Ok(1));
}

/// Panics when dropped.
struct PanicOnDrop;

impl Drop for PanicOnDrop {
    fn drop(&mut self) {
        panic!("deliberate panic in a task's drop");
    }
}

#[test]
fn a_panic_dropping_a_pending_task_stays_in_tokio_when_the_runtime_drops() {
    let mut runtime = Runtime::new().unwrap();
    let (guarded, guard_built) = oneshot::channel();
    runtime
        .block_on(async {
            crate::spawn(async move {
                let _guard = PanicOnDrop;
                guarded.send(()).unwrap();
                std::future::pending::<()>().await;
            });
            guard_built.await.unwrap();
        })
        .unwrap();
    // tokio drops a cancelled task's future under `catch_unwind`, so nothing unwinds out of the
    // context's destructor.
    drop(runtime);
}

#[test]
fn a_second_runtime_on_the_thread_is_refused() {
    let runtime = Runtime::new().unwrap();
    let error = Runtime::new().err().unwrap();
    assert!(
        error.description().contains("only one TokioEventPort"),
        "{error:?}"
    );
    drop(runtime);
    // The first one gone, the thread takes a Runtime again.
    drop(Runtime::new().unwrap());
}
