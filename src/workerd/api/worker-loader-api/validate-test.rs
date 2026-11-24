// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[expect(
    clippy::unnecessary_wraps,
    reason = "`python_flags` takes and returns an `Option`"
)]
fn flags(names: &[&str]) -> Option<Vec<String>> {
    Some(names.iter().map(|&n| n.to_owned()).collect())
}

#[test]
fn string_modules_classify_by_name() {
    assert_eq!(classify_string_module("a.js", "a.js"), Ok(ModuleKind::Es));
    assert_eq!(
        classify_string_module("a.py", "a.js"),
        Ok(ModuleKind::Python)
    );
    assert_eq!(
        classify_string_module("python_modules/pkg/METADATA", "main.py"),
        Ok(ModuleKind::Text)
    );
}

#[test]
fn python_modules_dir_is_text_only_for_python_workers() {
    let err = classify_string_module("python_modules/METADATA", "main.js").unwrap_err();
    assert_eq!(err.kind, ErrorKind::TypeError);
    assert_eq!(
        err.message,
        "Module name must end with '.js' or '.py' (or the content must be an object indicating \
         the type explicitly). Got: python_modules/METADATA"
    );
}

#[test]
fn typescript_names_suggest_bundling() {
    for name in ["a.ts", "a.tsx", "a.jsx"] {
        let err = classify_string_module(name, "main.js").unwrap_err();
        assert_eq!(
            err.message,
            format!(
                "Module name must end with '.js' or '.py' (or the content must be an object \
                 indicating the type explicitly). Got: {name}. If you're trying to load \
                 TypeScript, bundle it first with '@cloudflare/worker-bundler' and pass the \
                 generated JavaScript modules."
            )
        );
    }
}

#[test]
fn no_modules_is_a_type_error() {
    let err = require_modules(0).unwrap_err();
    assert_eq!(err.kind, ErrorKind::TypeError);
    assert_eq!(
        err.message,
        "Dynamic Worker code must contain at least one module."
    );
    assert!(require_modules(1).is_ok());
}

#[test]
fn module_objects_need_exactly_one_field() {
    assert!(require_exactly_one_field("m", 1).is_ok());
    for count in [0, 2] {
        let err = require_exactly_one_field("m", count).unwrap_err();
        assert_eq!(err.kind, ErrorKind::TypeError);
        assert_eq!(
            err.message,
            format!(
                "Each module must contain exactly one of 'js', 'cjs', 'text', 'data', 'json', \
                 'py', or 'wasm'. Module 'm' contained {count} properties."
            )
        );
    }
}

#[test]
fn javascript_workers_hold_no_python() {
    let err = check_modules(
        "main.js",
        [
            ("main.js", ModuleKind::Es, 1),
            ("x.py", ModuleKind::Python, 1),
        ],
    )
    .unwrap_err();
    assert_eq!(err.kind, ErrorKind::TypeError);
    assert_eq!(
        err.message,
        "Module \"x.py\" is a Python module, but the main module isn't a Python module."
    );
    assert!(check_modules("main.py", [("x.py", ModuleKind::Python, 1)]).is_ok());
}

#[test]
fn code_size_limit() {
    assert!(check_modules("a.js", [("a.js", ModuleKind::Es, MAX_CODE_SIZE)]).is_ok());
    let err = check_modules(
        "a.js",
        [
            ("a.js", ModuleKind::Es, MAX_CODE_SIZE),
            ("b.wasm", ModuleKind::Wasm, 1),
        ],
    )
    .unwrap_err();
    assert_eq!(err.kind, ErrorKind::Error);
    assert_eq!(
        err.message,
        "Dynamic Worker code size (67108865 bytes) exceeds the maximum allowed size of 67108864 \
         bytes."
    );
}

#[test]
fn env_size_limit() {
    assert!(check_env_size(MAX_ENV_SIZE).is_ok());
    let err = check_env_size(MAX_ENV_SIZE + 1).unwrap_err();
    assert_eq!(
        err.message,
        "Dynamic Worker env size (1048577 bytes) exceeds the maximum allowed size of 1048576 \
         bytes."
    );
}

#[test]
fn python_flags_are_added() {
    assert_eq!(python_flags("a.js", None), None);
    assert_eq!(python_flags("a.js", flags(&["x"])), flags(&["x"]));
    assert_eq!(
        python_flags("a.py", None),
        flags(&["python_workers", "disable_python_external_sdk"])
    );
    assert_eq!(
        python_flags("a.py", flags(&["x", "python_workers"])),
        flags(&["x", "python_workers", "disable_python_external_sdk"])
    );
    assert_eq!(
        python_flags("a.py", flags(&["enable_python_external_sdk"])),
        flags(&["enable_python_external_sdk", "python_workers"])
    );
    assert_eq!(
        python_flags(
            "a.py",
            flags(&["python_workers", "disable_python_external_sdk"])
        ),
        flags(&["python_workers", "disable_python_external_sdk"])
    );
}

#[test]
fn allow_experimental_needs_experimental_caller() {
    assert!(check_allow_experimental(false, false).is_ok());
    assert!(check_allow_experimental(true, true).is_ok());
    let err = check_allow_experimental(true, false).unwrap_err();
    assert_eq!(err.kind, ErrorKind::Error);
    assert_eq!(
        err.message,
        "'allowExperimental' is only allowed when the calling worker has the 'experimental' \
         compat flag set."
    );
}

#[test]
fn streaming_tails_need_allow_experimental() {
    assert!(check_streaming_tails(true).is_ok());
    assert_eq!(
        check_streaming_tails(false).unwrap_err().message,
        "Streaming tail workers are experimental. You must pass the option 'allowExperimental: \
         true' to the worker loader to use them"
    );
}

#[test]
fn default_entrypoint_names() {
    assert_eq!(entrypoint_name(None), None);
    assert_eq!(entrypoint_name(Some("default".to_owned())), None);
    assert_eq!(
        entrypoint_name(Some("Foo".to_owned())),
        Some("Foo".to_owned())
    );
}
