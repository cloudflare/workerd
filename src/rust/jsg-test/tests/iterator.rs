// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `#[jsg_iterable]` and `inherit_intrinsic` behave as C++ JSG's `JSG_ITERABLE` and
//! `JSG_INHERIT_INTRINSIC`. The first test mirrors "Iterables can be iterated" in
//! `src/workerd/jsg/resource-test.c++` case for case.

use std::cell::Cell;
use std::panic::AssertUnwindSafe;

use jsg::ExceptionType;
use jsg::Intrinsic;
use jsg::Member;
use jsg::MemberRegistry;
use jsg::Resource;
use jsg::feature_flags::compatibility_flags;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_iterable;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

fn panic_message(payload: &(dyn std::any::Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("<non-string panic payload>")
}

// =============================================================================
// resource-test.c++ `IterableContext`
// =============================================================================

const VALUES: [f64; 10] = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0];

/// `Iterable` of the C++ test.
#[jsg_resource]
struct Iterable;

#[jsg_resource]
impl Iterable {
    #[jsg_constructor]
    fn constructor() -> Self {
        Self
    }

    #[jsg_iterable]
    #[jsg_method]
    fn entries(this: jsg::Rc<Self>, lock: &mut jsg::Lock) -> jsg::Rc<NumberIterator> {
        jsg::Rc::new(
            lock,
            NumberIterator {
                parent: this,
                cursor: Cell::new(0),
            },
        )
    }
}

/// `{ done, value }`, as `next()` returns it. `value` is left out when it is `None`.
#[jsg_struct]
struct NextNumber {
    pub done: bool,
    pub value: Option<f64>,
}

/// `Iterable::Iterator` of the C++ test.
#[jsg_resource]
struct NumberIterator {
    parent: jsg::Rc<Iterable>,
    cursor: Cell<usize>,
}

#[jsg_resource(inherit_intrinsic = IteratorPrototype)]
impl NumberIterator {
    #[jsg_method]
    fn next(&self) -> NextNumber {
        let cursor = self.cursor.get();
        let Some(&value) = VALUES.get(cursor) else {
            return NextNumber {
                done: true,
                value: None,
            };
        };
        self.cursor.set(cursor + 1);
        NextNumber {
            done: false,
            value: Some(value),
        }
    }

    /// Makes the iterator iterable itself, so that `for (const n of iterable.entries())` works.
    #[jsg_iterable]
    fn iterator(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }
}

// =============================================================================
// An iterator shaped like HTMLRewriter's `Element.attributes`
// =============================================================================

/// `{ done, value?: [name, value] }`.
#[jsg_struct]
struct NextAttribute {
    pub done: bool,
    pub value: Option<Vec<String>>,
}

/// Iterates over name-value pairs, and fails once `invalidate()` is called, as
/// `Element::AttributesIterator` does when an attribute changes during iteration.
#[jsg_resource]
struct Attributes {
    /// `[name, value]` pairs.
    pairs: Vec<Vec<String>>,
    cursor: Cell<usize>,
    invalidated: Cell<bool>,
}

#[jsg_resource(inherit_intrinsic = IteratorPrototype)]
impl Attributes {
    #[jsg_method]
    fn next(&self) -> Result<NextAttribute, jsg::Error> {
        if self.invalidated.get() {
            return Err(jsg::Error::new_type_error(
                "The attributes were changed during iteration.",
            ));
        }
        let cursor = self.cursor.get();
        let Some(pair) = self.pairs.get(cursor) else {
            return Ok(NextAttribute {
                done: true,
                value: None,
            });
        };
        self.cursor.set(cursor + 1);
        Ok(NextAttribute {
            done: false,
            value: Some(pair.clone()),
        })
    }

    #[jsg_iterable]
    fn iterator(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }

    #[jsg_method]
    fn invalidate(&self) {
        self.invalidated.set(true);
    }
}

