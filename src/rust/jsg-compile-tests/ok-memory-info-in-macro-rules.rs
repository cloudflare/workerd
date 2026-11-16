// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The `VisitForMemoryInfo` that `#[jsg_resource(memory_info)]` generates compiles for a struct
// declared by a `macro_rules!` macro, whose field types are tokens from the macro's caller: the
// generated code uses the type's span only to resolve the type, not its own locals.
#![deny(warnings)]

use jsg_macros::jsg_resource;

macro_rules! resource {
    ($name:ident, $t:tt) => {
        #[jsg_resource(memory_info)]
        pub struct $name {
            pub field: $t,
        }

        #[jsg_resource]
        impl $name {}
    };
}

resource!(Built, String);

pub fn t(lock: &mut jsg::Lock<'_>) -> jsg::Rc<Built> {
    jsg::Rc::new(
        lock,
        Built {
            field: String::new(),
        },
    )
}
