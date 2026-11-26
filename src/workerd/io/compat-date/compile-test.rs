use super::*;

fn date_of(s: &str) -> CompatDate {
    CompatDate::parse(s).unwrap()
}

const TEST: CompileOptions<'static> = CompileOptions {
    date_validation: DateValidation::FutureForTest,
    allow_experimental: false,
    allowed_experimental_flags: &[],
    main_module_is_python: false,
};

fn with(validation: DateValidation) -> CompileOptions<'static> {
    CompileOptions {
        date_validation: validation,
        ..TEST
    }
}

fn compile_at(compat_date: &str, flags: &[&str], options: &CompileOptions<'_>) -> Compiled {
    compile_with_today(
        compat_date.as_bytes(),
        flags.iter().map(|f| f.as_bytes()),
        options,
        date_of("2026-11-18"),
        date_of("2026-11-20"),
    )
    .unwrap()
}

fn is_on(compiled: &Compiled, name: &str) -> bool {
    let table = FlagTable::get().unwrap();
    let index = table.fields.iter().position(|f| f.name == name).unwrap();
    compiled.enabled[index]
}

fn errors(compiled: &Compiled) -> Vec<String> {
    compiled
        .errors()
        .map(|e| String::from_utf8(e.to_vec()).unwrap())
        .collect()
}

fn warnings(compiled: &Compiled) -> Vec<String> {
    compiled
        .warnings()
        .map(|w| String::from_utf8(w.to_vec()).unwrap())
        .collect()
}

#[test]
fn enables_by_date() {
    assert!(!is_on(
        &compile_at("2021-11-02", &[], &TEST),
        "formDataParserSupportsFiles"
    ));
    assert!(is_on(
        &compile_at("2021-11-03", &[], &TEST),
        "formDataParserSupportsFiles"
    ));
    assert!(is_on(
        &compile_at("2021-05-17", &[], &TEST),
        "r2PublicBetaApi"
    ));
}

#[test]
fn enable_and_disable_flags_override_the_date() {
    let c = compile_at("2021-05-17", &["formdata_parser_supports_files"], &TEST);
    assert!(is_on(&c, "formDataParserSupportsFiles"));
    let c = compile_at(
        "2021-11-03",
        &["formdata_parser_converts_files_to_strings"],
        &TEST,
    );
    assert!(!is_on(&c, "formDataParserSupportsFiles"));
    let c = compile_at("2021-05-17", &["r2_internal_beta_bindings"], &TEST);
    assert!(!is_on(&c, "r2PublicBetaApi"));
    assert!(errors(&c).is_empty());
}

#[test]
fn reports_errors_in_cxx_order() {
    let c = compile_at(
        "abcd",
        &[
            "formdata_parser_supports_files",
            "fetch_refuses_unknown_protocols",
            "unknown_feature",
            "fetch_refuses_unknown_protocols",
            "another_feature",
            "formdata_parser_supports_files",
        ],
        &TEST,
    );
    assert_eq!(
        errors(&c),
        [
            "Compatibility flag specified multiple times: fetch_refuses_unknown_protocols",
            "Compatibility flag specified multiple times: formdata_parser_supports_files",
            "Invalid compatibility date: abcd",
            "No such compatibility flag: another_feature",
            "No such compatibility flag: unknown_feature",
        ]
    );
}

#[test]
fn reports_contradictory_flags() {
    let c = compile_at(
        "2021-05-17",
        &[
            "formdata_parser_supports_files",
            "formdata_parser_converts_files_to_strings",
        ],
        &TEST,
    );
    assert_eq!(
        errors(&c),
        [
            "Compatibility flags are mutually contradictory: formdata_parser_supports_files vs \
          formdata_parser_converts_files_to_strings"
        ]
    );
}

#[test]
fn warns_about_redundant_flags() {
    let c = compile_at(
        "2021-11-04",
        &["formdata_parser_supports_files"],
        &with(DateValidation::CodeVersion),
    );
    assert!(errors(&c).is_empty());
    assert_eq!(
        warnings(&c),
        [
            "The compatibility flag formdata_parser_supports_files became the default as of \
          2021-11-03 so does not need to be specified anymore."
        ]
    );

    let c = compile_at(
        "2021-05-17",
        &["r2_public_beta_bindings"],
        &with(DateValidation::CodeVersion),
    );
    assert_eq!(
        warnings(&c),
        [
            "The compatibility flag r2_public_beta_bindings is the default, so does not need to be \
          specified anymore."
        ]
    );

    let c = compile_at("2021-11-04", &["formdata_parser_supports_files"], &TEST);
    assert!(warnings(&c).is_empty());
}