fn attributes(lock: &mut jsg::Lock) -> jsg::Rc<Attributes> {
    jsg::Rc::new(
        lock,
        Attributes {
            pairs: vec![
                vec!["class".to_owned(), "a b".to_owned()],
                vec!["id".to_owned(), "x".to_owned()],
                vec!["href".to_owned(), "/".to_owned()],
            ],
            cursor: Cell::new(0),
            invalidated: Cell::new(false),
        },
    )
}

// =============================================================================
// Other shapes
// =============================================================================

/// The next result of an iterator that counts down from `remaining` to 1.
fn count_down(remaining: &Cell<u32>) -> NextNumber {
    let Some(next) = remaining.get().checked_sub(1) else {
        return NextNumber {
            done: true,
            value: None,
        };
    };
    let value = f64::from(remaining.get());
    remaining.set(next);
    NextNumber {
        done: false,
        value: Some(value),
    }
}

/// Inherits `Iterator.prototype` without a `#[jsg_iterable]` of its own, so its
/// `[Symbol.iterator]()` is the one `Iterator.prototype` has.
#[jsg_resource]
struct Countdown {
    remaining: Cell<u32>,
}

#[jsg_resource(inherit_intrinsic = IteratorPrototype)]
impl Countdown {
    #[jsg_method]
    fn next(&self) -> NextNumber {
        count_down(&self.remaining)
    }
}

/// Iterable, but does not inherit `Iterator.prototype`, so it has no iterator helpers.
#[jsg_resource]
struct PlainIterator {
    remaining: Cell<u32>,
}

#[jsg_resource]
impl PlainIterator {
    #[jsg_method]
    fn next(&self) -> NextNumber {
        count_down(&self.remaining)
    }

    #[jsg_iterable]
    fn iterator(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }
}

/// Neither iterable nor an iterator.
#[jsg_resource]
struct Plain;

#[jsg_resource]
impl Plain {
    #[jsg_method]
    fn foo(&self) -> bool {
        let _ = self;
        true
    }
}

/// Iterable one of two ways, and an iterator only, depending on a compatibility flag.
#[jsg_resource]
struct Versioned {
    remaining: Cell<u32>,
}

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_method]
    fn next(&self) -> NextNumber {
        count_down(&self.remaining)
    }

    #[jsg_iterable]
    fn iterator(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }
}

impl Resource for Versioned {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        registry.add(Self::next_member());
        registry.add(Self::iterator_iterable_member());
        if flags.get_cache_option_enabled() {
            registry.inherit_intrinsic(Intrinsic::IteratorPrototype);
        }
    }
}

fn with_globals(f: impl for<'s, 'c> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'c>)) {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let iterable = jsg::resource::function_template_of::<Iterable>(lock).unwrap();
        ctx.set_global(lock, "Iterable", iterable);
        let attributes = attributes(lock);
        ctx.set_global(lock, "attributes", attributes);
        let countdown = jsg::Rc::new(
            lock,
            Countdown {
                remaining: Cell::new(3),
            },
        );
        ctx.set_global(lock, "countdown", countdown);
        let plain_iterator = jsg::Rc::new(
            lock,
            PlainIterator {
                remaining: Cell::new(3),
            },
        );
        ctx.set_global(lock, "plainIterator", plain_iterator);
        let plain = jsg::Rc::new(lock, Plain);
        ctx.set_global(lock, "plain", plain);
        f(lock, ctx);
        Ok(())
    });
}

fn expect_eval(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str, expected: &str) {
    let result: String = ctx.eval(lock, code).unwrap();
    assert_eq!(result, expected, "{code}");
}

fn expect_error(
    lock: &mut jsg::Lock<'_>,
    ctx: &crate::EvalContext<'_>,
    code: &str,
    name: ExceptionType,
) -> String {
    let err = ctx
        .eval::<String>(lock, code)
        .unwrap_err()
        .unwrap_jsg_err(lock);
    assert_eq!(err.name, name, "{code}: {}", err.message);
    err.message
}

// =============================================================================
// resource-test.c++ "Iterables can be iterated"
// =============================================================================

