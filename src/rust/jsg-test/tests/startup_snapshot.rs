// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust resources across a V8 startup snapshot: a zygote's context holds them when the snapshot
//! is built, and an isolate started from it finds them attached to their restored wrappers.

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ToJS;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

static POINT_COPIES: AtomicUsize = AtomicUsize::new(0);

/// A resource with state that a snapshot carries.
#[jsg_resource]
struct SnapshotPoint {
    x: u32,
}

impl jsg::SnapshotTransfer for SnapshotPoint {
    type Snapshot = u32;

    fn snapshot(&self) -> u32 {
        POINT_COPIES.fetch_add(1, Ordering::SeqCst);
        self.x
    }

    fn restore(x: &u32) -> Self {
        POINT_COPIES.fetch_add(1, Ordering::SeqCst);
        Self { x: *x }
    }
}

#[jsg_resource]
impl SnapshotPoint {
    #[jsg_constructor]
    fn constructor(x: u32) -> Self {
        Self { x }
    }

    #[jsg_method]
    fn x(&self) -> u32 {
        self.x
    }
}

/// A resource that refers to another, which JavaScript can see.
#[jsg_resource]
struct SnapshotLine {
    end: jsg::RefCell<Option<jsg::Rc<SnapshotPoint>>>,
    /// The `x` of `end`, to rebuild it from when a snapshot does not carry it.
    end_x: u32,
}

impl jsg::SnapshotTransfer for SnapshotLine {
    type Snapshot = u32;

    fn snapshot(&self) -> u32 {
        self.end_x
    }

    fn restore(end_x: &u32) -> Self {
        Self {
            end: jsg::RefCell::new(None),
            end_x: *end_x,
        }
    }

    fn snapshot_references(&self, references: &mut jsg::SnapshotReferences) {
        references.add(self.end.borrow().as_ref());
    }

    fn snapshot_relink(
        &self,
        lock: &mut jsg::Lock<'_>,
        restored: &mut jsg::RestoredReferences<'_>,
    ) {
        let end = restored
            .next::<SnapshotPoint>(lock)
            .unwrap_or_else(|| jsg::Rc::new(lock, SnapshotPoint { x: self.end_x }));
        *self.end.borrow_mut(lock) = Some(end);
    }
}

#[jsg_resource]
impl SnapshotLine {
    #[jsg_constructor]
    #[expect(
        clippy::needless_pass_by_ref_mut,
        reason = "a callback takes its lock as `&mut`"
    )]
    fn constructor(lock: &mut jsg::Lock<'_>, end: jsg::Rc<SnapshotPoint>) -> Self {
        Self {
            end_x: end.get(lock).x,
            end: jsg::RefCell::new(Some(end)),
        }
    }

    #[jsg_method]
    fn end(&self, lock: &mut jsg::Lock<'_>) -> Result<jsg::Rc<SnapshotPoint>, jsg::Error> {
        self.end
            .borrow()
            .as_ref()
            .map(|end| end.clone(lock))
            .ok_or_else(|| jsg::Error::new_type_error("no end"))
    }
}

/// A resource that a snapshot cannot carry.
#[jsg_resource]
struct Unclonable;

#[jsg_resource]
impl Unclonable {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self
    }
}

fn define<R: jsg::Resource>(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, name: &str) {
    let constructor = jsg::resource::function_template_of::<R>(lock).unwrap();
    ctx.set_global(lock, name, constructor);
}

#[test]
fn a_resource_that_implements_snapshot_transfer_keeps_its_state() {
    POINT_COPIES.store(0, Ordering::SeqCst);
    crate::snapshot_round_trip(
        |lock, ctx| {
            define::<SnapshotPoint>(lock, ctx, "SnapshotPoint");
            ctx.eval_raw(lock, "globalThis.point = new SnapshotPoint(7); undefined")
                .unwrap();
            Ok(())
        },
        |lock, ctx| {
            // Once in the zygote, on the original, and once more for this isolate.
            assert_eq!(POINT_COPIES.load(Ordering::SeqCst), 2);
            assert_eq!(ctx.eval::<u32>(lock, "point.x()").unwrap(), 7);
            assert!(
                ctx.eval::<bool>(lock, "point instanceof SnapshotPoint")
                    .unwrap()
            );
            assert_eq!(
                ctx.eval::<u32>(lock, "new SnapshotPoint(3).x()").unwrap(),
                3
            );
            Ok(())
        },
    )
    .unwrap();
}

