// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `this: jsg::Rc<Self>` parameters, which give a method or accessor a `jsg::Rc` of
//! the resource it was called on, in place of `&self`.

use std::cell::Cell;
use std::cell::RefCell;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use jsg::ExceptionType;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

static TASK_DROPS: AtomicUsize = AtomicUsize::new(0);

/// A resource whose methods and accessors take `this` instead of `&self`.
#[jsg_resource]
struct Task {
    name: String,
    label: RefCell<String>,
    /// The wrapper of the queue this task is in, which the task keeps as a resource keeps a
    /// callback it registered: through a `TracedGlobal`, so that GC can collect the cycle.
    queue: Cell<Option<v8::TracedGlobal<v8::Value>>>,
}

impl Drop for Task {
    fn drop(&mut self) {
        TASK_DROPS.fetch_add(1, Ordering::SeqCst);
    }
}

#[jsg_resource]
impl Task {
    #[jsg_constructor]
    fn constructor(name: String) -> Self {
        Self {
            name,
            label: RefCell::new(String::new()),
            queue: Cell::new(None),
        }
    }

    /// Returns the resource itself, so that JavaScript can check that `this` is the object the
    /// method was called on.
    #[jsg_method]
    fn itself(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }

    /// A JavaScript argument of the resource's own type is still an argument.
    #[jsg_method]
    fn is_same(this: jsg::Rc<Self>, other: jsg::Rc<Self>) -> bool {
        this == other
    }

    /// `this` dereferences to the resource.
    #[jsg_method]
    fn name(this: jsg::Rc<Self>) -> String {
        this.name.clone()
    }

    /// `this` comes before the lock, as `&self` does.
    #[jsg_method]
    fn name_value<'s>(
        this: jsg::Rc<Self>,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        this.name.as_str().to_js(lock)
    }

    /// Drops its `jsg::Rc` while the JavaScript wrapper may be the only other owner.
    #[jsg_method]
    fn drop_this(this: jsg::Rc<Self>) {
        drop(this);
    }

    /// Hands `this` to `queue`, which keeps it as a callback's data would, and keeps the queue's
    /// wrapper in return.
    #[jsg_method]
    fn schedule(this: jsg::Rc<Self>, lock: &mut jsg::Lock<'_>, queue: jsg::Rc<Queue>) {
        this.queue.set(Some(
            queue.clone().to_js(lock).unwrap().to_traced_global(lock),
        ));
        queue.tasks.borrow_mut().push(this);
    }

    #[jsg_property]
    fn get_label(this: jsg::Rc<Self>) -> String {
        this.label.borrow().clone()
    }

    #[jsg_property]
    fn set_label(this: jsg::Rc<Self>, label: String) {
        *this.label.borrow_mut() = label;
    }

    #[jsg_property(instance)]
    fn get_own_name(this: jsg::Rc<Self>) -> String {
        this.name.clone()
    }

    #[jsg_inspect_property]
    fn debug_name(this: jsg::Rc<Self>) -> String {
        format!("Task({})", this.name)
    }
}

/// A resource that holds `jsg::Rc`s of tasks, as a callback's data would.
#[jsg_resource]
struct Queue {
    tasks: RefCell<Vec<jsg::Rc<Task>>>,
}

#[jsg_resource]
impl Queue {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self {
            tasks: RefCell::new(Vec::new()),
        }
    }

    #[jsg_method]
    fn clear(&self) {
        self.tasks.borrow_mut().clear();
    }

    #[jsg_method]
    fn len(&self) -> f64 {
        f64::from(u32::try_from(self.tasks.borrow().len()).unwrap())
    }
}

/// Sets `task` and `other`, two tasks, and `queue` as globals. JavaScript creates them, so that no
/// `Local` in the test's handle scope keeps a wrapper alive; the tests' scripts likewise end in
/// `undefined` where their completion value would be one.
fn install(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    let task = jsg::resource::function_template_of::<Task>(lock);
    ctx.set_global(lock, "Task", task);
    let queue = jsg::resource::function_template_of::<Queue>(lock);
    ctx.set_global(lock, "Queue", queue);
    ctx.eval_raw(
        lock,
        r"
        globalThis.task = new Task('first');
        globalThis.other = new Task('second');
        globalThis.queue = new Queue();
        undefined
        ",
    )
    .expect("should evaluate");
}

/// `this` is the resource the method was called on: it wraps to the same object, compares equal
/// to a `jsg::Rc` made from that object, and reads its fields.
#[test]
fn this_is_the_resource_the_method_was_called_on() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert!(ctx.eval::<bool>(lock, "task.itself() === task").unwrap());
        assert!(ctx.eval::<bool>(lock, "other.itself() === other").unwrap());
        assert!(ctx.eval::<bool>(lock, "task.isSame(task)").unwrap());
        assert!(!ctx.eval::<bool>(lock, "task.isSame(other)").unwrap());
        assert_eq!(ctx.eval::<String>(lock, "task.name()").unwrap(), "first");
        assert_eq!(ctx.eval::<String>(lock, "other.name()").unwrap(), "second");
        assert_eq!(
            ctx.eval::<String>(lock, "task.nameValue()").unwrap(),
            "first"
        );
        Ok(())
    });
}

