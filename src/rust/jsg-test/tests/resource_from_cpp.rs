// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for resources that C++ creates and holds, as `src/rust/jsg/README.md` §Creating
//! resources from C++ describes: C++ calls an `extern "Rust"` constructor, keeps the wrapper in a
//! `v8::Global`, and checks its type with `jsg::resource::ffi_is`.

use std::cell::Cell;
use std::cell::RefCell;

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::Harness;

#[jsg_resource]
struct Held {
    name: String,
}

#[jsg_resource]
impl Held {
    #[jsg_method]
    fn name(&self) -> String {
        self.name.clone()
    }
}

impl Drop for Held {
    fn drop(&mut self) {
        HELD_DROPS.with(|drops| drops.set(drops.get() + 1));
    }
}

#[jsg_resource]
struct Other {}

#[jsg_resource]
impl Other {}

thread_local! {
    static HELD_DROPS: Cell<u32> = const { Cell::new(0) };
    /// What `check` saw, recorded rather than asserted, since a panic in a function that C++
    /// calls would abort the test process.
    static CHECKED: RefCell<Option<Checked>> = const { RefCell::new(None) };
}

#[derive(Debug, PartialEq, Eq)]
struct Checked {
    drops: u32,
    /// What `jsg::resource::ffi_is` returned, none if an exception was pending.
    is_held: Option<bool>,
    is_other: Option<bool>,
    name: String,
}

/// A constructor for C++, written as a crate's `extern "Rust"` function would be.
///
/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn new_held(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let held = jsg::Rc::new(Held {
                name: "made for C++".to_owned(),
            });
            Ok::<_, jsg::Error>(held.to_js(lock)?)
        })
    }
}

/// A constructor for C++ that returns a resource of another type.
///
/// # Safety
/// As for [`new_held`].
unsafe fn new_other(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            Ok::<_, jsg::Error>(jsg::Rc::new(Other {}).to_js(lock)?)
        })
    }
}

/// A constructor for C++ that returns a plain object.
///
/// # Safety
/// As for [`new_held`].
unsafe fn new_plain_object(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            Ok::<_, jsg::Error>(v8::Local::<v8::Value>::from(lock.new_object()))
        })
    }
}

/// Records what C++ holds: its type, and the name if it is a `Held`.
///
/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open, and
/// `value` a handle in that scope.
unsafe fn check(isolate: *mut v8::ffi::Isolate, value: v8::ffi::Local) {
    // SAFETY: forwarded from the caller.
    let is_held = Option::from(unsafe { jsg::resource::ffi_is::<Held>(isolate, value) });
    // SAFETY: forwarded from the caller.
    let is_other = Option::from(unsafe { jsg::resource::ffi_is::<Other>(isolate, value) });
    // SAFETY: forwarded from the caller.
    jsg::callback_scope!(unsafe let lock, isolate);
    // SAFETY: `value` is a handle in the scope that `lock` stands for.
    let value: v8::Local<'_, v8::Value> = unsafe { v8::Local::from_ffi(value) };
    let name = if is_held == Some(true) {
        <jsg::Rc<Held> as jsg::FromJS>::from_js(lock, value)
            .map(|held| held.name.clone())
            .unwrap_or_default()
    } else {
        String::new()
    };
    let checked = Checked {
        drops: HELD_DROPS.with(Cell::get),
        is_held,
        is_other,
        name,
    };
    CHECKED.with(|c| *c.borrow_mut() = Some(checked));
}

fn hold_across_gc(create: fn(*mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal) -> Checked {
    HELD_DROPS.with(|drops| drops.set(0));
    CHECKED.with(|c| c.borrow_mut().take());
    Harness::new().run_in_context(|lock, _ctx| {
        // SAFETY: C++ calls `check` as `check` requires.
        Harness::hold_across_gc(lock, create, |isolate, value| unsafe {
            check(isolate, value);
        });
        Ok(())
    });
    CHECKED.with(|c| c.borrow_mut().take()).unwrap()
}

#[test]
fn resource_held_by_cpp_survives_gc_and_is_collected_after() {
    assert_eq!(
        hold_across_gc(|isolate| {
            // SAFETY: C++ calls this as `new_held` requires.
            unsafe { new_held(isolate) }
        }),
        Checked {
            drops: 0,
            is_held: Some(true),
            is_other: Some(false),
            name: "made for C++".to_owned(),
        }
    );
    // C++ reset its `Global` and ran a full GC, which collected the resource.
    assert_eq!(HELD_DROPS.with(Cell::get), 1);
}

#[test]
fn is_rejects_another_resource_type() {
    let checked = hold_across_gc(|isolate| {
        // SAFETY: C++ calls this as `new_other` requires.
        unsafe { new_other(isolate) }
    });
    assert_eq!(checked.is_held, Some(false));
    assert_eq!(checked.is_other, Some(true));
}

#[test]
fn is_rejects_a_plain_object() {
    let checked = hold_across_gc(|isolate| {
        // SAFETY: C++ calls this as `new_plain_object` requires.
        unsafe { new_plain_object(isolate) }
    });
    assert_eq!(checked.is_held, Some(false));
    assert_eq!(checked.is_other, Some(false));
}

#[test]
fn is_rejects_primitives_and_cpp_objects() {
    Harness::new().run_in_context(|lock, ctx| {
        for code in ["undefined", "null", "1", "'held'", "({})", "[]"] {
            let value = ctx.eval_raw(lock, code).unwrap();
            assert!(!jsg::resource::is::<Held>(lock, value), "{code}");
        }
        let cpp = Harness::create_cpp_tagged_object(lock);
        assert!(!jsg::resource::is::<Held>(lock, cpp));
        Ok(())
    });
}

#[test]
fn is_does_not_keep_the_resource_alive() {
    HELD_DROPS.with(|drops| drops.set(0));
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let held = jsg::Rc::new(Held {
            name: "checked".to_owned(),
        });
        let value = held.to_js(lock)?;
        assert!(jsg::resource::is::<Held>(lock, value));
        Ok(())
    });
    harness.run_in_context(|lock, _ctx| {
        Harness::request_gc(lock);
        Ok(())
    });
    // Only the wrapper owned the resource; `is` left no reference behind.
    assert_eq!(HELD_DROPS.with(Cell::get), 1);
}
