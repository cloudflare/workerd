use std::str;

use cxx_gen::generate_header_and_cc;

const BRIDGE0: &str = r#"
    #[cxx::bridge]
    mod ffi {
        unsafe extern "C++" {
            pub fn do_cpp_thing(foo: &str);
        }
    }
"#;

const BRIDGE1: &str = r"
    #[cxx::bridge]
    mod ffi {
        #[derive(JsgStruct)]
        struct MyStruct {
            field1: i32,
            field2: String,
        }
    }
";

const BRIDGE2: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            own: KjOwn<Thing>,
        }

        struct MultiHolder {
            first: KjOwn<Thing>,
            second: KjOwn<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

const BRIDGE3: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            rc: KjRc<Thing>,
        }

        struct MultiHolder {
            first: KjRc<Thing>,
            second: KjRc<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

const BRIDGE4: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            arc: KjArc<Thing>,
        }

        struct MultiHolder {
            first: KjArc<Thing>,
            second: KjArc<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

#[test]
fn test_extern_c_function() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE0.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.implementation).unwrap();
    // To avoid continual breakage we won't test every byte.
    // Let's look for the major features.
    assert!(
        output.contains("::rust::repr::Result cxxbridge1$do_cpp_thing(::rust::Str foo) noexcept")
    );
    // The shim must catch every exception: letting one escape the extern "C" boundary
    // would terminate the process.
    assert!(output.contains("return ::rust::repr::Result::run("));
}

#[test]
fn test_impl_annotation() {
    let opt = cxx_gen::Opt {
        cxx_impl_annotations: Some("ANNOTATION".to_owned()),
        ..Default::default()
    };
    let source = BRIDGE0.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.implementation).unwrap();
    assert!(
        output.contains("ANNOTATION ::rust::repr::Result cxxbridge1$do_cpp_thing(::rust::Str foo)")
    );
}

#[test]
fn test_jsg_struct_derive() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE1.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.header).unwrap();
    assert!(output.contains("JSG_STRUCT(field1, field2);"));
    assert!(output.contains("jsg.h"));
}

#[test]
fn test_kj_own_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE2.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Own<::Thing> own;"));
    assert!(header.contains("::kj::Own<::Thing> first;"));
    assert!(header.contains("::kj::Own<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}

#[test]
fn test_kj_rc_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE3.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Rc<::Thing> rc;"));
    assert!(header.contains("::kj::Rc<::Thing> first;"));
    assert!(header.contains("::kj::Rc<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}

#[test]
fn test_kj_arc_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE4.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Arc<::Thing> arc;"));
    assert!(header.contains("::kj::Arc<::Thing> first;"));
    assert!(header.contains("::kj::Arc<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    assert!(
        implementation.contains("static_assert(sizeof(::kj::Arc<::Thing>) == 2 * sizeof(void *)")
    );
    assert!(!implementation.contains("is_base_of<::kj::AtomicRefcounted"));
    assert!(!implementation.contains("cxxbridge1$kj_rs$arc$"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}

// Every spelling of a borrowing async `extern "Rust"` function the bridge accepts as safe, plus
// the `unsafe` forms that take on a longer borrow.
const ASYNC_BORROWS: &str = r#"
    #[cxx::bridge]
    mod ffi {
        extern "Rust" {
            type Borrower;

            async fn elided(buf: &[u8], text: &str) -> Result<u64>;
            async fn elided_self(self: &Borrower, text: &str) -> Result<u64>;
            async fn elided_self_shorthand(&self, delta: u64) -> u64;
            async fn named<'a>(buf: &'a [u8], tail: &'a [u8]) -> Result<()>;
            async unsafe fn named_self<'a>(self: &'a Borrower, text: &'a str) -> Result<u64>;
            async unsafe fn keeps_static<'a>(buf: &'a [u8], forever: &'static str) -> u64;
            async unsafe fn returns_borrow<'a>(text: &'a str) -> &'a str;
        }
    }
"#;

#[test]
fn test_async_borrows() {
    let opt = cxx_gen::Opt::default();
    let source = ASYNC_BORROWS.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    assert!(header.contains("kj::Promise<::std::uint64_t> elided(::rust::Slice<::std::uint8_t const> buf, ::rust::Str text)"));
    assert!(header.contains("kj::Promise<::std::uint64_t> elided_self(::rust::Str text) const"));
    assert!(header.contains(
        "kj::Promise<::std::uint64_t> elided_self_shorthand(::std::uint64_t delta) const"
    ));
    assert!(header.contains("kj::Promise<void> named("));
    assert!(header.contains("kj::Promise<::rust::Str> returns_borrow(::rust::Str text)"));
}

// The borrows a safe async function may not expose: a settled result C++ can keep after the
// argument it borrows is gone, and an argument bound for longer than the promise.
fn async_bridge(function: &str) -> String {
    format!(
        r#"
        #[cxx::bridge]
        mod ffi {{
            extern "Rust" {{
                type Borrower;
                {function}
            }}
        }}
        "#
    )
}

fn generation_error(function: &str) -> String {
    let opt = cxx_gen::Opt::default();
    let source = async_bridge(function)
        .parse()
        .unwrap_or_else(|err| panic!("{function}: {err}"));
    match generate_header_and_cc(source, &opt) {
        Ok(_) => panic!("accepted: {function}"),
        Err(err) => err.to_string(),
    }
}

#[test]
fn test_async_reference_result_rejected() {
    for function in [
        "async fn expose<'a>(input: &'a i32) -> &'a i32;",
        "async unsafe fn expose<'a>(input: &'a i32) -> &'a i32;",
    ] {
        let error = generation_error(function);
        assert!(
            error.contains("async function cannot return a reference"),
            "{error}"
        );
    }
}

#[test]
fn test_async_borrowed_result_requires_unsafe() {
    for function in [
        "async fn expose<'a>(input: &'a str) -> &'a str;",
        "async fn expose<'a>(input: &'a [u8]) -> &'a [u8];",
        "async fn expose<'a>(input: &'a i32) -> KjMaybe<&'a i32>;",
    ] {
        let error = generation_error(function);
        assert!(
            error.contains("must be `unsafe fn expose` in order to return a borrow"),
            "{error}"
        );
    }
}

#[test]
fn test_async_static_argument_requires_unsafe() {
    for function in [
        "async fn keep<'a>(input: &'a i32, forever: &'static i32) -> i32;",
        "async fn keep(input: &i32, forever: &'static i32) -> i32;",
        "async fn keep(forever: &'static i32) -> i32;",
        "async fn keep(forever: &'static [u8]) -> i32;",
        "async fn keep(self: &'static Borrower) -> i32;",
    ] {
        let error = generation_error(function);
        assert!(
            error.contains("borrows for `'static`, longer than its promise"),
            "{error}"
        );
        assert!(error.contains("requires `unsafe fn keep`"), "{error}");
    }
}
