// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `#[jsg_property(lazy_instance)]` and `PropertyPlacement::LazyInstance`.

use std::cell::Cell;

use jsg::MemberRegistry;
use jsg::PropertyPlacement;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

use crate::Harness;

/// Counts how often each lazy getter runs.
#[jsg_resource]
struct Lazy {
    list_calls: Cell<u32>,
    label_calls: Cell<u32>,
    fail: Cell<bool>,
}

#[jsg_resource]
impl Lazy {
    #[jsg_property(lazy_instance)]
    pub fn get_list(&self) -> Vec<String> {
        self.list_calls.set(self.list_calls.get() + 1);
        vec!["a".to_owned(), "b".to_owned()]
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_label(&self) -> Result<String, jsg::Error> {
        self.label_calls.set(self.label_calls.get() + 1);
        if self.fail.get() {
            return Err(jsg::Error::new_range_error("no label"));
        }
        Ok("lazy".to_owned())
    }
}

impl Lazy {
    fn new() -> Self {
        Self {
            list_calls: Cell::new(0),
            label_calls: Cell::new(0),
            fail: Cell::new(false),
        }
    }
}

/// Runs `code` with a fresh `Lazy` as the global `l`, and returns its result and the resource.
fn eval_lazy<T>(code: &str) -> (T, jsg::Rc<Lazy>)
where
    T: for<'a> jsg::FromJS<'a, ResultType = T> + Default,
{
    let mut out = T::default();
    let lazy = jsg::Rc::new(Lazy::new());
    let resource = lazy.clone();
    Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "l", lazy);
        out = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    (out, resource)
}

#[test]
fn lazy_getter_runs_once_and_caches_its_result() {
    let (same, lazy) = eval_lazy::<bool>("l.list === l.list && l.list.join() === 'a,b'");
    assert!(same, "every read must return the value of the first one");
    assert_eq!(lazy.list_calls.get(), 1);
}

#[test]
fn lazy_getter_does_not_run_until_first_access() {
    let (keys, lazy) = eval_lazy::<String>("Object.keys(l).join()");
    // Listing the keys does not read the values.
    assert_eq!(keys, "label,list");
    assert_eq!(lazy.list_calls.get(), 0);
    assert_eq!(lazy.label_calls.get(), 0);
}

#[test]
fn lazy_getters_run_per_instance() {
    let lazy = jsg::Rc::new(Lazy::new());
    let other = jsg::Rc::new(Lazy::new());
    let (first, second) = (lazy.clone(), other.clone());
    Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "a", lazy);
        ctx.set_global(lock, "b", other);
        let distinct: bool = ctx.eval(lock, "a.list !== b.list").unwrap();
        assert!(distinct);
        Ok(())
    });
    assert_eq!(first.list_calls.get(), 1);
    assert_eq!(second.list_calls.get(), 1);
}

#[test]
fn lazy_property_descriptors_match_cpp_lazy_instance_properties() {
    // Like C++ JSG_LAZY_INSTANCE_PROPERTY: an own data property, enumerable and configurable,
    // writable unless declared `readonly` (JSG_LAZY_READONLY_INSTANCE_PROPERTY). The descriptor
    // is the same before and after the first read.
    let (desc, _) = eval_lazy::<String>(
        "const d = (k) => { \
           const x = Object.getOwnPropertyDescriptor(l, k); \
           return [typeof x.get, typeof x.set, x.writable, x.enumerable, x.configurable].join(); \
         }; \
         const before = [d('list'), d('label')].join(';'); \
         l.list; l.label; \
         [before, [d('list'), d('label')].join(';')].join('|')",
    );
    let shape = "undefined,undefined,true,true,true;undefined,undefined,false,true,true";
    assert_eq!(desc, format!("{shape}|{shape}"));
}

#[test]
fn lazy_property_is_own_and_not_on_prototype() {
    let (result, _) = eval_lazy::<String>(
        "[Object.hasOwn(l, 'list'), 'list' in Object.getPrototypeOf(l)].join()",
    );
    assert_eq!(result, "true,false");
}

#[test]
fn writable_lazy_property_takes_assignments_without_running_the_getter() {
    let (value, lazy) = eval_lazy::<String>("'use strict'; l.list = 'mine'; l.list");
    assert_eq!(value, "mine");
    assert_eq!(lazy.list_calls.get(), 0);
}

#[test]
fn writable_lazy_property_takes_assignments_after_first_read() {
    let (value, lazy) = eval_lazy::<String>("'use strict'; l.list; l.list = 'mine'; l.list");
    assert_eq!(value, "mine");
    assert_eq!(lazy.list_calls.get(), 1);
}