/// A wrapper that Rust creates in the restored isolate gets the template the snapshot carried, so
/// that it is an instance of the class the restored wrappers are, and their methods accept it.
#[test]
fn restored_and_new_wrappers_share_their_template() {
    crate::snapshot_round_trip(
        |lock, ctx| {
            define::<SnapshotPoint>(lock, ctx, "SnapshotPoint");
            ctx.eval_raw(lock, "globalThis.point = new SnapshotPoint(7); undefined")
                .unwrap();
            Ok(())
        },
        |lock, ctx| {
            let made = jsg::Rc::new(lock, SnapshotPoint { x: 3 }).to_js(lock)?;
            ctx.set_global(lock, "made", made);
            assert!(
                ctx.eval::<bool>(lock, "made instanceof SnapshotPoint")
                    .unwrap()
            );
            assert!(
                ctx.eval::<bool>(
                    lock,
                    "Object.getPrototypeOf(made) === Object.getPrototypeOf(point)"
                )
                .unwrap()
            );
            assert_eq!(ctx.eval::<u32>(lock, "point.x.call(made)").unwrap(), 3);
            Ok(())
        },
    )
    .unwrap();
}

/// A restored resource refers to the restored resource behind the wrapper that JavaScript sees.
/// A wrapper that the context reaches only through a Rust reference is not in the snapshot, so
/// the restored resource gets none for it, and builds its own.
#[test]
fn a_restored_reference_is_to_the_restored_resource() {
    crate::snapshot_round_trip(
        |lock, ctx| {
            define::<SnapshotPoint>(lock, ctx, "SnapshotPoint");
            define::<SnapshotLine>(lock, ctx, "SnapshotLine");
            ctx.eval_raw(
                lock,
                "globalThis.point = new SnapshotPoint(7); \
                 globalThis.line = new SnapshotLine(point); \
                 globalThis.other = new SnapshotLine(new SnapshotPoint(5)); undefined",
            )
            .unwrap();
            Ok(())
        },
        |lock, ctx| {
            assert!(ctx.eval::<bool>(lock, "line.end() === point").unwrap());
            assert!(ctx.eval::<bool>(lock, "line.end() === line.end()").unwrap());
            assert_eq!(ctx.eval::<u32>(lock, "other.end().x()").unwrap(), 5);
            assert!(
                ctx.eval::<bool>(lock, "other.end() === other.end()")
                    .unwrap()
            );
            Ok(())
        },
    )
    .unwrap();
}

#[test]
fn a_resource_without_snapshot_transfer_fails_the_zygote() {
    let failure = crate::snapshot_round_trip(
        |lock, ctx| {
            define::<Unclonable>(lock, ctx, "Unclonable");
            ctx.eval_raw(lock, "globalThis.u = new Unclonable(); undefined")
                .unwrap();
            Ok(())
        },
        |_, _| panic!("no isolate starts from a snapshot that was not created"),
    )
    .unwrap_err();
    assert!(failure.contains("Unclonable"), "{failure}");
}

/// One the context no longer reaches when the snapshot is created does not matter.
#[test]
fn an_unreachable_resource_without_snapshot_transfer_does_not_matter() {
    crate::snapshot_round_trip(
        |lock, ctx| {
            define::<Unclonable>(lock, ctx, "Unclonable");
            ctx.eval_raw(lock, "new Unclonable(); undefined").unwrap();
            Ok(())
        },
        |lock, ctx| {
            assert!(
                ctx.eval::<bool>(lock, "typeof Unclonable === 'function'")
                    .unwrap()
            );
            Ok(())
        },
    )
    .unwrap();
}

