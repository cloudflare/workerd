// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for a `#[jsg_method]` callback registered in a member that V8 installs without the
//! type's `v8::Signature`, which safe code can do with `custom_members`. A receiver that is not a
//! wrapper of the type throws `Illegal invocation`.

use jsg::ExceptionType;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use super::cpp_interop::new_cpp;
use crate::cpp_interop::ffi;

#[jsg_resource]
struct Reused {
    value: u32,
}

#[jsg_resource(custom_members)]
impl Reused {
    #[jsg_method]
    fn value(&self) -> u32 {
        self.value
    }
}

impl jsg::Resource for Reused {
    fn members(
        registry: &mut impl jsg::MemberRegistry,
        _flags: jsg::feature_flags::compatibility_flags::Reader<'_>,
    ) {
        registry.add(jsg::resource::undescribed(jsg::Member::Property {
            name: "v".to_owned(),
            kind: jsg::PropertyKind::Prototype,
            getter_callback: Self::value_callback(),
            setter_callback: None,
        }));
        registry.add(jsg::resource::undescribed(jsg::Member::StaticMethod {
            name: "value".to_owned(),
            callback: Self::value_callback(),
            fast: None,
            length: 0,
        }));
    }
}

#[jsg_resource]
struct Other {}

#[jsg_resource]
impl Other {}

/// Calls the `v` getter and the static `value` method, both a `Reused`'s method callback, on
/// receivers that are and are not a `Reused`.
#[test]
fn method_callback_without_a_signature_checks_its_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let reused = jsg::Rc::new(lock, Reused { value: 7 }).to_js(lock)?;
        ctx.set_global(lock, "r", reused);
        let other = jsg::Rc::new(lock, Other {}).to_js(lock)?;
        ctx.set_global(lock, "other", other);
        // SAFETY: the constructor is called with the lock's isolate, in its scope.
        let greeter = new_cpp(lock, |isolate| unsafe {
            ffi::test_cpp_greeter_new(isolate, "Hello")
        });
        ctx.set_global(lock, "greeter", greeter);
        ctx.eval_raw(
            lock,
            "globalThis.getter = \
             Object.getOwnPropertyDescriptor(Object.getPrototypeOf(r), 'v').get; \
             globalThis.value = Object.getPrototypeOf(r).constructor.value",
        )
        .unwrap();

        assert_eq!(ctx.eval::<u32>(lock, "r.v").unwrap(), 7);
        assert_eq!(ctx.eval::<u32>(lock, "value.call(r)").unwrap(), 7);
        for code in [
            "getter.call({})",
            "getter.call(5)",
            "getter.call(undefined)",
            "getter.call(other)",
            "getter.call(greeter)",
            "value()",
            "value.call({})",
        ] {
            let err = ctx
                .eval::<u32>(lock, code)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert_eq!(
                err.name,
                ExceptionType::TypeError,
                "{code}: {}",
                err.message
            );
            assert!(
                err.message.contains("Illegal invocation"),
                "{code}: {}",
                err.message
            );
        }
        Ok(())
    });
}