#[test]
fn readonly_lazy_property_rejects_assignment() {
    let (result, _) = eval_lazy::<String>(
        "let thrown; \
         try { (() => { 'use strict'; l.label = 'x'; })(); } catch (e) { thrown = e; } \
         l.label = 'y'; \
         [thrown instanceof TypeError, l.label].join()",
    );
    assert_eq!(result, "true,lazy");
}

#[test]
fn lazy_getter_error_is_thrown_and_not_cached() {
    let lazy = jsg::Rc::new(Lazy::new());
    lazy.fail.set(true);
    let resource = lazy.clone();
    Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "l", lazy.clone());
        let thrown: String = ctx
            .eval(
                lock,
                "try { l.label; 'no error' } catch (e) { `${e.name}: ${e.message}` }",
            )
            .unwrap();
        assert_eq!(thrown, "RangeError: no label");
        // A failed read leaves the property lazy, so the next read runs the getter again.
        lazy.fail.set(false);
        let label: String = ctx.eval(lock, "l.label").unwrap();
        assert_eq!(label, "lazy");
        let cached: String = ctx.eval(lock, "l.label").unwrap();
        assert_eq!(cached, "lazy");
        Ok(())
    });
    assert_eq!(resource.label_calls.get(), 2);
}

#[jsg_resource]
struct Mixed {}

#[jsg_resource]
impl Mixed {
    #[jsg_property(instance)]
    pub fn get_id(&self) -> String {
        "id".to_owned()
    }

    #[jsg_property(instance, readonly)]
    pub fn get_kind(&self) -> String {
        "kind".to_owned()
    }

    #[jsg_property(lazy_instance, readonly)]
    pub fn get_tags(&self) -> String {
        "tags".to_owned()
    }
}

#[test]
fn lazy_and_native_instance_properties_enumerate_in_reverse_registration_order() {
    Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "m", jsg::Rc::new(Mixed {}));
        let keys: String = ctx.eval(lock, "Object.keys(m).join()").unwrap();
        assert_eq!(keys, "tags,kind,id");
        Ok(())
    });
}

// =============================================================================
// Custom registration
// =============================================================================

#[jsg_resource]
struct Placed {
    calls: Cell<u32>,
}

#[jsg_resource(custom_members)]
impl Placed {
    #[jsg_property(readonly)]
    pub fn get_value(&self) -> String {
        self.calls.set(self.calls.get() + 1);
        "placed".to_owned()
    }
}

impl Resource for Placed {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        let placement = if flags.get_jsg_property_on_prototype_template() {
            PropertyPlacement::Prototype
        } else {
            PropertyPlacement::LazyInstance
        };
        registry.add(Self::get_value_member(placement));
    }
}

fn eval_placed(harness: &Harness, code: &str) -> (String, u32) {
    let placed = jsg::Rc::new(Placed {
        calls: Cell::new(0),
    });
    let resource = placed.clone();
    let mut out = String::new();
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(lock, "p", placed);
        out = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    (out, resource.calls.get())
}

#[test]
fn custom_members_select_lazy_instance_placement() {
    let code = "[p.value, p.value, Object.hasOwn(p, 'value'), \
                Object.getOwnPropertyDescriptor(p, 'value')?.writable].join()";

    // The getter has `readonly`, which a lazy placement keeps.
    let (result, calls) = eval_placed(&Harness::new(), code);
    assert_eq!(result, "placed,placed,true,false");
    assert_eq!(calls, 1);

    let harness = Harness::with_flags(|mut f| f.set_jsg_property_on_prototype_template(true));
    let (result, calls) = eval_placed(&harness, code);
    assert_eq!(result, "placed,placed,false,");
    assert_eq!(calls, 2);
}

#[jsg_resource]
struct Settable {
    value: Cell<i32>,
}

#[jsg_resource(custom_members)]
impl Settable {
    #[jsg_property]
    pub fn get_value(&self) -> i32 {
        self.value.get()
    }

    #[jsg_property]
    pub fn set_value(&self, value: i32) {
        self.value.set(value);
    }
}

impl Resource for Settable {
    fn members(registry: &mut impl MemberRegistry, _flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::get_value_member(PropertyPlacement::LazyInstance));
    }
}

#[test]
#[should_panic(expected = "a lazy instance property has no setter")]
fn custom_members_reject_lazy_instance_placement_with_setter() {
    Harness::new().run_in_context(|lock, _ctx| {
        jsg::resource::function_template_of::<Settable>(lock);
        Ok(())
    });
}
