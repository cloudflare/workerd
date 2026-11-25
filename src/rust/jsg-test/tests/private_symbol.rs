// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `v8::Private::for_api`, C++ `v8::Private::ForApi()`, and the property key that
//! `Local<Private>::to_property_key` makes of a private symbol.

use jsg::v8;

fn key<'s>(lock: &mut jsg::Lock<'s>, name: &str) -> v8::Local<'s, v8::Value> {
    let name = v8::String::new_from_str(lock, name).unwrap();
    v8::Private::for_api(lock, name)
        .to_property_key(lock)
        .unwrap()
}

#[test]
fn equal_names_give_the_same_key() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let a = key(lock, "test:a");
        ctx.set_global(lock, "a1", a);
        let a = key(lock, "test:a");
        ctx.set_global(lock, "a2", a);
        let b = key(lock, "test:b");
        ctx.set_global(lock, "b", b);
        assert!(ctx.eval::<bool>(lock, "a1 === a2 && a1 !== b").unwrap());
        assert_eq!(ctx.eval::<String>(lock, "typeof a1").unwrap(), "symbol");
        Ok(())
    });
}

#[test]
fn key_is_the_one_cpp_reaches_by_the_same_name() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let rust = key(lock, "test:shared");
        ctx.set_global(lock, "rust", rust);
        let cpp = crate::Harness::private_symbol(lock, "test:shared");
        ctx.set_global(lock, "cpp", cpp);
        assert!(ctx.eval::<bool>(lock, "rust === cpp").unwrap());
        Ok(())
    });
}

#[test]
fn private_property_is_hidden() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let k = key(lock, "test:hidden");
        ctx.set_global(lock, "k", k);
        assert_eq!(
            ctx.eval::<String>(
                lock,
                "const o = {};
                 o[k] = 1;
                 [o[k], Reflect.ownKeys(o).length, Object.create(o)[k], JSON.stringify(o)].join()",
            )
            .unwrap(),
            "1,0,,{}"
        );
        // A private symbol is never seen by a Proxy trap.
        assert!(
            ctx.eval::<bool>(
                lock,
                "new Proxy({}, { get() { return 'trapped'; } })[k] === undefined",
            )
            .unwrap()
        );
        Ok(())
    });
}
