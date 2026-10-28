// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::Dict<V>` converts like C++ JSG's `jsg::Dict<V>` (`DictWrapper` in `jsg/value.h`), which
//! is Web IDL's `record<DOMString, V>`.

use jsg::Dict;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Records {
    headers: Dict<String>,
}

#[jsg_resource]
impl Records {
    /// Describes the entries, in order, as `key=value` pairs.
    #[jsg_method]
    pub fn describe(&self, dict: Dict<f64>) -> String {
        dict.into_iter()
            .map(|(key, value)| format!("{key}={value}"))
            .collect::<Vec<_>>()
            .join(",")
    }

    #[jsg_method]
    pub fn describe_strings(&self, dict: Dict<String>) -> String {
        dict.into_iter()
            .map(|(key, value)| format!("{key}={value}"))
            .collect::<Vec<_>>()
            .join(",")
    }

    #[jsg_method]
    pub fn count_present(&self, dict: Dict<Option<f64>>) -> f64 {
        f64::from(u32::try_from(dict.into_iter().filter(|(_, v)| v.is_some()).count()).unwrap())
    }

    #[jsg_method]
    pub fn total_lengths(&self, dict: Dict<Vec<f64>>) -> f64 {
        f64::from(u32::try_from(dict.into_iter().map(|(_, v)| v.len()).sum::<usize>()).unwrap())
    }

    #[jsg_method]
    pub fn sum_nested(&self, dict: Dict<Dict<f64>>) -> f64 {
        dict.into_iter()
            .flat_map(|(_, inner)| inner.into_iter().map(|(_, v)| v))
            .sum()
    }

    #[jsg_method]
    pub fn count_functions<'s>(&self, dict: Dict<v8::Local<'s, v8::Value>>) -> f64 {
        f64::from(u32::try_from(dict.into_iter().filter(|(_, v)| v.is_function()).count()).unwrap())
    }

    #[jsg_method]
    pub fn lookup(&self, dict: Dict<String>, key: String) -> Option<String> {
        dict.find(&key).cloned()
    }

    #[jsg_method]
    pub fn make(&self, keys: Vec<String>) -> Dict<f64> {
        keys.into_iter()
            .zip(1..)
            .map(|(key, n)| (key, f64::from(n)))
            .collect()
    }

    /// Is terminated and then returns a `Dict`, which is converted after the termination.
    #[jsg_method]
    pub fn terminate_then_make(&self, lock: &mut jsg::Lock<'_>) -> Dict<f64> {
        crate::Harness::terminate_and_deliver(lock);
        self.make(vec!["a".to_owned()])
    }

    #[jsg_property(instance, readonly)]
    pub fn get_headers(&self) -> Dict<String> {
        self.headers.clone()
    }
}

fn with_records(f: impl for<'s> FnOnce(&mut jsg::Lock<'s>, &crate::EvalContext<'_>)) {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let records = jsg::Rc::new(Records {
            headers: Dict::from(vec![
                ("content-type".to_owned(), "text/plain".to_owned()),
                ("x-id".to_owned(), "42".to_owned()),
            ]),
        });
        ctx.set_global(lock, "records", records);
        f(lock, ctx);
        Ok(())
    });
}

fn expect_eval(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str, expected: &str) {
    let result: String = ctx.eval(lock, code).unwrap();
    assert_eq!(result, expected, "{code}");
}

/// Evaluates `code`, which calls a method, and returns what it threw as `name: message`.
fn thrown(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, code: &str) -> String {
    let result: String = ctx
        .eval(
            lock,
            &format!("try {{ {code}; 'nothing' }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}"),
        )
        .unwrap();
    result
}

// Like Object.keys(): integer-like keys first in ascending order, then the other strings in
// the order they were added.
#[test]
fn entries_are_in_property_order() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "records.describe({ b: 1, a: 2, 2: 3, 1: 4 })",
            "1=4,2=3,b=1,a=2",
        );
        expect_eval(lock, ctx, "records.describe({})", "");
    });
}

#[test]
fn only_own_enumerable_string_keys_are_entries() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const o = Object.create({ inherited: 1 }, {
               hidden: { value: 2, enumerable: false },
               shown: { value: 3, enumerable: true },
             });
             o[Symbol('s')] = 4;
             records.describe(o)",
            "shown=3",
        );
    });
}

#[test]
fn any_object_other_than_an_array_is_a_dict() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const f = () => {}; f.x = 1;
             class C { constructor() { this.y = 2; } }
             [records.describe(f), records.describe(new C()),
              records.describe(new Proxy({ z: 3 }, {})), records.describe(new Date(0))].join(' ')",
            "x=1 y=2 z=3 ",
        );
    });
}

