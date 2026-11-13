// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `#[jsg_resource(inherit = Parent)]` (`MemberRegistry::inherit`): the templates of
//! a child and its parent, which C++ JSG's `JSG_INHERIT` also links. The parent's callbacks
//! resolve their receiver by its exact type, so they reject the child's instances.

use std::panic::AssertUnwindSafe;

use jsg::Intrinsic;
use jsg::MemberRegistry;
use jsg::ParentType;
use jsg::Resource;
use jsg::ToJS;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_callable;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;
use jsg_macros::jsg_wildcard_property;

use crate::EvalContext;

#[jsg_resource]
struct Base;

#[jsg_resource]
impl Base {
    #[jsg_static_constant]
    const BASE_CONSTANT: i32 = 1;

    #[jsg_method]
    fn base_method(&self) -> String {
        let _ = self;
        "base".to_owned()
    }

    #[jsg_property(prototype, readonly)]
    fn get_base_property(&self) -> String {
        let _ = self;
        "base".to_owned()
    }

    #[jsg_wildcard_property]
    fn base_wildcard(&self, name: &str) -> Option<String> {
        let _ = self;
        (name == "fromWildcard").then(|| "base".to_owned())
    }

    #[jsg_callable]
    fn call(&self) -> String {
        let _ = self;
        "called".to_owned()
    }
}

#[jsg_resource]
struct Child;

#[jsg_resource(inherit = Base)]
impl Child {
    #[jsg_method]
    fn child_method(&self) -> String {
        let _ = self;
        "child".to_owned()
    }
}

/// A child with a wildcard property of its own.
#[jsg_resource]
struct OwnWildcard;

#[jsg_resource(inherit = Base)]
impl OwnWildcard {
    #[jsg_wildcard_property]
    fn own_wildcard(&self, name: &str) -> Option<String> {
        let _ = self;
        (name == "fromWildcard").then(|| "own".to_owned())
    }
}

/// A child of a child.
#[jsg_resource]
struct Grandchild;

#[jsg_resource(inherit = Child)]
impl Grandchild {}

fn set_globals(lock: &mut jsg::Lock, ctx: &EvalContext) {
    let base = jsg::Rc::new(lock, Base).to_js(lock).unwrap();
    ctx.set_global(lock, "base", base);
    let child = jsg::Rc::new(lock, Child).to_js(lock).unwrap();
    ctx.set_global(lock, "child", child);
    let own = jsg::Rc::new(lock, OwnWildcard).to_js(lock).unwrap();
    ctx.set_global(lock, "own", own);
    let grandchild = jsg::Rc::new(lock, Grandchild).to_js(lock).unwrap();
    ctx.set_global(lock, "grandchild", grandchild);
    let constructor = jsg::resource::function_template_of::<Base>(lock).unwrap();
    ctx.set_global(lock, "Base", constructor);
    let constructor = jsg::resource::function_template_of::<Child>(lock).unwrap();
    ctx.set_global(lock, "Child", constructor);
    let constructor = jsg::resource::function_template_of::<Grandchild>(lock).unwrap();
    ctx.set_global(lock, "Grandchild", constructor);
}

fn eval(source: &str) -> String {
    let harness = crate::Harness::with_flags(|mut flags| flags.set_set_to_string_tag(true));
    let mut result = String::new();
    harness.run_in_context(|lock, ctx| {
        set_globals(lock, ctx);
        result = ctx.eval(lock, source).unwrap();
        Ok(())
    });
    result
}

/// The parent's prototype is the [[Prototype]] of the child's, so a child is an instance of
/// both, and keeps its own class name.
#[test]
fn child_prototype_inherits_parent_prototype() {
    assert_eq!(
        eval(
            "[Object.getPrototypeOf(Child.prototype) === Base.prototype, child instanceof Child, \
             child instanceof Base, base instanceof Child, Child.name, \
             Object.prototype.toString.call(child), child.constructor === Child].join()"
        ),
        "true,true,true,false,Child,[object Child],true"
    );
}

