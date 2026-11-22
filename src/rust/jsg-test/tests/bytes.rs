// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `jsg::Bytes`, the counterpart of a C++ `kj::Array<kj::byte>` parameter: it takes the bytes
//! of any buffer source, and rejects anything else as C++ does.

use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Sink {}

#[jsg_resource]
impl Sink {
    /// The bytes it was given, as a comma-separated list.
    #[jsg_method]
    pub fn take(&self, bytes: jsg::Bytes) -> String {
        bytes
            .iter()
            .map(u8::to_string)
            .collect::<Vec<_>>()
            .join(",")
    }
}

fn define(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) -> Result<(), jsg::Error> {
    let sink = jsg::Rc::new(lock, Sink {}).to_js(lock)?;
    ctx.set_global(lock, "sink", sink);
    Ok(())
}

#[test]
fn bytes_takes_every_buffer_source() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define(lock, ctx)?;
        let cases = [
            ("sink.take(new Uint8Array([1, 2, 3]))", "1,2,3"),
            (
                "sink.take(new Uint8Array([0, 1, 2, 3, 4]).subarray(1, 3))",
                "1,2",
            ),
            ("sink.take(new Uint16Array([0x0201]))", "1,2"),
            (
                "sink.take(new DataView(new Uint8Array([5, 6, 7]).buffer, 1))",
                "6,7",
            ),
            ("sink.take(new Uint8Array([8, 9]).buffer)", "8,9"),
            ("sink.take(new ArrayBuffer(0))", ""),
            (
                "(() => { const s = new SharedArrayBuffer(2); new Uint8Array(s).set([3, 4]); \
                 return sink.take(s); })()",
                "3,4",
            ),
            ("sink.take(new SharedArrayBuffer(0))", ""),
        ];
        for (code, expected) in cases {
            assert_eq!(ctx.eval::<String>(lock, code).unwrap(), expected, "{code}");
        }
        Ok(())
    });
}

#[test]
fn bytes_copies_what_it_takes() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define(lock, ctx)?;
        let code = "const a = new Uint8Array([1]); const taken = sink.take(a); a[0] = 2; taken";
        assert_eq!(ctx.eval::<String>(lock, code).unwrap(), "1");
        Ok(())
    });
}

#[test]
fn bytes_rejects_anything_else_as_cpp_does() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        define(lock, ctx)?;
        for value in ["'text'", "[1, 2]", "42", "undefined", "{}"] {
            let code = format!(
                "try {{ sink.take({value}); 'ok' }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}"
            );
            assert_eq!(
                ctx.eval::<String>(lock, &code).unwrap(),
                "TypeError: Failed to execute 'take' on 'Sink': parameter 1 is not of type \
                 'ArrayBuffer or ArrayBufferView'.",
                "{value}"
            );
        }
        Ok(())
    });
}