#[test]
fn iterables_can_be_iterated() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "let results = [];\
             for (let n of new Iterable()) { results.push(n); };\
             '' + results.join('')",
            "0123456789",
        );
        expect_eval(
            lock,
            ctx,
            "let results2 = [];\
             for (let n of new Iterable().entries()) { results2.push(n); };\
             '' + results2.join('')",
            "0123456789",
        );
        expect_eval(
            lock,
            ctx,
            "let arrayIterator = [][Symbol.iterator]();\
             let arrayIteratorPrototype = \
               Object.getPrototypeOf(Object.getPrototypeOf(arrayIterator));\
             let iterator = new Iterable().entries();\
             let iteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(iterator));\
             String(iteratorPrototype === arrayIteratorPrototype)",
            "true",
        );
    });
}

// =============================================================================
// `[Symbol.iterator]()`
// =============================================================================

// JSG_ITERABLE installs a method of the prototype that, unlike the type's other methods, is not
// enumerable, like the iterator methods of built-in collections.
#[test]
fn iterator_method_is_a_hidden_method_of_the_prototype() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const d = Object.getOwnPropertyDescriptor(Iterable.prototype, Symbol.iterator);\
             [typeof d.value, d.enumerable, d.writable, d.configurable].join()",
            "function,false,true,true",
        );
        expect_eval(
            lock,
            ctx,
            "String(Object.getOwnPropertyDescriptor(Iterable.prototype, 'entries').enumerable)",
            "true",
        );
        expect_eval(
            lock,
            ctx,
            "const it = new Iterable();\
             [Object.getOwnPropertySymbols(it).length, Symbol.iterator in it].join()",
            "0,true",
        );
    });
}

// The method has the signature of the type's other methods.
#[test]
fn iterator_method_needs_an_instance() {
    with_globals(|lock, ctx| {
        let message = expect_error(
            lock,
            ctx,
            "Iterable.prototype[Symbol.iterator].call({})",
            ExceptionType::TypeError,
        );
        assert!(message.contains("Illegal invocation"), "{message}");
        expect_error(
            lock,
            ctx,
            "new (new Iterable()[Symbol.iterator])()",
            ExceptionType::TypeError,
        );
    });
}

#[test]
fn iterator_method_may_also_be_a_method() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String([...new Iterable().entries()].length)",
            "10",
        );
        // They are separate functions, from separate templates.
        expect_eval(
            lock,
            ctx,
            "String(Iterable.prototype.entries === Iterable.prototype[Symbol.iterator])",
            "false",
        );
    });
}

#[test]
fn iterator_of_itself_returns_this() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(attributes[Symbol.iterator]() === attributes)",
            "true",
        );
        expect_eval(
            lock,
            ctx,
            "String(plainIterator[Symbol.iterator]() === plainIterator)",
            "true",
        );
    });
}

#[test]
fn resource_without_jsg_iterable_is_not_iterable() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "String(Symbol.iterator in plain)", "false");
        let message = expect_error(lock, ctx, "[...plain]", ExceptionType::TypeError);
        assert!(message.contains("is not iterable"), "{message}");
    });
}

// =============================================================================
// `next()` results
// =============================================================================

#[test]
fn results_leave_out_value_when_done() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "JSON.stringify([attributes.next(), attributes.next(), attributes.next(), \
             attributes.next(), attributes.next()])",
            r#"[{"done":false,"value":["class","a b"]},{"done":false,"value":["id","x"]},{"done":false,"value":["href","/"]},{"done":true},{"done":true}]"#,
        );
        expect_eval(lock, ctx, "String('value' in attributes.next())", "false");
    });
}

#[test]
fn pairs_destructure_and_spread() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const out = []; for (const [name, value] of attributes) out.push(`${name}=${value}`);\
             out.join('&')",
            "class=a b&id=x&href=/",
        );
        // The iterator is consumed.
        expect_eval(lock, ctx, "JSON.stringify([...attributes])", "[]");
    });
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(Object.fromEntries(attributes))",
            r#"{"class":"a b","id":"x","href":"/"}"#,
        );
    });
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(Array.from(attributes))",
            r#"[["class","a b"],["id","x"],["href","/"]]"#,
        );
    });
}