/// The chain is as deep as the types' inheritance.
#[test]
fn grandchild_inherits_through_child() {
    assert_eq!(
        eval(
            "[Object.getPrototypeOf(Grandchild.prototype) === Child.prototype, \
             grandchild instanceof Base, typeof grandchild.childMethod, \
             typeof grandchild.baseMethod].join()"
        ),
        "true,true,function,function"
    );
}

/// As with `FunctionTemplate::Inherit` in C++ JSG, statics stay on the parent's constructor,
/// whose [[Prototype]] the child's constructor does not become.
#[test]
fn statics_are_not_inherited() {
    assert_eq!(
        eval(
            "[Base.BASE_CONSTANT, 'BASE_CONSTANT' in Child, \
             Object.getPrototypeOf(Child) === Function.prototype, child.BASE_CONSTANT].join()"
        ),
        "1,false,true,1"
    );
}

/// The child's methods have the child's signature, which a parent's instance fails.
#[test]
fn child_method_rejects_parent_instance() {
    assert_eq!(
        eval(
            "try { Child.prototype.childMethod.call(base); 'no error' } \
             catch (e) { e.constructor.name + ': ' + e.message.startsWith('Illegal invocation') }"
        ),
        "TypeError: true"
    );
}

/// The parent's members reach a child's instance through the prototype chain, and V8's
/// signature check accepts it, but their callbacks resolve the receiver as exactly the parent
/// type, so they throw.
#[test]
fn parent_members_reject_child_receiver() {
    assert_eq!(
        eval(
            "const attempt = (f) => { try { return 'no error: ' + f(); } \
             catch (e) { return e.constructor.name + ': ' + e.message; } }; \
             [attempt(() => child.baseMethod()), attempt(() => child.baseProperty), \
             attempt(() => base.baseMethod()), attempt(() => base.baseProperty)].join('; ')"
        ),
        "TypeError: Illegal invocation; TypeError: Illegal invocation; no error: base; \
         no error: base"
    );
}

/// V8 does not inherit named-property handlers, so a child without a wildcard property of its
/// own gets its parent's, which it resolves the receiver of as the parent's own callbacks do. A
/// child's own wildcard property takes precedence.
#[test]
fn wildcard_property_is_inherited_unless_overridden() {
    assert_eq!(
        eval(
            "const attempt = (f) => { try { return String(f()); } \
             catch (e) { return e.constructor.name + ': ' + e.message; } }; \
             [attempt(() => base.fromWildcard), attempt(() => child.fromWildcard), \
             attempt(() => grandchild.fromWildcard), attempt(() => own.fromWildcard)].join('; ')"
        ),
        "base; TypeError: Illegal invocation; TypeError: Illegal invocation; own"
    );
}

/// A callable parent's call handler is not inherited: instances of the child are not
/// functions.
#[test]
fn callable_is_not_inherited() {
    assert_eq!(
        eval("[typeof base, base(), typeof child, typeof grandchild].join()"),
        "function,called,object,object"
    );
}

/// A child can nest its parent, as C++ `WorkerGlobalScope` nests `EventTarget`.
#[jsg_resource]
struct NestsParent;

#[jsg_resource(inherit = Base, nested_types(Base))]
impl NestsParent {}

#[test]
fn child_can_nest_parent() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = jsg::Rc::new(lock, NestsParent).to_js(lock).unwrap();
        ctx.set_global(lock, "value", value);
        let base = jsg::resource::function_template_of::<Base>(lock).unwrap();
        ctx.set_global(lock, "Base", base);
        let result: String = ctx
            .eval(
                lock,
                "[value.Base === Base, value instanceof value.Base].join()",
            )
            .unwrap();
        assert_eq!(result, "true,true");
        Ok(())
    });
}

/// The parent of an iterator inherits from `Iterator.prototype`, and so does the child.
#[jsg_resource]
struct IntrinsicParent;

#[jsg_resource(inherit_intrinsic = IteratorPrototype)]
impl IntrinsicParent {}

