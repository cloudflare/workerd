// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::EvalContext;
use jsg_test::Harness;

use super::*;

/// Runs `test` with the global `TextEncoderUtil` and `enc`, which calls its static methods as the
/// TypeScript `TextEncoder` does, after converting the arguments.
fn with_text_encoder(test: impl FnOnce(&mut jsg::Lock, &mut EvalContext)) {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<TextEncoderUtil>(lock)?;
        ctx.set_global(lock, "TextEncoderUtil", constructor);
        ctx.eval_raw(
            lock,
            "globalThis.enc = {
                encode: (input) => TextEncoderUtil.encode(`${input}`),
                encodeInto(source, dest) {
                    const counts = new Uint32Array(2);
                    TextEncoderUtil.encodeInto(`${source}`, dest, counts);
                    return { read: counts[0], written: counts[1] };
                },
            };",
        )
        .unwrap();
        test(lock, ctx);
        Ok(())
    });
}

fn eval_string(lock: &mut jsg::Lock<'_>, ctx: &EvalContext, code: &str) -> String {
    ctx.eval::<String>(lock, code).unwrap()
}

#[test]
fn encode_produces_utf8() {
    with_text_encoder(|lock, ctx| {
        let cases = [
            ("enc.encode('abc')", "97,98,99"),
            ("enc.encode('\\u00e9')", "195,169"),
            ("enc.encode('\\u20ac')", "226,130,172"),
            ("enc.encode('\\ud83d\\ude00')", "240,159,152,128"),
            ("enc.encode('')", ""),
        ];
        for (code, expected) in cases {
            assert_eq!(
                eval_string(lock, ctx, &format!("Array.from({code}).join()")),
                expected,
                "{code}"
            );
        }
        assert!(
            ctx.eval::<bool>(lock, "enc.encode('a') instanceof Uint8Array")
                .unwrap()
        );
    });
}

#[test]
fn encode_replaces_lone_surrogates() {
    with_text_encoder(|lock, ctx| {
        assert_eq!(
            eval_string(lock, ctx, "Array.from(enc.encode('a\\ud800b')).join()"),
            "97,239,191,189,98"
        );
        assert_eq!(
            eval_string(lock, ctx, "Array.from(enc.encode('\\udc00')).join()"),
            "239,191,189"
        );
    });
}

#[test]
fn encode_into_fits_whole_code_points() {
    with_text_encoder(|lock, ctx| {
        let cases = [
            // input, destination size, read, written, bytes
            ("'abc'", 8, "3,3,97,98,99,0,0,0,0,0"),
            ("'abc'", 2, "2,2,97,98"),
            ("'\\u00e9\\u00e9'", 3, "1,2,195,169,0"),
            ("'\\u20ac'", 2, "0,0,0,0"),
            ("'a\\ud83d\\ude00'", 4, "1,1,97,0,0,0"),
            ("'a\\ud83d\\ude00'", 5, "3,5,97,240,159,152,128"),
            ("'\\ud800'", 3, "1,3,239,191,189"),
            ("'abc'", 0, "0,0"),
        ];
        for (input, size, expected) in cases {
            let code = format!(
                "(() => {{
                    const dest = new Uint8Array({size});
                    const {{ read, written }} = enc.encodeInto({input}, dest);
                    return [read, written, ...dest].join();
                }})()"
            );
            assert_eq!(
                eval_string(lock, ctx, &code),
                expected,
                "{input} into {size}"
            );
        }
    });
}

#[test]
fn encode_handles_inputs_longer_than_the_stack_scratch() {
    with_text_encoder(|lock, ctx| {
        let cases = [
            ("'a'.repeat(5000)", "5000,97"),
            ("'\\u00e9'.repeat(5000)", "10000,195,169"),
            ("'\\u20ac'.repeat(5000)", "15000,226,130,172"),
            ("'\\ud800'.repeat(5000)", "15000,239,191,189"),
        ];
        for (input, expected) in cases {
            // The length, then the bytes of one repetition, after checking that all repeat.
            let code = format!(
                "(() => {{
                    const bytes = enc.encode({input});
                    const unit = bytes.length / 5000;
                    for (let i = unit; i < bytes.length; i++) {{
                        if (bytes[i] !== bytes[i % unit]) return `mismatch at ${{i}}`;
                    }}
                    return [bytes.length, ...bytes.subarray(0, unit)].join();
                }})()"
            );
            assert_eq!(eval_string(lock, ctx, &code), expected, "{input}");
        }
    });
}

