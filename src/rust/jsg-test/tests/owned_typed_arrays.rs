// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Owned typed-array input conversions, including backing-store edge cases.

use jsg::FromJS;
use jsg::v8;

macro_rules! test_owned_typed_array {
    ($test:ident, $elem:ty, $kind:ident, $values:literal, $expected:expr) => {
        #[test]
        fn $test() {
            let harness = crate::Harness::new();
            harness.run_in_context(|lock, ctx| {
                let kind = stringify!($kind);
                let expected: Vec<$elem> = $expected;
                let script = format!(
                    "globalThis.source = new {kind}(4);
                     source.set(new {kind}([{}]), 1);
                     globalThis.view = source.subarray(1, 3); view",
                    $values,
                );
                let mut first: Vec<$elem> = ctx.eval(lock, &script).unwrap();
                assert_eq!(first, expected);
                let retained: Vec<$elem> = ctx.eval(lock, "view").unwrap();
                let raw = ctx.eval_raw(lock, "view").unwrap();
                let attempted = Vec::<$elem>::try_from_js(lock, raw).unwrap().unwrap();
                assert_eq!(attempted, expected);

                // Neither the JS view nor another conversion aliases the returned Vec.
                first.fill(<$elem>::default());
                let unchanged: Vec<$elem> = ctx.eval(lock, "view").unwrap();
                assert_eq!(unchanged, expected);
                ctx.eval_raw(lock, "source.fill(source[0])").unwrap();
                assert_eq!(retained, expected);

                let buffer = ctx.eval_raw(lock, "source.buffer").unwrap();
                v8::Local::<v8::ArrayBuffer>::try_from(buffer)
                    .unwrap()
                    .detach(lock);
                let detached: Vec<$elem> = ctx.eval(lock, "view").unwrap();
                assert!(detached.is_empty());
                assert_eq!(retained, expected);

                for script in [
                    format!("new {kind}(0)"),
                    format!("new {kind}(4).subarray(4)"),
                    format!("new {kind}(new SharedArrayBuffer(0))"),
                ] {
                    let empty: Vec<$elem> = ctx.eval(lock, &script).unwrap();
                    assert!(empty.is_empty());
                }
                assert!(ctx.eval::<Vec<$elem>>(lock, "[1, 2]").is_err());
                assert!(
                    ctx.eval::<Vec<$elem>>(lock, "new DataView(new ArrayBuffer(16))")
                        .is_err()
                );

                // Fixed and length-tracking views can become out of bounds and recover.
                ctx.eval_raw(
                    lock,
                    &format!(
                        "globalThis.buffer = new ArrayBuffer({kind}.BYTES_PER_ELEMENT * 4,
                             {{ maxByteLength: {kind}.BYTES_PER_ELEMENT * 8 }});
                         globalThis.fixed = new {kind}(buffer, {kind}.BYTES_PER_ELEMENT, 2);
                         globalThis.tracking = new {kind}(buffer, {kind}.BYTES_PER_ELEMENT);
                         fixed.set(new {kind}([{}]));",
                        $values,
                    ),
                )
                .unwrap();
                let fixed: Vec<$elem> = ctx.eval(lock, "fixed").unwrap();
                assert_eq!(fixed, expected);
                let tracking: Vec<$elem> = ctx.eval(lock, "tracking").unwrap();
                assert_eq!(&tracking[..2], expected);
                assert_eq!(tracking.len(), 3);

                ctx.eval_raw(
                    lock,
                    &format!("buffer.resize({kind}.BYTES_PER_ELEMENT * 2)"),
                )
                .unwrap();
                let fixed: Vec<$elem> = ctx.eval(lock, "fixed").unwrap();
                assert!(fixed.is_empty());
                let tracking: Vec<$elem> = ctx.eval(lock, "tracking").unwrap();
                assert_eq!(tracking, expected[..1]);
                ctx.eval_raw(lock, "buffer.resize(0)").unwrap();
                let tracking: Vec<$elem> = ctx.eval(lock, "tracking").unwrap();
                assert!(tracking.is_empty());
                ctx.eval_raw(
                    lock,
                    &format!("buffer.resize({kind}.BYTES_PER_ELEMENT * 4)"),
                )
                .unwrap();
                let fixed: Vec<$elem> = ctx.eval(lock, "fixed").unwrap();
                assert_eq!(fixed, vec![<$elem>::default(); 2]);
                assert_eq!(retained, expected);

                // Shared backing stores must be copied without borrowing them as Rust slices.
                for options in ["", ", { maxByteLength: 8 * K.BYTES_PER_ELEMENT }"] {
                    let shared: Vec<$elem> = ctx
                        .eval(
                            lock,
                            &format!(
                                "(() => {{ const K = {kind};
                                    const buffer = new SharedArrayBuffer(
                                        4 * K.BYTES_PER_ELEMENT{options});
                                    const source = new K(buffer);
                                    source.set(new K([{}]), 1);
                                    return source.subarray(1, 3);
                                }})()",
                                $values,
                            ),
                        )
                        .unwrap();
                    assert_eq!(shared, expected);
                }
                Ok(())
            });
        }
    };
}