#[jsg_resource]
struct IntrinsicChild;

#[jsg_resource(inherit = IntrinsicParent)]
impl IntrinsicChild {}

#[test]
fn child_inherits_parent_intrinsic() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = jsg::Rc::new(lock, IntrinsicChild).to_js(lock).unwrap();
        ctx.set_global(lock, "value", value);
        let result: bool = ctx
            .eval(
                lock,
                "value instanceof Iterator && typeof value.map === 'function'",
            )
            .unwrap();
        assert!(result);
        Ok(())
    });
}

#[jsg_resource]
struct SelfParent;

#[jsg_resource(inherit = SelfParent)]
impl SelfParent {}

#[jsg_resource]
struct CycleA;

#[jsg_resource(inherit = CycleB)]
impl CycleA {}

#[jsg_resource]
struct CycleB;

#[jsg_resource(inherit = CycleA)]
impl CycleB {}

/// V8 instantiates a template's prototype, with the nested types on it, before it caches the
/// template's function, so a type nesting its own child would instantiate itself recursively.
#[jsg_resource]
struct NestsChild;

#[jsg_resource(nested_types(ChildOfNester))]
impl NestsChild {}

#[jsg_resource]
struct ChildOfNester;

#[jsg_resource(inherit = NestsChild)]
impl ChildOfNester {}

/// Inherits from both a type and an intrinsic.
#[jsg_resource]
struct TwoParents;

#[jsg_resource(custom_members)]
impl TwoParents {}

impl Resource for TwoParents {
    fn members(registry: &mut impl MemberRegistry, _flags: compatibility_flags::Reader<'_>) {
        registry.inherit(ParentType::of::<Base>());
        registry.inherit_intrinsic(Intrinsic::IteratorPrototype);
    }
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> String {
    payload
        .downcast_ref::<&str>()
        .map(ToString::to_string)
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_default()
}

fn path(names: &[&str]) -> String {
    names
        .iter()
        .map(|name| format!("jsg_test::tests::inherit::{name}"))
        .collect::<Vec<_>>()
        .join(" -> ")
}

/// Creating the template of a type that inherits from itself, or nests a type that inherits
/// from it, panics, every time and whichever type's template is asked for first.
#[test]
fn inheritance_cycle_panics() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut catch = |f: &mut dyn FnMut(&mut jsg::Lock)| {
            let payload = std::panic::catch_unwind(AssertUnwindSafe(|| f(lock)))
                .expect_err("an inheritance cycle must panic");
            panic_message(&*payload)
        };
        for _ in 0..2 {
            assert_eq!(
                catch(&mut |lock| {
                    jsg::resource::function_template_of::<SelfParent>(lock).unwrap();
                }),
                format!(
                    "resource type inherits from itself: {}",
                    path(&["SelfParent", "SelfParent"])
                )
            );
            assert_eq!(
                catch(&mut |lock| {
                    jsg::resource::function_template_of::<CycleA>(lock).unwrap();
                }),
                format!(
                    "resource type inherits from itself: {}",
                    path(&["CycleA", "CycleB", "CycleA"])
                )
            );
            assert_eq!(
                catch(&mut |lock| {
                    jsg::resource::function_template_of::<NestsChild>(lock).unwrap();
                }),
                format!(
                    "resource type nests a type that inherits from it: {}",
                    path(&["NestsChild", "ChildOfNester", "NestsChild"])
                )
            );
            assert_eq!(
                catch(&mut |lock| {
                    jsg::resource::function_template_of::<ChildOfNester>(lock).unwrap();
                }),
                format!(
                    "resource type nests a type that inherits from it: {}",
                    path(&["ChildOfNester", "NestsChild", "ChildOfNester"])
                )
            );
            assert_eq!(
                catch(&mut |lock| {
                    jsg::resource::function_template_of::<TwoParents>(lock).unwrap();
                }),
                "TwoParents inherits from more than one type or intrinsic"
            );
        }
        Ok(())
    });
}
