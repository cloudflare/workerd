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
fn test_nothrow_extern_c_function() {
    let source = r#"
        #[cxx::bridge]
        mod ffi {
            struct Pair {
                a: u32,
                b: u32,
            }
            unsafe extern "C++" {
                #[nothrow]
                fn nothrow_len(s: &str) -> usize;
                #[nothrow]
                fn nothrow_pair() -> Pair;
                #[nothrow]
                fn nothrow_string() -> String;
                #[nothrow]
                fn nothrow_void(n: usize);
            }
        }
    "#;
    let opt = cxx_gen::Opt::default();
    let generated = generate_header_and_cc(source.parse().unwrap(), &opt).unwrap();
    let output = str::from_utf8(&generated.implementation).unwrap();
    // Called with no exception handling, returning through the out parameter as any shim does.
    assert!(
        output.contains("void cxxbridge1$nothrow_len(::rust::Str s, ::std::size_t *return$) {")
    );
    assert!(output.contains("new (return$) ::std::size_t(nothrow_len$(s));"));
    assert!(output.contains("void cxxbridge1$nothrow_pair(::Pair *return$) {"));
    assert!(output.contains("void cxxbridge1$nothrow_string(::rust::String *return$) {"));
    assert!(output.contains("new (return$) ::rust::String(nothrow_string$());"));
    assert!(output.contains("void cxxbridge1$nothrow_void(::std::size_t n) {"));
    assert!(!output.contains("Result::run("));
}

#[test]
fn test_nothrow_rejected() {
    for (decl, error) in [
        (
            "fn f() -> Result<()>;",
            "#[nothrow] function cannot return Result",
        ),
        ("async fn f();", "#[nothrow] function cannot be async"),
        (
            "fn f(callback: fn(u8));",
            "#[nothrow] function cannot take a function pointer",
        ),
    ] {
        let source =
            format!("#[cxx::bridge] mod ffi {{ unsafe extern \"C++\" {{ #[nothrow] {decl} }} }}");
        let opt = cxx_gen::Opt::default();
        let Err(err) = generate_header_and_cc(source.parse().unwrap(), &opt) else {
            panic!("expected an error for {decl}");
        };
        assert!(err.to_string().contains(error), "{decl}: {err}");
    }
    let source = r#"#[cxx::bridge] mod ffi { extern "Rust" { #[nothrow] fn f(); } }"#;
    let opt = cxx_gen::Opt::default();
    let Err(err) = generate_header_and_cc(source.parse().unwrap(), &opt) else {
        panic!("expected an error for #[nothrow] on an extern Rust function");
    };
    assert!(err.to_string().contains("unsupported attribute"), "{err}");
}

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
    assert!(implementation.contains("static_assert(!::kj::isPointerType<::Thing>()"));
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
    assert!(implementation.contains("static_assert(!::kj::isPointerType<::Thing>()"));
    assert!(!implementation.contains("is_base_of<::kj::AtomicRefcounted"));
    assert!(!implementation.contains("cxxbridge1$kj_rs$arc$"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}
