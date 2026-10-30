// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `#[jsg_resource(nested_types(...))]`.

use std::panic::AssertUnwindSafe;

use jsg::ToJS;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalContext;

#[jsg_resource]
struct Inner {}

#[jsg_resource]
impl Inner {}

#[jsg_resource(name = "RenamedInner")]
struct Renamed {}

#[jsg_resource]
impl Renamed {}

#[jsg_resource]
struct Constructible {
    value: String,
}

#[jsg_resource]
impl Constructible {
    #[jsg_constructor]
    fn constructor(value: String) -> Self {
        Self { value }
    }

    #[jsg_method]
    fn get_value(&self) -> String {
        self.value.clone()
    }
}

#[jsg_resource]
struct Outer {}

#[jsg_resource(nested_types(Inner, Alias = Renamed, Renamed, Constructible))]
impl Outer {
    #[jsg_method]
    fn make_inner(&self) -> jsg::Rc<Inner> {
        jsg::Rc::new(Inner {})
    }
}

fn set_outer(lock: &mut jsg::Lock, ctx: &EvalContext) {
    let outer = jsg::Rc::new(Outer {}).to_js(lock).unwrap();
    ctx.set_global(lock, "outer", outer);
    let constructor = jsg::resource::function_template_of::<Outer>(lock);
    ctx.set_global(lock, "Outer", constructor);
}

/// A nested type is reachable from instances, named after its class name.
#[test]
fn nested_type_is_reachable_from_instance() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(lock, "typeof outer.Inner + ',' + outer.Inner.name")
            .unwrap();
        assert_eq!(result, "function,Inner");
        Ok(())
    });
}

/// A nested type is a property of the prototype, not of the constructor or the
/// instance.
#[test]
fn nested_type_is_prototype_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(
                lock,
                "[Object.hasOwn(Outer.prototype, 'Inner'), Object.hasOwn(outer, 'Inner'), \
                 Object.hasOwn(Outer, 'Inner')].join()",
            )
            .unwrap();
        assert_eq!(result, "true,false,false");
        Ok(())
    });
}

/// A nested type is the same constructor as the type's own constructor, so
/// `instanceof` works with instances created from Rust.
#[test]
fn nested_type_is_type_constructor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let inner = jsg::resource::function_template_of::<Inner>(lock);
        ctx.set_global(lock, "Inner", inner);
        let result: String = ctx
            .eval(
                lock,
                "[outer.Inner === Inner, outer.makeInner() instanceof outer.Inner].join()",
            )
            .unwrap();
        assert_eq!(result, "true,true");
        Ok(())
    });
}

/// A nested type without a constructor throws "Illegal constructor".
#[test]
fn nested_type_without_constructor_throws() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(
                lock,
                "try { new outer.Inner(); 'no error' } catch (e) { e.constructor.name + ': ' + e.message }",
            )
            .unwrap();
        assert_eq!(result, "TypeError: Illegal constructor");
        Ok(())
    });
}

/// A nested type with a constructor can be constructed.
#[test]
fn nested_type_with_constructor_constructs() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(lock, "new outer.Constructible('hi').getValue()")
            .unwrap();
        assert_eq!(result, "hi");
        Ok(())
    });
}

/// A nested type can be given a property name other than its class name.
#[test]
fn nested_type_with_name() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let renamed = jsg::resource::function_template_of::<Renamed>(lock);
        ctx.set_global(lock, "RenamedInner", renamed);
        let result: String = ctx
            .eval(
                lock,
                "[outer.Alias === RenamedInner, outer.RenamedInner === RenamedInner, \
                 'Renamed' in outer].join()",
            )
            .unwrap();
        assert_eq!(result, "true,true,false");
        Ok(())
    });
}

/// V8 names a function after the property it is first instantiated through, so
/// the `name` of a renamed nested type's constructor is the property name when
/// the constructor is first reached through that property.
#[test]
fn nested_type_with_name_first_reached_by_name() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(lock, "outer.Alias.name + ',' + outer.RenamedInner.name")
            .unwrap();
        assert_eq!(result, "Alias,Alias");
        Ok(())
    });
}

/// A nested type's constructor is the type's constructor when the type's
/// template was created before the template of the type nesting it.
#[test]
fn nested_type_created_before_outer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let inner = jsg::resource::function_template_of::<Inner>(lock);
        ctx.set_global(lock, "Inner", inner);
        set_outer(lock, ctx);
        let result: String = ctx
            .eval(
                lock,
                "[outer.Inner === Inner, outer.makeInner() instanceof Inner, Inner.name].join()",
            )
            .unwrap();
        assert_eq!(result, "true,true,Inner");
        Ok(())
    });
}

#[jsg_resource]
struct SelfNesting {}

#[jsg_resource(nested_types(SelfNesting))]
impl SelfNesting {}

#[jsg_resource]
struct CycleA {}

#[jsg_resource(nested_types(CycleB))]
impl CycleA {}

#[jsg_resource]
struct CycleB {}

#[jsg_resource(nested_types(CycleA))]
impl CycleB {}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    payload
        .downcast_ref::<&str>()
        .map(ToString::to_string)
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_default()
}

/// Creating the template of a resource type that nests itself panics, every
/// time.
#[test]
fn nested_type_cycle_panics() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let path = |names: &[&str]| {
            names
                .iter()
                .map(|name| format!("jsg_test::tests::nested_type::{name}"))
                .collect::<Vec<_>>()
                .join(" -> ")
        };
        for _ in 0..2 {
            let payload = std::panic::catch_unwind(AssertUnwindSafe(|| {
                jsg::resource::function_template_of::<SelfNesting>(lock);
            }))
            .expect_err("a type nesting itself must panic");
            assert_eq!(
                panic_message(&*payload),
                format!(
                    "resource type nests itself: {}",
                    path(&["SelfNesting", "SelfNesting"])
                )
            );

            let payload = std::panic::catch_unwind(AssertUnwindSafe(|| {
                jsg::resource::function_template_of::<CycleA>(lock);
            }))
            .expect_err("types nesting each other must panic");
            assert_eq!(
                panic_message(&*payload),
                format!(
                    "resource type nests itself: {}",
                    path(&["CycleA", "CycleB", "CycleA"])
                )
            );
        }
        Ok(())
    });
}