/// Both methods must read a concatenated string, which V8 keeps as a rope of its parts until
/// something flattens it, the same as a flat one.
#[test]
fn methods_read_concatenated_strings() {
    with_text_encoder(|lock, ctx| {
        for (left, right, expected) in [
            ("'ab'", "'c\\u00e9'", "97,98,99,195,169"),
            ("'a\\u20ac'", "'b'", "97,226,130,172,98"),
            ("'a\\ud800'", "'\\udc00b'", "97,240,144,128,128,98"),
            ("'a\\ud800'", "'b'", "97,239,191,189,98"),
        ] {
            // Repeating the left part makes the result long enough for V8 to concatenate lazily.
            let code = format!(
                "(() => {{
                    const prefix = 'x'.repeat(20);
                    const encoded = (s) => [...enc.encode(s).subarray(20)].join();
                    const into = (s) => {{
                        const dest = new Uint8Array(64);
                        const {{ written }} = enc.encodeInto(s, dest);
                        return [...dest.subarray(20, written)].join();
                    }};
                    const results = [encoded(prefix + {left} + {right}), into(prefix + {left} + {right})];
                    return results[0] === results[1] ? results[0] : results.join(' / ');
                }})()"
            );
            assert_eq!(eval_string(lock, ctx, &code), expected, "{left} + {right}");
        }
    });
}

/// `encode()` copies a string that is not flat rather than reading it in place, which would
/// flatten it into a new string on the V8 heap.
#[test]
fn encode_does_not_flatten_its_input() {
    with_text_encoder(|lock, ctx| {
        for (right, expected) in [("'\\u00e9'", "195,169"), ("'\\u20ac'", "226,130,172")] {
            let code = format!("globalThis.s = 'x'.repeat(20) + {right}; s");
            let input = ctx
                .eval_raw(lock, &code)
                .unwrap()
                .to_js_string(lock)
                .unwrap();
            assert!(!input.is_flat(), "{right}");
            assert_eq!(
                eval_string(lock, ctx, "[...enc.encode(s).subarray(20)].join()"),
                expected,
                "{right}"
            );
            assert!(!input.is_flat(), "{right}");
        }
    });
}

#[test]
fn encode_into_handles_long_inputs() {
    with_text_encoder(|lock, ctx| {
        let cases = [
            // input, destination size, read, written
            ("'abc\\u00e9\\u00e9'", 6, "4,5"),
            ("'a'.repeat(5000)", 4999, "4999,4999"),
            ("'\\u00e9'.repeat(5000)", 4999, "2499,4998"),
            ("'\\ud83d\\ude39'.repeat(1000)", 9, "4,8"),
            ("'\\ud83d\\ude39'.repeat(1000)", 10000, "2000,4000"),
            ("'a\\ud800'.repeat(1000)", 100, "50,100"),
            ("'a\\ud800'.repeat(1000)", 5000, "2000,4000"),
        ];
        for (input, size, expected) in cases {
            let code = format!(
                "(() => {{
                    const {{ read, written }} = enc.encodeInto({input}, new Uint8Array({size}));
                    return [read, written].join();
                }})()"
            );
            assert_eq!(
                eval_string(lock, ctx, &code),
                expected,
                "{input} into {size}"
            );
        }
        let code = "(() => {
            const dest = new Uint8Array(8);
            enc.encodeInto('a\\ud800'.repeat(4), dest);
            return dest.join();
        })()";
        assert_eq!(
            eval_string(lock, ctx, code),
            "97,239,191,189,97,239,191,189"
        );
    });
}

#[test]
fn encode_into_fills_the_destination() {
    with_text_encoder(|lock, ctx| {
        // character, code units per character, UTF-8 bytes per character
        let cases = [
            ("\\u{1F639}", 2, 4),
            ("\\u0436", 1, 2),
            ("\\u07ff", 1, 2),
            ("\\u6f22", 1, 3),
        ];
        for (character, units, bytes) in cases {
            for size in [1, 2, 3, 4, 5, 7, 100, 1001, 1003, 2001, 2999] {
                let fit = (size / bytes).min(1000);
                let code = format!(
                    "(() => {{
                        const dest = new Uint8Array({size});
                        const {{ read, written }} = enc.encodeInto('{character}'.repeat(1000), dest);
                        return [read, written].join();
                    }})()"
                );
                assert_eq!(
                    eval_string(lock, ctx, &code),
                    format!("{},{}", fit * units, fit * bytes),
                    "{character} into {size}"
                );
            }
        }
    });
}

