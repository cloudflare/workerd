// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_wildcard_property]` behaves as C++ JSG's `JSG_WILDCARD_PROPERTY`. The first test
//! mirrors the "Named interceptor" test in `src/workerd/jsg/jsg-test.c++` case for case.

use std::cell::RefCell;

use jsg::ExceptionType;
use jsg::Member;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

/// `ProxyImpl` of the C++ test.
#[jsg_resource]
struct ProxyImpl {
    foo: String,
}

#[jsg_resource]
impl ProxyImpl {
    #[jsg_property(prototype, readonly)]
    pub fn get_bar(&self) -> i32 {
        123
    }

    #[jsg_wildcard_property]
    pub fn test_get_named(&self, name: &str) -> Result<Option<String>, jsg::Error> {
        match name {
            "foo" => Ok(Some(self.foo.clone())),
            "abc" => Err(jsg::Error::new_type_error("boom")),
            // Properties of the object and its prototypes take precedence.
            "bar" | "toString" => Ok(Some("from the wildcard".to_owned())),
            _ => Ok(None),
        }
    }
}

/// Has every property but `none`, which is its prefix followed by the property name.
#[jsg_resource]
struct Echo {
    prefix: String,
}

#[jsg_resource]
impl Echo {
    #[jsg_wildcard_property]
    pub fn get(&self, lock: &mut jsg::Lock, mut name: String) -> Option<String> {
        let _ = lock;
        if name == "none" {
            return None;
        }
        name.insert_str(0, &self.prefix);
        Some(name)
    }
}

/// Calls the function it was given when its `call` property is read, and then fails with a
/// new error, as an error that replaces whatever the call threw.
#[jsg_resource]
struct Caller {
    callback: RefCell<Option<v8::TracedGlobal<v8::Function>>>,
}

#[jsg_resource]
impl Caller {
    #[jsg_method]
    pub fn set_callback(
        &self,
        lock: &mut jsg::Lock,
        callback: v8::Local<'_, v8::Value>,
    ) -> Result<(), jsg::Error> {
        let callback = v8::Local::<v8::Function>::try_from(callback)
            .map_err(|_| jsg::Error::new_type_error("not a function"))?;
        *self.callback.borrow_mut() = Some(v8::TracedGlobal::new(lock, callback));
        Ok(())
    }

    #[jsg_wildcard_property]
    pub fn get(&self, lock: &mut jsg::Lock, name: &str) -> Result<Option<i32>, jsg::Error> {
        if name != "call" {
            return Ok(None);
        }
        let callback = self.callback.borrow().as_ref().map(|f| f.as_local(lock));
        if let Some(callback) = callback {
            let _ = callback.call(lock, None, &[]);
        }
        Err(jsg::Error::new_type_error("replaced"))
    }
}

/// Panics when its `trigger` property is read.
#[jsg_resource]
struct Panicker {
    trigger: String,
}

#[jsg_resource]
impl Panicker {
    #[jsg_wildcard_property]
    pub fn get(&self, name: &str) -> Option<i32> {
        assert!(
            name != self.trigger,
            "intentional panic in a wildcard property"
        );
        None
    }

    #[jsg_method]
    pub fn panic_now(&self) {
        let _ = self.get(&self.trigger);
    }
}

fn panicker() -> jsg::Rc<Panicker> {
    jsg::Rc::new(Panicker {
        trigger: "panic".to_owned(),
    })
}

/// Has one of two wildcard properties, depending on a compatibility flag.
#[jsg_resource]
struct Versioned;

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_wildcard_property]
    pub fn get_old(&self, name: &str) -> Option<String> {
        let _ = self;
        (name != "none").then(|| format!("old {name}"))
    }

    #[jsg_wildcard_property]
    pub fn get_new(&self, name: &str) -> Option<String> {
        let _ = self;
        (name != "none").then(|| format!("new {name}"))
    }
}

impl Resource for Versioned {
    fn members(flags: compatibility_flags::Reader<'_>) -> Vec<Member> {
        if flags.get_cache_option_enabled() {
            vec![Self::get_new_member()]
        } else {
            vec![Self::get_old_member()]
        }
    }
}

