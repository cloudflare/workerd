// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The value a resource's method or property returns is converted for its creator: the
//! resource's wrapper for an instance member, none for a static one. Containers pass the creator
//! on to what they hold.

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

/// Converts to the creator it is converted for, or to `undefined` if there is none.
struct CreatorOf;

impl<'s> ToJS<'s> for CreatorOf {
    fn to_js(self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        self.to_js_with_creator(lock, None)
    }

    fn to_js_with_creator(
        self,
        lock: &mut jsg::Lock<'s>,
        creator: Option<v8::Local<'s, v8::Object>>,
    ) -> jsg::Result<v8::Local<'s, v8::Value>> {
        Ok(creator.map_or_else(|| lock.undefined(), Into::into))
    }
}

/// Converts to an object or `undefined`, depending on the call.
impl jsg::TypeInfo for CreatorOf {
    fn describe(_cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
        jsg::rtti::Type::Unknown
    }
}

#[jsg_resource]
struct Creator;

#[jsg_resource]
#[expect(
    clippy::unnecessary_wraps,
    clippy::unused_self,
    reason = "the members exist to return their value through each kind of callback"
)]
impl Creator {
    #[jsg_method]
    fn direct(&self) -> CreatorOf {
        CreatorOf
    }

    #[jsg_method]
    fn fallible(&self) -> jsg::Result<CreatorOf> {
        Ok(CreatorOf)
    }

    #[jsg_method]
    fn optional(&self) -> Option<CreatorOf> {
        Some(CreatorOf)
    }

    #[jsg_method]
    fn many(&self) -> Vec<jsg::Nullable<CreatorOf>> {
        vec![jsg::Nullable::Some(CreatorOf)]
    }

    #[jsg_method]
    fn with_this(this: jsg::Rc<Self>) -> CreatorOf {
        let _ = this;
        CreatorOf
    }

    #[jsg_method]
    fn unbound() -> CreatorOf {
        CreatorOf
    }

    #[jsg_property(prototype, readonly)]
    fn get_property(&self) -> CreatorOf {
        CreatorOf
    }

    #[jsg_wildcard_property]
    fn get(&self, name: &str) -> Option<CreatorOf> {
        (name == "wildcard").then_some(CreatorOf)
    }
}

#[test]
fn instance_members_convert_for_their_receiver() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let creator = jsg::Rc::new(lock, Creator);
        ctx.set_global(lock, "creator", creator);
        for expression in [
            "creator.direct() === creator",
            "creator.fallible() === creator",
            "creator.optional() === creator",
            "creator.many()[0] === creator",
            "creator.withThis() === creator",
            "creator.property === creator",
            "creator.wildcard === creator",
        ] {
            assert!(ctx.eval::<bool>(lock, expression).unwrap(), "{expression}");
        }
        Ok(())
    });
}

#[test]
fn static_methods_convert_for_no_creator() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Creator>(lock).unwrap();
        ctx.set_global(lock, "Creator", constructor);
        assert!(
            ctx.eval::<bool>(lock, "Creator.unbound() === undefined")
                .unwrap()
        );
        Ok(())
    });
}
