// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn described(src: &str) -> Vec<String> {
    parse_annotations(src)
        .unwrap()
        .iter()
        .map(describe_expected)
        .collect()
}

#[test]
fn annotation_forms() {
    let src = "\
f(); //~ ERROR E0308
g();
//~^ ERROR mismatched types
//~| ERROR E0425: `Undefined`
h();

//~^^ ERROR E0597
//~? ERROR E0277
";
    assert_eq!(
        described(src),
        [
            "error[E0308] on line 1 (annotation on line 1)",
            "error containing \"mismatched types\" on line 2 (annotation on line 3)",
            "error[E0425] containing \"`Undefined`\" on line 2 (annotation on line 4)",
            "error[E0597] on line 5 (annotation on line 7)",
            "error[E0277] anywhere (annotation on line 8)",
        ]
    );
}

#[test]
fn malformed_annotations_are_rejected() {
    let error = |src: &str| parse_annotations(src).err().unwrap();
    assert!(error("//~| ERROR E0308").contains("must follow another"));
    assert!(error("\n//~^^ ERROR E0308").contains("points before the first line"));
    assert!(error("f(); //~ E0308").contains("expected `ERROR`"));
    assert!(error("f(); //~ ERROR").contains("needs an error code"));
}

#[test]
fn errors_are_parsed_and_summaries_skipped() {
    let stderr = "\
error[E0308]: mismatched types
 --> snippet.rs:5:5
  |
warning: unused variable
 --> snippet.rs:2:9
error: lifetime may not live long enough
 --> snippet.rs:9:5
error: aborting due to 2 previous errors
 --> snippet.rs:3:1
error: no location
error: aborting due to 3 previous errors; 1 warning emitted
";
    let errors: Vec<String> = parse_errors(stderr).iter().map(describe_actual).collect();
    assert_eq!(
        errors,
        [
            "error[E0308]: mismatched types at snippet.rs:5",
            "error: lifetime may not live long enough at snippet.rs:9",
            // A compile_error! that reads like the summary has a location; the summary has none.
            "error: aborting due to 2 previous errors at snippet.rs:3",
            "error: no location",
        ]
    );
}

#[test]
fn abort_summary_forms() {
    assert!(is_abort_summary("aborting due to 1 previous error"));
    assert!(is_abort_summary("aborting due to 2 previous errors"));
    assert!(is_abort_summary("aborting due to previous error"));
    assert!(is_abort_summary(
        "aborting due to 2 previous errors; 3 warnings emitted"
    ));
    assert!(is_abort_summary(
        "aborting due to 1 previous error; 1 warning emitted"
    ));
    assert!(!is_abort_summary("aborting due to a bug"));
    assert!(!is_abort_summary("aborting due to two previous errors"));
    assert!(!is_abort_summary("aborting due to 2 previous errors; oops"));
    assert!(!is_abort_summary("aborting due to 2 previous errors and more"));
}

#[test]
fn every_annotation_gets_its_own_error() {
    // Both errors match the first annotation, but only the first error matches the second, so the
    // first annotation must take the second error.
    let expected = parse_annotations("f();\n//~^ ERROR E0425\n//~| ERROR E0425: `Undefined`").unwrap();
    let actual = parse_errors(
        "error[E0425]: cannot find type `Undefined`\n --> s.rs:1:1\n\
         error[E0425]: cannot find type `AlsoUndefined`\n --> s.rs:1:2\n",
    );
    assert_eq!(assign(&expected, &actual, "s.rs"), [Some(1), Some(0)]);
}

#[test]
fn verdicts() {
    let expected = parse_annotations("f(); //~ ERROR E0425").unwrap();
    let matching = parse_errors("error[E0425]: cannot find function `f`\n --> s.rs:1:1\n");
    let problems = find_problems(&expected, &matching, false, "s.rs");
    assert_eq!(problems, "");
    assert_eq!(verdict(problems.clone(), false), Ok(problems.clone()));
    assert!(verdict(problems, true).unwrap_err().contains("marked as a mismatch"));

    let problems = find_problems(&expected, &[], true, "s.rs");
    assert!(problems.contains("compiled, but its annotations expect 1 error(s)"));
    assert!(problems.contains("missing: error[E0425] on line 1"));
    assert_eq!(verdict(problems.clone(), true), Ok(problems.clone()));
    assert_eq!(verdict(problems.clone(), false), Err(problems));

    assert!(find_problems(&[], &[], false, "s.rs").contains("must compile, but rustc failed"));
    assert_eq!(find_problems(&[], &[], true, "s.rs"), "");
}
