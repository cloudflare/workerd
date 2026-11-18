// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A `#[jsg_struct]` field or a `Vec` element that is not of its type is a `TypeError` naming the
//! field or the index, with the messages of C++ JSG's `TypeErrorContext::structField()` and
//! `arrayElement()` (jsg/util.c++). An error from converting one passes through.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct Inner {
    pub name: String,
    pub count: Option<u32>,
}

#[jsg_struct]
struct Outer {
    pub list: Vec<f64>,
    pub inner: Option<Inner>,
    pub items: Option<Vec<Inner>>,
}

#[jsg_struct]
struct Wrapper {
    pub outer: Option<Outer>,
}

/// Asserts that converting `expr` to `T` is a `TypeError` with `message`.
fn assert_type_error<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
    message: &str,
) {
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    let Err(err) = T::from_js(lock, value) else {
        panic!("{expr} should not convert");
    };
    assert_eq!(err.name, ExceptionType::TypeError, "{expr}");
    assert_eq!(err.message, message, "{expr}");
}

#[test]
fn struct_field_of_the_wrong_type_names_the_field() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_type_error::<Outer>(
            lock,
            ctx,
            "({ list: 1 })",
            "Incorrect type for the 'list' field on 'Outer': the provided value is not of type \
             'Array'.",
        );
        assert_type_error::<Outer>(
            lock,
            ctx,
            "({ list: [], inner: 1 })",
            "Incorrect type for the 'inner' field on 'Outer': the provided value is not of type \
             'Inner'.",
        );
        // A nested struct reports its own field, and an error from converting a field passes
        // through.
        assert_type_error::<Wrapper>(
            lock,
            ctx,
            "({ outer: { list: 1 } })",
            "Incorrect type for the 'list' field on 'Outer': the provided value is not of type \
             'Array'.",
        );
        assert_type_error::<Outer>(
            lock,
            ctx,
            "({ list: [], inner: { name: 'a', count: -1 } })",
            "The value cannot be converted because it is negative and this API expects a \
             positive number.",
        );
        Ok(())
    });
}

#[test]
fn vec_element_of_the_wrong_type_names_the_index() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_type_error::<Vec<Inner>>(
            lock,
            ctx,
            "[{ name: 'a' }, 1]",
            "Incorrect type for array element 1: the provided value is not of type 'Inner'.",
        );
        // A hole reads as undefined.
        assert_type_error::<Vec<Vec<f64>>>(
            lock,
            ctx,
            "[[1], , [2]]",
            "Incorrect type for array element 1: the provided value is not of type 'Array'.",
        );
        assert_type_error::<Vec<Vec<f64>>>(
            lock,
            ctx,
            "[[1], [2], 3]",
            "Incorrect type for array element 2: the provided value is not of type 'Array'.",
        );
        // The innermost failure is the one reported.
        assert_type_error::<Vec<Vec<Inner>>>(
            lock,
            ctx,
            "[[], [{ name: 'a' }, 'b']]",
            "Incorrect type for array element 1: the provided value is not of type 'Inner'.",
        );
        assert_type_error::<Outer>(
            lock,
            ctx,
            "({ list: [], items: [{ name: 'a' }, null] })",
            "Cannot initialize Inner with required members from an undefined or null value.",
        );
        assert_type_error::<Outer>(
            lock,
            ctx,
            "({ list: [], items: [{ name: 'a' }, 1] })",
            "Incorrect type for array element 1: the provided value is not of type 'Inner'.",
        );
        Ok(())
    });
}