fn with_proxy(f: impl for<'s, 'c> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'c>)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(
            lock,
            "p",
            jsg::Rc::new(ProxyImpl {
                foo: "bar".to_owned(),
            }),
        );
        ctx.set_global(
            lock,
            "echo",
            jsg::Rc::new(Echo {
                prefix: "echo ".to_owned(),
            }),
        );
        f(lock, ctx);
        Ok(())
    });
}

fn expect_eval(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str, expected: &str) {
    let result: String = ctx.eval(lock, code).unwrap();
    assert_eq!(result, expected, "{code}");
}

fn expect_type_error(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    let err = ctx
        .eval::<String>(lock, code)
        .unwrap_err()
        .unwrap_jsg_err(lock);
    assert_eq!(err.name, ExceptionType::TypeError, "{code}");
    err.message
}

// =============================================================================
// jsg-test.c++ "Named interceptor"
// =============================================================================

#[test]
fn named_interceptor() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "String(p.bar)", "123");
        expect_eval(lock, ctx, "String(Reflect.has(p, 'foo'))", "true");
        expect_eval(lock, ctx, "String(p.hasOwnProperty('foo'))", "false");
        expect_eval(
            lock,
            ctx,
            "String(Object.getOwnPropertyDescriptor(p, 'foo'))",
            "undefined",
        );
        expect_eval(lock, ctx, "String(Reflect.has(p, 'bar'))", "true");
        expect_eval(lock, ctx, "String(Reflect.has(p, 'baz'))", "false");
        assert_eq!(expect_type_error(lock, ctx, "p.abc"), "boom");
    });
}

// =============================================================================
// Lookup
// =============================================================================

#[test]
fn returns_the_value_or_undefined() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "p.foo", "bar");
        expect_eval(lock, ctx, "String(p.baz)", "undefined");
        expect_eval(lock, ctx, "echo.anything", "echo anything");
        expect_eval(lock, ctx, "echo['with space']", "echo with space");
        expect_eval(lock, ctx, "String(echo.none)", "undefined");
        expect_eval(lock, ctx, "String('none' in echo)", "false");
        // An integer key is an indexed property, which a wildcard property doesn't cover, as in
        // C++ JSG.
        expect_eval(lock, ctx, "String(echo[1])", "undefined");
        expect_eval(lock, ctx, "echo['1.5']", "echo 1.5");
    });
}

#[test]
fn properties_of_the_object_and_its_prototypes_take_precedence() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "String(p.bar)", "123");
        expect_eval(lock, ctx, "typeof p.toString", "function");
        expect_eval(lock, ctx, "typeof echo.hasOwnProperty", "function");
        expect_eval(lock, ctx, "typeof echo.constructor", "function");
    });
}

#[test]
fn symbols_are_not_intercepted() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "String(echo[Symbol('x')])", "undefined");
        expect_eval(lock, ctx, "String(echo[Symbol.iterator])", "undefined");
        expect_eval(lock, ctx, "String(Symbol.iterator in echo)", "false");
    });
}

#[test]
fn wildcard_properties_are_not_own_properties() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "JSON.stringify(Object.keys(echo))", "[]");
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(Object.getOwnPropertyNames(echo))",
            "[]",
        );
        expect_eval(lock, ctx, "String(Object.hasOwn(echo, 'x'))", "false");
    });
}

// Assigning a property defines an own property, which then takes precedence. Assigning asks
// the query whether the property exists, so the method is called, and its error thrown.
#[test]
fn assignment_defines_an_own_property() {
    with_proxy(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "echo.x = 1; [String(echo.x), JSON.stringify(Object.keys(echo))].join()",
            "1,[\"x\"]",
        );
        expect_eval(lock, ctx, "p.foo = 'set'; p.foo", "set");
        assert_eq!(expect_type_error(lock, ctx, "p.abc = 1"), "boom");
        expect_eval(
            lock,
            ctx,
            "Object.defineProperty(p, 'abc', { value: 'defined' }); p.abc",
            "defined",
        );
    });
}

#[test]
fn delete_and_enumeration_ignore_wildcard_properties() {
    with_proxy(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "[String(delete echo.y), echo.y].join()",
            "true,echo y",
        );
        expect_eval(lock, ctx, "JSON.stringify({ ...echo })", "{}");
        expect_eval(
            lock,
            ctx,
            "const keys = []; for (const key in echo) keys.push(key); JSON.stringify(keys)",
            "[]",
        );
    });
}

