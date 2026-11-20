use std::sync::mpsc;

use super::*;
use crate::Runtime;

#[test]
fn a_thread_without_a_port_has_no_progress_to_watch() {
    let error = watch_progress(Duration::from_millis(1)).err().unwrap();
    assert!(matches!(error, WatchError::NoPort), "{error:?}");
    assert_eq!(error.to_string(), "no TokioEventPort drives this thread");
}

/// A loop that sleeps counts its beats.
#[test]
fn a_sleeping_loop_counts_its_beats() {
    let mut runtime = Runtime::new().unwrap();
    let counted = runtime
        .block_on(async {
            let progress = watch_progress(Duration::from_millis(5)).unwrap();
            let before = progress.count();
            tokio::time::sleep(Duration::from_millis(200)).await;
            progress.count() - before
        })
        .unwrap();
    // 40 beats on time; a loaded machine runs fewer.
    assert!(counted >= 5, "{counted} beats in 200 ms");
}

/// A task that keeps the loop thread stops the count, as another thread sees it.
#[test]
fn a_task_that_keeps_the_thread_stops_the_count() {
    let mut runtime = Runtime::new().unwrap();
    let (first, second) = runtime
        .block_on(async {
            let progress = watch_progress(Duration::from_millis(1)).unwrap();
            let (send, receive) = mpsc::channel();
            let watcher = std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(50));
                let first = progress.count();
                std::thread::sleep(Duration::from_millis(100));
                send.send((first, progress.count())).unwrap();
            });
            // Nothing else runs on the thread meanwhile.
            std::thread::sleep(Duration::from_millis(200));
            watcher.join().unwrap();
            receive.recv().unwrap()
        })
        .unwrap();
    assert_eq!(first, second);
}

/// The beat ends with the last clone of its `Progress`.
#[test]
fn the_beat_ends_with_its_progress() {
    let mut runtime = Runtime::new().unwrap();
    let counted = runtime
        .block_on(async {
            let progress = watch_progress(Duration::from_millis(5)).unwrap();
            let count = Arc::clone(&progress.count);
            drop(progress);
            // The beat's task sees its `Progress` gone after its next beat.
            tokio::time::sleep(Duration::from_millis(20)).await;
            let before = count.load(Ordering::Relaxed);
            tokio::time::sleep(Duration::from_millis(200)).await;
            count.load(Ordering::Relaxed) - before
        })
        .unwrap();
    // The probe still counts each wake of the loop, of which a sleep has one or two.
    assert!(counted < 5, "{counted} counts in 200 ms without a beat");
}