// The error matches C++ JSG's for an argument of the wrong type.
#[test]
fn values_other_than_objects_are_not_dicts() {
    with_records(|lock, ctx| {
        for value in [
            "[1]",
            "null",
            "undefined",
            "1",
            "'a'",
            "Symbol()",
            "1n",
            "true",
        ] {
            assert_eq!(
                thrown(lock, ctx, &format!("records.describe({value})")),
                "TypeError: Failed to execute 'describe' on 'Records': parameter 1 is not of type \
                 'object'.",
                "{value}"
            );
        }
        let value = ctx.eval_raw(lock, "[1]").unwrap();
        let err = Dict::<f64>::from_js(lock, value).unwrap_err();
        assert_eq!(err.message, "Expected object but got array");
        let value = ctx.eval_raw(lock, "1").unwrap();
        let err = Dict::<f64>::from_js(lock, value).unwrap_err();
        assert_eq!(err.message, "Expected object but got number");
    });
}

// Values convert as the value type does, coercing like an argument would.
#[test]
fn values_convert_as_their_type_does() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "records.describeStrings({ a: 1, b: true, c: null, d: { toString() { return 'x'; } } })",
            "a=1,b=true,c=null,d=x",
        );
        expect_eval(
            lock,
            ctx,
            "records.describe({ a: '2', b: null })",
            "a=2,b=0",
        );
        expect_eval(
            lock,
            ctx,
            "String(records.countPresent({ a: 1, b: undefined, c: null }))",
            "2",
        );
        expect_eval(
            lock,
            ctx,
            "String(records.sumNested({ a: { x: 1, y: 2 }, b: { z: 3 } }))",
            "6",
        );
        expect_eval(
            lock,
            ctx,
            "String(records.countFunctions({ a: () => {}, b: 1, c: class {} }))",
            "2",
        );
    });
}

// As in C++ JSG, a value that is not of the value type names its entry, and an error from
// converting a value passes through.
#[test]
fn value_errors_name_their_entry() {
    with_records(|lock, ctx| {
        assert_eq!(
            thrown(lock, ctx, "records.totalLengths({ a: [1], b: 2 })"),
            "TypeError: Incorrect type for map entry 'b': the provided value is not of type \
             'Array'."
        );
        assert_eq!(
            thrown(lock, ctx, "records.sumNested({ a: { x: 1 }, b: [] })"),
            "TypeError: Incorrect type for map entry 'b': the provided value is not of type \
             'object'."
        );
        assert_eq!(
            thrown(lock, ctx, "records.describe({ a: Symbol() })"),
            "TypeError: Cannot convert a Symbol value to a number"
        );
    });
}

// Each value is read and then converted before the next one is read, as in C++ JSG.
#[test]
fn values_are_read_and_converted_in_turn() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const log = [];
             const value = (name) => ({ valueOf() { log.push('convert ' + name); return 1; } });
             records.describe({
               get a() { log.push('get a'); return value('a'); },
               get b() { log.push('get b'); return value('b'); },
             });
             log.join(', ')",
            "get a, convert a, get b, convert b",
        );
    });
}

// What a Proxy trap, a getter or a conversion throws reaches the caller unchanged, and nothing
// after it runs.
#[test]
fn exceptions_pass_through_unchanged() {
    with_records(|lock, ctx| {
        for (name, value) in [
            ("ownKeys", "new Proxy({}, { ownKeys() { throw thrown; } })"),
            (
                "getOwnPropertyDescriptor",
                "new Proxy({ a: 1 }, { getOwnPropertyDescriptor() { throw thrown; } })",
            ),
            (
                "getter",
                "({ get a() { throw thrown; }, get b() { ran = true; } })",
            ),
            (
                "valueOf",
                "({ a: { valueOf() { throw thrown; } }, get b() { ran = true; } })",
            ),
        ] {
            let result: bool = ctx
                .eval(
                    lock,
                    &format!(
                        "globalThis.thrown = new RangeError('{name}'); globalThis.ran = false;
                         {{
                           let caught;
                           try {{ records.describe({value}); }} catch (e) {{ caught = e; }}
                           caught === thrown && !ran
                         }}"
                    ),
                )
                .unwrap();
            assert!(result, "{name}");
        }
    });
}