// An error that `next()` returns reaches the code iterating, and ends the iteration.
#[test]
fn error_from_next_is_thrown_to_the_loop() {
    with_globals(|lock, ctx| {
        let message = expect_error(
            lock,
            ctx,
            "for (const [name] of attributes) { if (name === 'id') attributes.invalidate(); }",
            ExceptionType::TypeError,
        );
        assert_eq!(message, "The attributes were changed during iteration.");
        expect_error(lock, ctx, "attributes.next()", ExceptionType::TypeError);
    });
}

// =============================================================================
// inherit_intrinsic = IteratorPrototype
// =============================================================================

#[test]
fn iterator_prototype_is_inherited() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(Object.getPrototypeOf(Object.getPrototypeOf(attributes)) === \
             Iterator.prototype)",
            "true",
        );
        expect_eval(lock, ctx, "String(attributes instanceof Iterator)", "true");
        // The type's prototype has its own members, and inherits the rest.
        expect_eval(
            lock,
            ctx,
            "const proto = Object.getPrototypeOf(attributes);\
             [Object.hasOwn(proto, 'next'), Object.hasOwn(proto, 'map'), \
              proto.map === Iterator.prototype.map].join()",
            "true,false,true",
        );
    });
}

// The type's prototype has a `Symbol.toStringTag` of its own only with `set_to_string_tag`;
// without it, the one of `Iterator.prototype` shows, as in C++ JSG.
#[test]
fn to_string_tag_is_inherited_unless_the_type_has_its_own() {
    for (enabled, expected) in [(false, "[object Iterator]"), (true, "[object Attributes]")] {
        let harness = crate::Harness::with_flags(|mut flags| flags.set_set_to_string_tag(enabled));
        harness.run_in_context(|lock, ctx| {
            let attributes = attributes(lock);
            ctx.set_global(lock, "attributes", attributes);
            expect_eval(
                lock,
                ctx,
                "Object.prototype.toString.call(attributes)",
                expected,
            );
            Ok(())
        });
    }
}

#[test]
fn iterator_helpers_work() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "attributes.map(([name, value]) => `${name}=${value}`).toArray().join('&')",
            "class=a b&id=x&href=/",
        );
    });
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "attributes.filter(([name]) => name !== 'id').map(([name]) => name).toArray().join()",
            "class,href",
        );
    });
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(attributes.some(([, value]) => value === 'x'))",
            "true",
        );
        // `some()` stopped at the first match, leaving the rest.
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(attributes.toArray())",
            r#"[["href","/"]]"#,
        );
    });
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(new Iterable().entries().reduce((a, b) => a + b))",
            "45",
        );
        expect_eval(
            lock,
            ctx,
            "new Iterable().entries().drop(2).take(3).toArray().join()",
            "2,3,4",
        );
    });
}

#[test]
fn errors_reach_iterator_helpers() {
    with_globals(|lock, ctx| {
        let message = expect_error(
            lock,
            ctx,
            "attributes.map(([name]) => { if (name === 'id') attributes.invalidate(); return name; \
             }).toArray()",
            ExceptionType::TypeError,
        );
        assert_eq!(message, "The attributes were changed during iteration.");
    });
}

// `Iterator.prototype[Symbol.iterator]()` returns `this`, so an iterator that inherits it is
// iterable without a `#[jsg_iterable]`, as C++ JSG iterators would be.
#[test]
fn inherited_iterator_method_returns_this() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(Object.hasOwn(Object.getPrototypeOf(countdown), Symbol.iterator))",
            "false",
        );
        expect_eval(
            lock,
            ctx,
            "String(countdown[Symbol.iterator]() === countdown)",
            "true",
        );
        expect_eval(lock, ctx, "[...countdown].join()", "3,2,1");
    });
}