#[test]
fn validates_the_date() {
    let c = compile_at(
        "2252-04-01",
        &[],
        &with(DateValidation::CurrentDateForCloudflare),
    );
    assert_eq!(
        errors(&c),
        ["Can't set compatibility date in the future: 2252-04-01"]
    );

    let c = compile_at("2252-04-01", &[], &with(DateValidation::CodeVersion));
    assert_eq!(
        errors(&c),
        [
            "This Worker requires compatibility date \"2252-04-01\", but the newest date \
             supported by this server binary is \"2026-11-20\".",
            "Can't set compatibility date in the future: \"2252-04-01\". Today's date (UTC) is \
             \"2026-11-18\".",
        ]
    );

    // Between today and the maximum: in the future, but supported.
    let c = compile_at("2026-11-19", &[], &with(DateValidation::CodeVersion));
    assert_eq!(
        errors(&c),
        [
            "Can't set compatibility date in the future: \"2026-11-19\". Today's date (UTC) is \
          \"2026-11-18\"."
        ]
    );
}

#[test]
fn gates_experimental_flags() {
    let code = with(DateValidation::CodeVersion);
    let c = compile_at("2020-01-01", &["durable_object_rename"], &code);
    assert!(is_on(&c, "obsolete19"));
    assert_eq!(
        errors(&c),
        [
            "The compatibility flag durable_object_rename is experimental and may break or be \
          removed in a future version of workerd. To use this flag, you must pass \
          --experimental on the command line."
        ]
    );

    let allowed = CompileOptions {
        allow_experimental: true,
        ..code
    };
    assert!(
        errors(&compile_at(
            "2020-01-01",
            &["durable_object_rename"],
            &allowed
        ))
        .is_empty()
    );

    let allowlisted = CompileOptions {
        allowed_experimental_flags: &[b"durable_object_rename"],
        ..code
    };
    assert!(
        errors(&compile_at(
            "2020-01-01",
            &["durable_object_rename"],
            &allowlisted
        ))
        .is_empty()
    );

    let cloudflare = CompileOptions {
        allowed_experimental_flags: &[b"some_other_flag"],
        ..with(DateValidation::CurrentDateForCloudflare)
    };
    assert_eq!(
        errors(&compile_at(
            "2020-01-01",
            &["durable_object_rename"],
            &cloudflare
        )),
        [
            "The compatibility flag durable_object_rename is experimental and cannot yet be used \
          in Workers deployed to Cloudflare."
        ]
    );

    // durable_object_alarms is obsolete and was never experimental.
    assert!(errors(&compile_at("2020-01-01", &["durable_object_alarms"], &code)).is_empty());
}

#[test]
fn implies_flags_after_their_date() {
    let c = compile_at("2024-09-23", &["nodejs_compat"], &TEST);
    assert!(is_on(&c, "nodeJsCompatV2"));
    let c = compile_at("2024-09-22", &["nodejs_compat"], &TEST);
    assert!(!is_on(&c, "nodeJsCompatV2"));
}

#[test]
fn injects_python_workers() {
    let python = CompileOptions {
        allow_experimental: true,
        main_module_is_python: true,
        ..TEST
    };
    let c = compile_at("2025-10-16", &["auto_inject_python_workers"], &python);
    assert!(errors(&c).is_empty(), "{:?}", errors(&c));
    assert!(is_on(&c, "pythonWorkers"));
    assert!(is_on(&c, "pythonWorkflows"));
    assert!(is_on(&c, "pythonWorkers20250116"));
    assert!(is_on(&c, "pythonDedicatedSnapshot"));

    let c = compile_at("2025-10-16", &[], &python);
    assert!(!is_on(&c, "pythonWorkers"));
}

#[test]
fn round_trips_through_a_builder() {
    let compiled = compile_at("2024-09-23", &["nodejs_compat"], &TEST);
    let mut message = capnp::message::Builder::new_default();
    let builder = message.init_root::<compatibility_flags::Builder<'_>>();
    compiled.write_to(builder).unwrap();
    let reader = message
        .get_root_as_reader::<compatibility_flags::Reader<'_>>()
        .unwrap();
    assert!(reader.get_node_js_compat_v2());
    assert!(reader.get_form_data_parser_supports_files());

    let all = decompile(reader, Decompile::All).unwrap();
    assert!(all.contains(&"nodejs_compat"));
    assert!(all.contains(&"nodejs_compat_v2"));
    assert!(all.contains(&"formdata_parser_supports_files"));

    let fl = decompile(reader, Decompile::NeededByFl).unwrap();
    assert!(fl.len() < all.len());
    assert!(fl.iter().all(|f| all.contains(f)));
}

#[test]
fn maximum_date_is_valid() {
    assert!(maximum_compat_date() >= date_of("2026-01-01"));
    assert!(newest_compat_date() <= maximum_compat_date());
}

// Diagnostics quote the configuration byte for byte, as the C++ compiler did, even where it is
// not UTF-8.
#[test]
fn quotes_text_that_is_not_utf8() {
    let flag: &[u8] = b"bad_\xff_flag";
    let date: &[u8] = b"2021-\xfe";
    let c = compile_with_today(
        date,
        [flag, flag],
        &TEST,
        date_of("2026-11-18"),
        date_of("2026-11-20"),
    )
    .unwrap();
    let errors: Vec<&[u8]> = c.errors().collect();
    assert_eq!(
        errors,
        [
            b"Compatibility flag specified multiple times: bad_\xff_flag".as_slice(),
            b"Invalid compatibility date: 2021-\xfe".as_slice(),
            b"No such compatibility flag: bad_\xff_flag".as_slice(),
        ]
    );
}