/// A function that `Function::from_callback` creates crosses the snapshot: V8 serializes its
/// callback by its index in the external-reference table.
#[test]
fn a_function_from_a_callback_crosses_the_snapshot() {
    crate::snapshot_round_trip(
        |lock, ctx| {
            let f = jsg::v8::Function::from_callback(
                lock,
                jsg::function_callback(|_lock, args| args.set_return_number(42.0)),
            )?;
            ctx.set_global(lock, "f", f);
            Ok(())
        },
        |lock, ctx| {
            assert_eq!(ctx.eval::<u32>(lock, "f()").unwrap(), 42);
            Ok(())
        },
    )
    .unwrap();
}

/// A function that `Function::new` creates fails the zygote, as one that C++ JSG wraps does: the
/// Wrappable that owns its data has no snapshot form.
#[test]
fn a_function_with_data_fails_the_zygote() {
    let failure = crate::snapshot_round_trip(
        |lock, ctx| {
            let f = jsg::v8::Function::new(lock, (), |lock, (), _args| Ok(lock.undefined()))?;
            ctx.set_global(lock, "f", f);
            Ok(())
        },
        |_, _| panic!("no isolate starts from a snapshot that was not created"),
    )
    .unwrap_err();
    assert!(failure.contains("WrappableFunction"), "{failure}");
}

/// So does a pending promise with a Rust reaction.
#[test]
fn a_pending_rust_reaction_fails_the_zygote() {
    let failure = crate::snapshot_round_trip(
        |lock, ctx| {
            let promise = jsg::v8::PromiseResolver::new(lock)?.promise();
            let reacted = promise.then(lock, (), |_lock, (), value| Ok(value))?;
            ctx.set_global(lock, "p", reacted);
            ctx.set_global(lock, "q", promise);
            Ok(())
        },
        |_, _| panic!("no isolate starts from a snapshot that was not created"),
    )
    .unwrap_err();
    assert!(failure.contains("WrappableFunction"), "{failure}");
}

#[test]
fn lock_knows_which_side_of_a_snapshot_it_is_on() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        assert!(!lock.is_preparing_snapshot());
        assert!(!lock.is_starting_from_snapshot());
        Ok(())
    });
    crate::snapshot_round_trip(
        |lock, _ctx| {
            assert!(lock.is_preparing_snapshot());
            assert!(!lock.is_starting_from_snapshot());
            Ok(())
        },
        |lock, _ctx| {
            assert!(!lock.is_preparing_snapshot());
            assert!(lock.is_starting_from_snapshot());
            Ok(())
        },
    )
    .unwrap();
}

#[test]
fn pinned_value_is_extracted_once_from_the_snapshot() {
    let index = std::cell::Cell::new(None);
    crate::snapshot_round_trip(
        |lock, ctx| {
            let value = ctx
                .eval_raw(lock, "({ answer: 42, nested: [1, 2] })")
                .unwrap();
            index.set(Some(lock.pin_into_snapshot(value)?));
            Ok(())
        },
        |lock, ctx| {
            let index = index.get().expect("the zygote pinned a value");
            let value = lock
                .extract_once_from_snapshot(index)
                .expect("the snapshot holds the pinned value");
            ctx.set_global(lock, "pinned", value);
            assert_eq!(
                ctx.eval::<String>(lock, "`${pinned.answer},${pinned.nested}`")
                    .unwrap(),
                "42,1,2"
            );
            assert!(lock.extract_once_from_snapshot(index).is_none());
            Ok(())
        },
    )
    .unwrap();
}

#[test]
#[should_panic(expected = "not preparing a snapshot")]
fn pin_into_snapshot_requires_a_zygote() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let value = 1.to_js(lock)?;
        lock.pin_into_snapshot(value)?;
        Ok(())
    });
}
