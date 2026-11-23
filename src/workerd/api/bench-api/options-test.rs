// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn ns(text: &str) -> jsg::Result<u64> {
    parse_duration(&BenchDuration::String(text.to_owned()), "minTime")
}

/// The type and message of the error `result` fails with.
fn message(result: jsg::Result<u64>) -> (jsg::ExceptionType, String) {
    let Err(err) = result else {
        panic!("expected an error");
    };
    let ty = [
        jsg::ExceptionType::TypeError,
        jsg::ExceptionType::RangeError,
        jsg::ExceptionType::Error,
    ]
    .into_iter()
    .find(|ty| err.name == *ty)
    .unwrap();
    (ty, err.message)
}

#[test]
fn numbers_are_milliseconds() {
    let parse = |ms| parse_duration(&BenchDuration::Number(ms), "warmup").unwrap();
    assert_eq!(parse(0.0), 0);
    assert_eq!(parse(1.5), 1_500_000);
    assert_eq!(parse(250.0), 250_000_000);
}

#[test]
fn strings_take_a_unit() {
    assert_eq!(ns("7ns").unwrap(), 7);
    assert_eq!(ns("2us").unwrap(), 2_000);
    assert_eq!(ns("2µs").unwrap(), 2_000);
    assert_eq!(ns("3ms").unwrap(), 3_000_000);
    assert_eq!(ns("3").unwrap(), 3_000_000);
    assert_eq!(ns("1.5s").unwrap(), 1_500_000_000);
    assert_eq!(ns(".5s").unwrap(), 500_000_000);
    assert_eq!(ns("5.s").unwrap(), 5_000_000_000);
}

#[test]
fn strings_without_a_number_are_type_errors() {
    let expected = (
        jsg::ExceptionType::TypeError,
        "minTime must be a number of milliseconds or a string like \"500ms\" or \"2s\".".to_owned(),
    );
    for text in ["", "ms", ".", "1..2ms", "-1ms", " 1ms"] {
        assert_eq!(message(ns(text)), expected, "{text:?}");
    }
}

#[test]
fn unknown_units_are_type_errors() {
    assert_eq!(
        message(ns("5min")),
        (
            jsg::ExceptionType::TypeError,
            "minTime has an unknown unit \"min\"; use \"ns\", \"us\", \"ms\", or \"s\".".to_owned()
        )
    );
    // The number ends at the first byte that isn't a digit or a point.
    assert_eq!(
        message(ns("1e3ms")).1,
        "minTime has an unknown unit \"e3ms\"; use \"ns\", \"us\", \"ms\", or \"s\"."
    );
    assert_eq!(
        message(ns("5 ms")).1,
        "minTime has an unknown unit \" ms\"; use \"ns\", \"us\", \"ms\", or \"s\"."
    );
}

#[test]
fn durations_must_be_non_negative_and_finite() {
    let expected = (
        jsg::ExceptionType::RangeError,
        "warmup must be a non-negative duration.".to_owned(),
    );
    for ms in [-1.0, f64::NAN, f64::INFINITY, 1e12] {
        assert_eq!(
            message(parse_duration(&BenchDuration::Number(ms), "warmup")),
            expected,
            "{ms}"
        );
    }
    let huge = format!("{}s", "9".repeat(400));
    assert_eq!(message(ns(&huge)).0, jsg::ExceptionType::RangeError);
}

#[test]
fn counts_are_integers_in_range() {
    assert_eq!(parse_count(1.0, 10, "samples").unwrap(), 1);
    assert_eq!(parse_count(10.0, 10, "samples").unwrap(), 10);
    for value in [0.0, 11.0, 1.5, -3.0, f64::NAN, f64::INFINITY] {
        assert_eq!(
            message(parse_count(value, 10, "samples")),
            (
                jsg::ExceptionType::RangeError,
                "samples must be an integer from 1 to 10.".to_owned()
            ),
            "{value}"
        );
    }
}