test_owned_typed_array!(owned_u8, u8, Uint8Array, "1, 255", vec![1, u8::MAX]);
test_owned_typed_array!(owned_u16, u16, Uint16Array, "1, 65535", vec![1, u16::MAX]);
test_owned_typed_array!(
    owned_u32,
    u32,
    Uint32Array,
    "1, 4294967295",
    vec![1, u32::MAX]
);
test_owned_typed_array!(owned_i8, i8, Int8Array, "-128, 127", vec![i8::MIN, i8::MAX]);
test_owned_typed_array!(
    owned_i16,
    i16,
    Int16Array,
    "-32768, 32767",
    vec![i16::MIN, i16::MAX]
);
test_owned_typed_array!(
    owned_i32,
    i32,
    Int32Array,
    "-2147483648, 2147483647",
    vec![i32::MIN, i32::MAX]
);
test_owned_typed_array!(owned_f32, f32, Float32Array, "-1.5, 2.25", vec![-1.5, 2.25]);
test_owned_typed_array!(owned_f64, f64, Float64Array, "-1.5, 2.25", vec![-1.5, 2.25]);
test_owned_typed_array!(
    owned_i64,
    i64,
    BigInt64Array,
    "-9223372036854775808n, 9223372036854775807n",
    vec![i64::MIN, i64::MAX]
);
test_owned_typed_array!(
    owned_u64,
    u64,
    BigUint64Array,
    "1n, 18446744073709551615n",
    vec![1, u64::MAX]
);

#[test]
fn owned_float_bits() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let f32_values: Vec<f32> = ctx
            .eval(
                lock,
                "new Float32Array(new Uint32Array([
                    0, 0x80000000, 1, 0x7f800000, 0xff800000, 0x7fc01234, 0x7f801234
                 ]).buffer)",
            )
            .unwrap();
        assert_eq!(
            f32_values
                .iter()
                .map(|value| value.to_bits())
                .collect::<Vec<_>>(),
            vec![
                0,
                0x8000_0000,
                1,
                0x7f80_0000,
                0xff80_0000,
                0x7fc0_1234,
                0x7f80_1234
            ],
        );
        let f64_values: Vec<f64> = ctx
            .eval(
                lock,
                "new Float64Array(new BigUint64Array([
                    0n, 0x8000000000000000n, 1n, 0x7ff0000000000000n,
                    0xfff0000000000000n, 0x7ff8000000001234n, 0x7ff0000000001234n
                 ]).buffer)",
            )
            .unwrap();
        assert_eq!(
            f64_values
                .iter()
                .map(|value| value.to_bits())
                .collect::<Vec<_>>(),
            vec![
                0,
                0x8000_0000_0000_0000,
                1,
                0x7ff0_0000_0000_0000,
                0xfff0_0000_0000_0000,
                0x7ff8_0000_0000_1234,
                0x7ff0_0000_0000_1234,
            ],
        );
        Ok(())
    });
}
