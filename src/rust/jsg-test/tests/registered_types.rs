// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A type registered with `jsg::modules::add_type` is described by the RTTI encoder although no
//! module exports it, and ignored by the registries that instantiate modules.

use std::any::type_name;
use std::pin::Pin;

use jsg::modules::ModuleType;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// A type the runtime would pass to JavaScript, such as a handler's argument.
#[jsg_resource(name = "RegisteredType")]
struct Registered;

#[jsg_resource]
impl Registered {
    #[jsg_method]
    fn ping(&self) -> String {
        "pong".to_owned()
    }
}

#[jsg_resource]
struct Exports;

#[jsg_resource]
impl Exports {}

fn register(mut registry: Pin<&mut v8::ffi::ModuleRegistry>) {
    jsg::modules::add_type::<Registered>(registry.as_mut());
    jsg::modules::add_builtin(
        registry,
        "test:registered-types",
        |lock| Ok(jsg::Rc::new(lock, Exports)),
        ModuleType::Internal,
    );
}

#[test]
fn the_rtti_encoder_describes_a_registered_type() {
    let bytes = crate::describe_registered_types(register);
    let message =
        capnp::serialize::read_message(&mut bytes.as_slice(), capnp::message::ReaderOptions::new())
            .unwrap();
    let root = message
        .get_root::<rtti_capnp::structure_groups::Reader>()
        .unwrap();
    let structures = root.get_groups().unwrap().get(0).get_structures().unwrap();
    // The describing registry ignores modules, so only the registered type is described.
    assert_eq!(structures.len(), 1);
    let structure = structures.get(0);
    assert_eq!(structure.get_name().unwrap(), "RegisteredType");
    // C++ RTTI refers to the structure by this identity (`JsgRttiRustStructure::IDENTITY`).
    assert_eq!(
        structure.get_fully_qualified_name().unwrap(),
        type_name::<Registered>()
    );
    // A registered type is not a TypeScript root of its own: it is declared when a root that
    // refers to it is.
    assert!(!structure.get_ts_root());
    let members = structure.get_members().unwrap();
    assert_eq!(members.len(), 1);
    let rtti_capnp::member::Method(method) = members.get(0).which().unwrap() else {
        panic!("expected a method");
    };
    assert_eq!(method.unwrap().get_name().unwrap(), "ping");
    assert_eq!(root.get_modules().unwrap().len(), 0);
}

#[test]
fn registries_that_instantiate_modules_ignore_registered_types() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let exports = ctx.import_builtin(lock, register, "test:registered-types");
        assert!(exports.is_ok());
        Ok(())
    });
}