// A value is read with the name V8 listed, so even a key that does not survive conversion to a
// Rust string gives its value: a lone surrogate becomes U+FFFD, as in any string.
#[test]
fn values_are_read_by_the_listed_name() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "records.describe({ '\\ud800': 1 })",
            "\u{fffd}=1",
        );
    });
}

#[test]
fn dicts_convert_to_plain_objects() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const d = records.make(['b', 'a', '1']);
             [Object.getPrototypeOf(d) === Object.prototype, JSON.stringify(d)].join(' ')",
            "true {\"1\":3,\"b\":1,\"a\":2}",
        );
        expect_eval(lock, ctx, "JSON.stringify(records.make([]))", "{}");
        // A later entry with the same key overwrites an earlier one.
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(records.make(['a', 'a']))",
            "{\"a\":2}",
        );
        expect_eval(
            lock,
            ctx,
            "JSON.stringify(records.headers)",
            "{\"content-type\":\"text/plain\",\"x-id\":\"42\"}",
        );
        expect_eval(
            lock,
            ctx,
            "String(records.headers === records.headers)",
            "false",
        );
    });
}

// As Web IDL does for a record, converting a Dict defines its entries as own data properties,
// so a setter that Object.prototype has for a key neither runs nor hides the entry.
#[test]
fn converting_a_dict_runs_no_inherited_setters() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const seen = [];
             Object.defineProperty(Object.prototype, 'a', {
               set(v) { seen.push(v); }, configurable: true,
             });
             Object.defineProperty(Object.prototype, 'b', {
               set(v) { throw new RangeError('setter'); }, configurable: true,
             });
             const d = records.make(['a', 'b']);
             delete Object.prototype.a;
             delete Object.prototype.b;
             const own = Object.getOwnPropertyDescriptor(d, 'a');
             [seen.length, JSON.stringify(d), own.writable, own.enumerable, own.configurable]
               .join(' ')",
            "0 {\"a\":1,\"b\":2} true true true",
        );
    });
}

// A `__proto__` entry is an own property, as `Object.fromEntries` makes it: it neither
// replaces the object's prototype nor is dropped.
#[test]
fn a_proto_entry_is_an_own_property() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "const d = records.make(['__proto__', 'a']);
             [Object.getPrototypeOf(d) === Object.prototype, Object.hasOwn(d, '__proto__'),
              JSON.stringify(d)].join(' ')",
            "true true {\"__proto__\":1,\"a\":2}",
        );
    });
}

// Converting a Dict while execution is terminating keeps the termination: JavaScript neither
// catches it nor runs on.
#[test]
fn converting_a_dict_while_terminating_keeps_the_termination() {
    with_records(|lock, ctx| {
        let result = ctx.eval_raw(
            lock,
            "globalThis.after = false;
             try { records.terminateThenMake(); } catch (e) {}
             globalThis.after = true;",
        );
        assert!(result.is_err(), "not terminated: {result:?}");
        assert!(!ctx.eval::<bool>(lock, "globalThis.after").unwrap());
    });
}

#[test]
fn dicts_round_trip() {
    with_records(|lock, ctx| {
        expect_eval(
            lock,
            ctx,
            "records.describe(records.make(['x', 'y']))",
            "x=1,y=2",
        );
        expect_eval(
            lock,
            ctx,
            "records.lookup(records.headers, 'x-id') + ' ' + records.lookup({}, 'x-id')",
            "42 undefined",
        );
    });
}

#[test]
#[expect(
    clippy::float_cmp,
    reason = "the values are small integers, exactly representable"
)]
fn get_by_name_and_keys() {
    with_records(|lock, ctx| {
        let value = ctx
            .eval_raw(lock, "({ b: 1, [Symbol.for('s')]: 2, a: 3 })")
            .unwrap();
        let object = v8::Local::<v8::Object>::try_from(value).unwrap();
        let keys = object.keys(lock).unwrap();
        let names: Vec<String> = keys
            .iter()
            .map(|key| String::from_js(lock, (*key).into()).unwrap())
            .collect();
        assert_eq!(names, ["b", "a"]);
        let value = object.get_by_name(lock, keys[1].into()).unwrap();
        assert_eq!(f64::from_js(lock, value).unwrap(), 3.0);

        let symbol = ctx.eval_raw(lock, "Symbol.for('s')").unwrap();
        let symbol = v8::Local::<v8::Symbol>::try_from(symbol).unwrap();
        let value = object.get_by_name(lock, symbol.into()).unwrap();
        assert_eq!(f64::from_js(lock, value).unwrap(), 2.0);
    });
}
