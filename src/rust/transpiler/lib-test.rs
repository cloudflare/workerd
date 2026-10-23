use crate::ffi::Output;
use crate::ffi::{self};
use crate::tr_strip_string;

fn tr(src: &str) -> Output {
    tr_strip_string("foo.ts", src.to_owned())
}

#[test]
fn js() {
    let out = tr("let x = 42;");
    assert!(out.success);
    assert_eq!("let x = 42;", out.code);
    assert!(out.diagnostics.is_empty());
}

#[test]
fn ts() {
    let out = tr("let x: Number = 42;");
    assert!(out.success);
    assert_eq!("let x         = 42;", out.code);
    assert!(out.diagnostics.is_empty());
}

#[test]
fn worker() {
    assert_eq!(
        r"
export default {
    async fetch(request, env, ctx)                    {
        return new Response('Hello World from Typescript!');
    },
}                               ;",
        tr(r"
export default {
    async fetch(request, env, ctx): Promise<Response> {
        return new Response('Hello World from Typescript!');
    },
} satisfies ExportedHandler<Env>;")
        .code
    );
}

#[test]
fn erase_enum() {
    // only types are stripped, unsupported typescript construct are reported as errors
    let out = tr(r"enum Foo { A,B,C }");
    assert!(!out.success);
    assert_eq!("", out.code);
    assert_eq!("Unsupported syntax", out.error);
    assert_eq!(
        vec![ffi::Message {
            level: ffi::Level::Error,
            message: "TypeScript enum is not supported in strip-only mode".to_owned()
        }],
        out.diagnostics
    );
}