#[test]
fn encode_into_handles_surrogates_at_the_end_of_the_destination() {
    with_text_encoder(|lock, ctx| {
        let cases = [
            // input, destination size, read, written
            ("'123\\ud83d\\ude39'", 7, "5,7"),
            ("'123\\ud83d\\ude39'", 6, "3,3"),
            ("'a\\ud800'", 4, "2,4"),
            ("'a\\ud800'", 3, "1,1"),
            ("'a\\ud800b'", 4, "2,4"),
            ("'\\udc00\\ud800'", 6, "2,6"),
        ];
        for (input, size, expected) in cases {
            let code = format!(
                "(() => {{
                    const {{ read, written }} = enc.encodeInto({input}, new Uint8Array({size}));
                    return [read, written].join();
                }})()"
            );
            assert_eq!(
                eval_string(lock, ctx, &code),
                expected,
                "{input} into {size}"
            );
        }
    });
}

#[test]
fn encode_into_respects_the_view_offset() {
    with_text_encoder(|lock, ctx| {
        let code = "(() => {
            const buffer = new Uint8Array(6);
            const view = new Uint8Array(buffer.buffer, 2, 2);
            enc.encodeInto('xyz', view);
            return buffer.join();
        })()";
        assert_eq!(eval_string(lock, ctx, code), "0,0,120,121,0,0");
    });
}

#[test]
fn encode_into_requires_a_uint8_array() {
    with_text_encoder(|lock, ctx| {
        assert!(
            ctx.eval::<bool>(
                lock,
                "try { TextEncoderUtil.encodeInto('a', new Uint16Array(4), new Uint32Array(2)); false } \
                 catch (e) { e instanceof TypeError }"
            )
            .unwrap()
        );
    });
}

/// What `encodeInto()` must return for `units` into `capacity` bytes: each code point in turn,
/// with each lone surrogate as U+FFFD, while it fits.
fn expected_prefix(units: &[u16], capacity: usize) -> (usize, Vec<u8>) {
    let mut read = 0;
    let mut bytes = Vec::new();
    for c in char::decode_utf16(units.iter().copied()) {
        let (c, width) = match c {
            Ok(c) => (c, c.len_utf16()),
            Err(_) => (char::REPLACEMENT_CHARACTER, 1),
        };
        let mut buf = [0; 4];
        let encoded = c.encode_utf8(&mut buf).as_bytes();
        if bytes.len() + encoded.len() > capacity {
            break;
        }
        bytes.extend_from_slice(encoded);
        read += width;
    }
    (read, bytes)
}

fn check_prefixes(name: &str, units: &[u16]) {
    for capacity in 0..=3 * units.len() + 1 {
        let (read, bytes) = expected_prefix(units, capacity);
        let mut output = vec![0; capacity];
        assert_eq!(
            encode_utf16_prefix(units, &mut output),
            (read, bytes.len()),
            "{name} into {capacity}"
        );
        assert_eq!(&output[..bytes.len()], &bytes[..], "{name} into {capacity}");
        if let Ok(latin1) = units
            .iter()
            .map(|&u| u8::try_from(u))
            .collect::<Result<Vec<_>, _>>()
        {
            let mut output = vec![0; capacity];
            assert_eq!(
                encode_latin1_prefix(&latin1, &mut output),
                (read, bytes.len()),
                "{name} as Latin-1 into {capacity}"
            );
            assert_eq!(
                &output[..bytes.len()],
                &bytes[..],
                "{name} as Latin-1 into {capacity}"
            );
        }
    }
}

#[test]
fn prefix_encoders_write_every_code_point_that_fits() {
    let pieces: [(&str, &[u16]); 7] = [
        ("ascii", &[0x61]),
        ("latin1", &[0xe9]),
        ("two-byte", &[0x436]),
        ("three-byte", &[0x6f22]),
        ("pair", &[0xd83d, 0xde39]),
        ("lone lead", &[0xd800]),
        ("lone trail", &[0xdc00]),
    ];
    for (name, piece) in pieces {
        check_prefixes(&format!("{name} x 600"), &piece.repeat(600));
        check_prefixes(
            &format!("a + {name} x 300"),
            &[&[0x61][..], &piece.repeat(300)].concat(),
        );
    }
    // Every piece next to every other, so each boundary case appears somewhere.
    let mixed: Vec<u16> = pieces
        .iter()
        .flat_map(|(_, a)| pieces.iter().flat_map(move |(_, b)| [*a, *b].concat()))
        .collect();
    check_prefixes("mixed", &mixed.repeat(4));
    check_prefixes(
        "ascii then latin1",
        &[&[0x61; 40][..], &[0xe9; 40]].concat(),
    );
}