#[test]
fn function_lengths_exclude_this_and_lock() {
    for (enabled, expected) in [(false, "0,0,0,0"), (true, "0,1,0,1")] {
        crate::Harness::with_flags(|mut flags| {
            flags.set_spec_compliant_property_attributes(enabled);
        })
        .run_in_context(|lock, ctx| {
            install(lock, ctx);
            let lengths = ctx
                .eval::<String>(
                    lock,
                    "[task.itself.length, task.isSame.length, task.nameValue.length, \
                     task.schedule.length].join()",
                )
                .unwrap();
            assert_eq!(lengths, expected);
            Ok(())
        });
    }
}

/// Accessors take `this` too, on the prototype, as own properties and as inspect properties.
#[test]
fn accessors_take_this() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert_eq!(ctx.eval::<String>(lock, "task.label").unwrap(), "");
        assert_eq!(
            ctx.eval::<String>(lock, "task.label = 'urgent'; task.label")
                .unwrap(),
            "urgent"
        );
        assert_eq!(ctx.eval::<String>(lock, "other.label").unwrap(), "");
        assert_eq!(ctx.eval::<String>(lock, "task.ownName").unwrap(), "first");
        assert!(
            ctx.eval::<bool>(lock, "Object.keys(task).includes('ownName')")
                .unwrap()
        );
        let debug_name = ctx
            .eval::<String>(
                lock,
                r"
                const sym = Object.getOwnPropertySymbols(Object.getPrototypeOf(task))
                    .find((s) => s.description === 'debugName');
                task[sym]
                ",
            )
            .unwrap();
        assert_eq!(debug_name, "Task(first)");
        Ok(())
    });
}

/// A method dropping `this` leaves the resource to its wrapper, which GC collects once
/// JavaScript lets go of it.
#[test]
fn dropping_this_leaves_the_resource_to_its_wrapper() {
    TASK_DROPS.store(0, Ordering::SeqCst);
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        ctx.eval_raw(lock, "task.dropThis(); task.dropThis()")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(ctx.eval::<String>(lock, "task.name()").unwrap(), "first");

        ctx.eval_raw(lock, "globalThis.task = undefined")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 1);
        Ok(())
    });
}

/// A `jsg::Rc` that a method hands off keeps the resource alive after JavaScript drops its
/// wrapper, and the resource is collected once that `jsg::Rc` is gone.
#[test]
fn this_handed_off_keeps_the_resource_alive() {
    TASK_DROPS.store(0, Ordering::SeqCst);
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        ctx.eval_raw(
            lock,
            "task.schedule(queue); globalThis.task = undefined; globalThis.other = undefined",
        )
        .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(
            TASK_DROPS.load(Ordering::SeqCst),
            1,
            "only the task that was not scheduled is collected"
        );
        assert_eq!(ctx.eval::<u32>(lock, "queue.len()").unwrap(), 1);

        ctx.eval_raw(lock, "queue.clear()")
            .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// A cycle of a resource that keeps a wrapper through a `TracedGlobal`, and of the `jsg::Rc` of
/// that resource which the wrapper's resource keeps, is collected once JavaScript drops both.
#[test]
fn cycle_through_this_and_a_traced_global_is_collected() {
    TASK_DROPS.store(0, Ordering::SeqCst);
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        ctx.eval_raw(lock, "task.schedule(queue); other.schedule(queue)")
            .expect("should evaluate");
        // The tasks' and the queue's references to each other are strong at first; a GC puts
        // them in traced mode, and the next one can collect the cycle.
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 0);
        ctx.eval_raw(
            lock,
            "globalThis.task = undefined; globalThis.other = undefined; globalThis.queue = undefined",
        )
        .expect("should evaluate");
        crate::Harness::request_gc(lock);
        assert_eq!(TASK_DROPS.load(Ordering::SeqCst), 2);
        Ok(())
    });
}

/// Asserts that `code` throws a `TypeError` about an illegal invocation.
fn assert_illegal_invocation(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str) {
    let err = ctx
        .eval::<String>(lock, code)
        .unwrap_err()
        .unwrap_jsg_err(lock);
    assert_eq!(err.name, ExceptionType::TypeError, "{code}");
    assert!(
        err.message.contains("Illegal invocation"),
        "{code}: {}",
        err.message
    );
}

/// A method or accessor called on a value that is not a resource of its type throws a
/// `TypeError` and never runs.
#[test]
fn wrong_this_throws_a_type_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        ctx.eval_raw(
            lock,
            r"
            const proto = Object.getPrototypeOf(task);
            const label = Object.getOwnPropertyDescriptor(proto, 'label');
            const debugName = Object.getOwnPropertyDescriptor(
                proto,
                Object.getOwnPropertySymbols(proto).find((s) => s.description === 'debugName'),
            );
            ",
        )
        .expect("should evaluate");
        for receiver in ["{}", "queue", "undefined", "1"] {
            assert_illegal_invocation(lock, ctx, &format!("proto.name.call({receiver})"));
            assert_illegal_invocation(lock, ctx, &format!("label.get.call({receiver})"));
            assert_illegal_invocation(lock, ctx, &format!("label.set.call({receiver}, 'x')"));
            assert_illegal_invocation(lock, ctx, &format!("debugName.get.call({receiver})"));
        }
        // The wrong calls left the resources as they were.
        assert_eq!(ctx.eval::<String>(lock, "task.label").unwrap(), "");
        assert_eq!(
            ctx.eval::<String>(lock, "proto.name.call(other)").unwrap(),
            "second"
        );
        Ok(())
    });
}
