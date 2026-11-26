// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::rc::Rc;
use std::rc::Weak;
use std::time::Duration;
use std::time::SystemTime;

use futures::future::LocalBoxFuture;
use sqlite_rs::Vfs;

use super::time_from_ns;
use super::time_to_ns;
use crate::Result;
use crate::alarm_scheduler::ActorKey;
use crate::alarm_scheduler::AlarmEnv;
use crate::alarm_scheduler::AlarmScheduler;
use crate::alarm_scheduler::AlarmStorage;
use crate::alarm_scheduler::StoredAlarm;
use crate::alarm_store::AlarmStore;
use crate::channels::ActorAlarms;

#[test]
fn converts_times_to_and_from_nanoseconds_around_the_epoch() {
    for ns in [0, 1, -1, 3_600_000_000_001, -3_599_999_999_999] {
        assert_eq!(time_to_ns(time_from_ns(ns)), ns);
    }
    assert_eq!(
        time_from_ns(-1_500_000_000),
        SystemTime::UNIX_EPOCH - Duration::from_millis(1_500)
    );
    assert_eq!(
        time_from_ns(2_000_000_001),
        SystemTime::UNIX_EPOCH + Duration::new(2, 1)
    );
}

#[test]
fn saturates_times_beyond_the_range_of_nanoseconds() {
    let far = Duration::from_secs(u64::from(u32::MAX) * 1_000);
    assert_eq!(time_to_ns(SystemTime::UNIX_EPOCH + far), i64::MAX);
    assert_eq!(time_to_ns(SystemTime::UNIX_EPOCH - far), i64::MIN);
}

/// An env for a scheduler whose alarms never come due: its clock stays at the epoch and its
/// timer never fires.
struct IdleEnv;

impl AlarmEnv for IdleEnv {
    fn now(&self) -> SystemTime {
        SystemTime::UNIX_EPOCH
    }

    fn sleep(&self, _delay: Duration) -> LocalBoxFuture<'static, ()> {
        Box::pin(std::future::pending())
    }

    fn spawn(&self, task: LocalBoxFuture<'static, ()>) {
        drop(task);
    }

    fn actor(&self, _actor: &ActorKey) -> Result<Box<dyn worker::Interface>> {
        unreachable!("no alarm comes due")
    }
}

/// A root actor's storage hooks outlive its namespace while its last requests drain. An alarm
/// they set or delete then goes to the store, so that the next run schedules it.
#[test]
fn an_alarm_set_after_the_namespace_is_gone_is_stored_for_the_next_run() {
    let vfs = Vfs::in_memory().unwrap();
    let alarms = ActorAlarms {
        namespace: Weak::new(),
        store: Rc::new(AlarmStore::open(&vfs, "metadata.sqlite").unwrap()),
    };
    let time = SystemTime::UNIX_EPOCH + Duration::from_hours(1);
    alarms
        .schedule(
            "named",
            Some("my-name").into(),
            Some(time_to_ns(time)).into(),
        )
        .unwrap();
    alarms
        .schedule("deleted", None.into(), Some(time_to_ns(time)).into())
        .unwrap();
    alarms
        .schedule("deleted", None.into(), None.into())
        .unwrap();
    drop(alarms);

    let store = AlarmStore::open(&vfs, "metadata.sqlite").unwrap();
    assert_eq!(
        store.load().unwrap(),
        [StoredAlarm {
            actor: ActorKey {
                id: "named".to_owned(),
                name: Some("my-name".to_owned()),
            },
            scheduled_time: time,
        }]
    );
    let scheduler = AlarmScheduler::new(Rc::new(IdleEnv), Box::new(store)).unwrap();
    assert_eq!(scheduler.get_alarm("named"), Some(time));
    assert_eq!(scheduler.get_alarm("deleted"), None);
}