// The query reports no attributes, so a wildcard property is enumerable, as in C++ JSG, though
// it isn't an own property.
#[test]
fn wildcard_property_is_enumerable() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "String(echo.propertyIsEnumerable('y'))", "true");
        expect_eval(
            lock,
            ctx,
            "String(echo.propertyIsEnumerable('none'))",
            "false",
        );
    });
}

// A wildcard property named `then` makes the object a thenable, so `await` calls it.
#[test]
fn then_is_a_wildcard_property_too() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "typeof echo.then", "string");
        expect_eval(lock, ctx, "String(p.then)", "undefined");
    });
}

#[test]
fn custom_members_choose_the_wildcard_property() {
    for (enabled, expected) in [(false, "old x"), (true, "new x")] {
        let harness =
            crate::Harness::with_flags(|mut flags| flags.set_cache_option_enabled(enabled));
        harness.run_in_context(|lock, ctx| {
            ctx.set_global(lock, "v", jsg::Rc::new(Versioned));
            expect_eval(lock, ctx, "v.x", expected);
            Ok(())
        });
    }
}

// V8 passes the object that has the interceptor, so an object that inherits from the resource
// reads its wildcard properties.
#[test]
fn inheriting_object_reads_the_wildcard_property() {
    with_proxy(|lock, ctx| {
        expect_eval(lock, ctx, "Object.create(p).foo", "bar");
        expect_eval(lock, ctx, "String('foo' in Object.create(p))", "true");
        expect_eval(lock, ctx, "Object.create(echo).x", "echo x");
    });
}

// =============================================================================
// Errors
// =============================================================================

// `in` asks the query callback, which calls the method too.
#[test]
fn error_is_thrown_from_reads_and_queries() {
    with_proxy(|lock, ctx| {
        assert_eq!(expect_type_error(lock, ctx, "p.abc"), "boom");
        assert_eq!(expect_type_error(lock, ctx, "'abc' in p"), "boom");
        assert_eq!(
            expect_type_error(lock, ctx, "Reflect.has(p, 'abc')"),
            "boom"
        );
        expect_eval(
            lock,
            ctx,
            "try { p.abc; 'none' } catch (e) { e instanceof TypeError ? 'TypeError' : 'other' }",
            "TypeError",
        );
    });
}

#[test]
fn lookups_after_an_error_are_unaffected() {
    with_proxy(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "try { p.abc } catch {} [p.foo, String(p.baz), String('abc' in Object.create({}))]\
             .join()",
            "bar,undefined,false",
        );
        expect_eval(
            lock,
            ctx,
            "try { 'abc' in p } catch {} [p.foo, String('foo' in p)].join()",
            "bar,true",
        );
    });
}

/// JavaScript that terminates execution: `panicNow()` throws an internal error and requests
/// termination, which takes effect at the next interrupt check, in the loop.
const TERMINATE: &str = "() => { try { panicker.panicNow(); } catch (e) {} while (true) {} }";

// An error that the method returns while execution is terminating is not thrown, so the
// termination keeps propagating through both the getter and the query.
#[test]
fn error_does_not_replace_termination() {
    for read in ["caller.call", "'call' in caller"] {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, ctx| {
            ctx.set_global(
                lock,
                "caller",
                jsg::Rc::new(Caller {
                    callback: RefCell::new(None),
                }),
            );
            ctx.set_global(lock, "panicker", panicker());
            let result = ctx.eval::<bool>(
                lock,
                &format!(
                    "caller.setCallback({TERMINATE}); try {{ {read}; false }} catch (e) {{ true }}"
                ),
            );
            assert!(
                result.is_err(),
                "{read}: the termination was caught: {result:?}"
            );
            Ok(())
        });
    }
}

// The panic throws an internal error, without the panic message.
#[test]
fn panic_is_an_internal_error() {
    for read in ["panicker.panic", "'panic' in panicker"] {
        let harness = crate::Harness::new();
        harness.run_in_context(|lock, ctx| {
            ctx.set_global(lock, "panicker", panicker());
            let err = ctx
                .eval::<String>(lock, read)
                .unwrap_err()
                .unwrap_jsg_err(lock);
            assert!(
                err.message.contains("internal error"),
                "{read}: {}",
                err.message
            );
            assert!(
                !err.message.contains("intentional panic"),
                "{read}: {}",
                err.message
            );
            Ok(())
        });
    }
}
