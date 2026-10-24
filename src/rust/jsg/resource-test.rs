// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

struct Bad;

impl Type for Bad {
    fn class_name() -> &'static str {
        "Bad"
    }

    fn is_exact(_value: v8::Local<'_, v8::Value>) -> bool {
        false
    }
}

impl crate::Traced for Bad {}

impl GarbageCollected for Bad {
    fn memory_name(&self) -> &'static std::ffi::CStr {
        c"Bad"
    }
}

extern "C" fn getter(_args: *mut v8::ffi::FunctionCallbackInfo) {}

impl Resource for Bad {
    fn members(_flags: compatibility_flags::Reader<'_>) -> Vec<Member> {
        vec![Member::Property {
            name: "bad".to_owned(),
            kind: crate::PropertyKind::Instance,
            getter_callback: getter,
            setter_callback: None,
        }]
    }
}

#[test]
#[should_panic(expected = "use Member::InstanceProperty")]
fn instance_kind_property_panics() {
    let message = capnp::message::Builder::new_default();
    get_resource_descriptor::<Bad>(message.get_root_as_reader().unwrap());
}
