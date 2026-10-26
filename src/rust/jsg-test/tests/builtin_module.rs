// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A builtin module's callback produces its exports resource when JavaScript imports it, and
//! an error it returns is thrown to the importer.

use std::pin::Pin;

use jsg::modules::ModuleType;
use jsg::v8;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Exports;

#[jsg_resource]
impl Exports {
    #[jsg_property(prototype, readonly)]
    fn get_value(&self) -> String {
        "exported".to_owned()
    }
}

fn register(mut registry: Pin<&mut v8::ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(
        registry.as_mut(),
        "test:exports",
        |_lock| Ok(jsg::Rc::new(Exports)),
        ModuleType::Internal,
    );
    jsg::modules::add_builtin(
        registry,
        "test:fails",
        |_lock| -> jsg::Result<jsg::Rc<Exports>> {
            Err(jsg::Error::new_type_error("the module failed"))
        },
        ModuleType::Internal,
    );
}

#[test]
fn importing_a_builtin_gives_its_exports() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let exports = ctx.import_builtin(lock, register, "test:exports").unwrap();
        ctx.set_global(lock, "exports", exports);
        assert_eq!(
            ctx.eval::<String>(lock, "exports.value").unwrap(),
            "exported"
        );
        Ok(())
    });
}

#[test]
fn importing_a_builtin_throws_what_its_callback_returns() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let Err(crate::EvalError::Exception(exception)) =
            ctx.import_builtin(lock, register, "test:fails")
        else {
            panic!("importing the module did not throw");
        };
        ctx.set_global(lock, "exception", exception);
        assert_eq!(
            ctx.eval::<String>(lock, "`${exception.name}: ${exception.message}`")
                .unwrap(),
            "TypeError: the module failed"
        );
        Ok(())
    });
}