#[test]
fn iterator_without_inherit_intrinsic_has_no_helpers() {
    with_globals(|lock, ctx| {
        expect_eval(lock, ctx, "typeof plainIterator.map", "undefined");
        expect_eval(
            lock,
            ctx,
            "String(plainIterator instanceof Iterator)",
            "false",
        );
        expect_eval(
            lock,
            ctx,
            "String(Object.getPrototypeOf(Object.getPrototypeOf(plainIterator)) === \
             Object.prototype)",
            "true",
        );
        expect_eval(lock, ctx, "[...plainIterator].join()", "3,2,1");
    });
    // `Iterator.from()` wraps it in an object that has the helpers.
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const wrapped = Iterator.from(plainIterator);\
             [wrapped === plainIterator, wrapped.toArray()].join()",
            "false,3,2,1",
        );
    });
    // It is returned as it is when it inherits from `Iterator.prototype`.
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "String(Iterator.from(attributes) === attributes)",
            "true",
        );
    });
}

// Inheriting from the intrinsic does not make the constructor's prototype chain or `instanceof`
// of the type itself any different.
#[test]
fn inheriting_keeps_the_type_identity() {
    with_globals(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const it = new Iterable().entries();\
             [it.constructor.name, it instanceof it.constructor, \
              Object.getPrototypeOf(it) === it.constructor.prototype].join()",
            "NumberIterator,true,true",
        );
        expect_error(
            lock,
            ctx,
            "new (new Iterable().entries().constructor)()",
            ExceptionType::TypeError,
        );
    });
}

// =============================================================================
// Custom registration
// =============================================================================

#[test]
fn custom_members_choose_whether_to_inherit() {
    for (enabled, expected) in [(false, "undefined,false"), (true, "function,true")] {
        let harness =
            crate::Harness::with_flags(|mut flags| flags.set_cache_option_enabled(enabled));
        harness.run_in_context(|lock, ctx| {
            let v = jsg::Rc::new(
                lock,
                Versioned {
                    remaining: Cell::new(2),
                },
            );
            ctx.set_global(lock, "v", v);
            expect_eval(
                lock,
                ctx,
                "[typeof v.map, v instanceof Iterator].join()",
                expected,
            );
            expect_eval(lock, ctx, "[...v].join()", "2,1");
            Ok(())
        });
    }
}

/// Registers two of a member a type has at most one of.
#[jsg_resource]
struct Conflicting;

#[jsg_resource(custom_members)]
impl Conflicting {
    #[jsg_iterable]
    fn first(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }

    #[jsg_iterable]
    fn second(this: jsg::Rc<Self>) -> jsg::Rc<Self> {
        this
    }
}

impl Resource for Conflicting {
    fn members(registry: &mut impl MemberRegistry, flags: compatibility_flags::Reader<'_>) {
        if flags.get_cache_option_enabled() {
            registry.add(Self::first_iterable_member());
            registry.add(jsg::undescribed(Member::Iterable {
                callback: Self::second_iterable_callback(),
            }));
        } else {
            registry.inherit_intrinsic(Intrinsic::IteratorPrototype);
            registry.inherit_intrinsic(Intrinsic::IteratorPrototype);
        }
    }
}

#[test]
fn conflicting_members_panic() {
    for (cache, message) in [
        (
            false,
            "Conflicting inherits from more than one type or intrinsic",
        ),
        (true, "Conflicting has more than one iterator method"),
    ] {
        let harness = crate::Harness::with_flags(|mut flags| flags.set_cache_option_enabled(cache));
        harness.run_in_context(|lock, ctx| {
            let payload = std::panic::catch_unwind(AssertUnwindSafe(|| {
                let _ = jsg::resource::function_template_of::<Conflicting>(lock);
            }))
            .expect_err("conflicting members must panic");
            assert_eq!(panic_message(&*payload), message);
            assert!(ctx.eval::<bool>(lock, "true").unwrap());
            Ok(())
        });
    }
}
