// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::ToJS;
use jsg::ToJsArrayBuffer;
use jsg::ToJsTypedArray;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct Person {
    pub name: String,
    pub age: u32,
}

#[jsg_resource]
struct ArrayResource;

#[jsg_resource]
impl ArrayResource {
    #[jsg_method]
    pub fn sum(&self, numbers: Vec<u32>) -> u32 {
        numbers.iter().copied().sum()
    }

    #[jsg_method]
    pub fn sum_slice(&self, numbers: &[u32]) -> u32 {
        numbers.iter().copied().sum()
    }

    #[jsg_method]
    pub fn join_strings(&self, strings: &[String]) -> String {
        strings.join("-")
    }

    #[jsg_method]
    pub fn double(&self, numbers: Vec<f64>) -> Vec<f64> {
        numbers.into_iter().map(|n| n * 2.0).collect()
    }

    #[jsg_method]
    pub fn concat_strings(&self, strings: Vec<String>) -> String {
        strings.join(", ")
    }

    #[jsg_method]
    pub fn split_string(&self, s: &str) -> Vec<String> {
        s.split(',').map(|s| s.trim().to_owned()).collect()
    }

    #[jsg_method]
    pub fn filter_positive(&self, numbers: Vec<i32>) -> Vec<i32> {
        numbers.into_iter().filter(|&n| n > 0).collect()
    }

    #[jsg_method]
    pub fn negate_all(&self, flags: Vec<bool>) -> Vec<bool> {
        flags.into_iter().map(|b| !b).collect()
    }

    #[jsg_method]
    pub fn bytes_of(&self, s: &str) -> Vec<u8> {
        s.as_bytes().to_vec()
    }

    #[jsg_method]
    pub fn reverse_bytes<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
    ) -> v8::Local<'s, v8::Uint8Array> {
        let mut reversed = bytes.as_slice(lock).to_vec();
        reversed.reverse();
        reversed.to_js_typed_array(lock)
    }

    #[jsg_method]
    pub fn reverse_bytes_to_buffer<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: v8::Local<'s, v8::Uint8Array>,
    ) -> v8::Local<'s, v8::ArrayBuffer> {
        let mut reversed = bytes.as_slice(lock).to_vec();
        reversed.reverse();
        reversed.to_js_array_buffer(lock)
    }

    #[jsg_method]
    pub fn sum_i32<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        numbers: v8::Local<'s, v8::Int32Array>,
    ) -> i32 {
        numbers.as_slice(lock).iter().sum()
    }

    #[jsg_method]
    pub fn filter_adults(&self, people: Vec<Person>) -> Vec<Person> {
        people.into_iter().filter(|p| p.age >= 18).collect()
    }

    #[jsg_method]
    pub fn double_f32<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        numbers: v8::Local<'s, v8::Float32Array>,
    ) -> v8::Local<'s, v8::Float32Array> {
        let doubled: Vec<f32> = numbers.as_slice(lock).iter().map(|n| n * 2.0).collect();
        doubled.to_js_typed_array(lock)
    }

    #[jsg_method]
    pub fn double_f64<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        numbers: v8::Local<'s, v8::Float64Array>,
    ) -> v8::Local<'s, v8::Float64Array> {
        let doubled: Vec<f64> = numbers.as_slice(lock).iter().map(|n| n * 2.0).collect();
        doubled.to_js_typed_array(lock)
    }

    #[jsg_method]
    pub fn double_i64<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        numbers: v8::Local<'s, v8::BigInt64Array>,
    ) -> v8::Local<'s, v8::BigInt64Array> {
        let doubled: Vec<i64> = numbers.as_slice(lock).iter().map(|n| n * 2).collect();
        doubled.to_js_typed_array(lock)
    }

    #[jsg_method]
    pub fn double_u64<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        numbers: v8::Local<'s, v8::BigUint64Array>,
    ) -> v8::Local<'s, v8::BigUint64Array> {
        let doubled: Vec<u64> = numbers.as_slice(lock).iter().map(|n| n * 2).collect();
        doubled.to_js_typed_array(lock)
    }
}

#[test]
fn resource_accepts_array_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        assert_eq!(
            ctx.eval::<u32>(lock, "arr.sum([1, 2, 3, 4, 5])").unwrap(),
            15
        );

        let result: String = ctx
            .eval(lock, "arr.concatStrings(['hello', 'world'])")
            .unwrap();
        assert_eq!(result, "hello, world");
        Ok(())
    });
}

#[test]
fn resource_accepts_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        assert_eq!(
            ctx.eval::<u32>(lock, "arr.sumSlice([1, 2, 3, 4, 5])")
                .unwrap(),
            15
        );

        let result: String = ctx.eval(lock, "arr.joinStrings(['a', 'b', 'c'])").unwrap();
        assert_eq!(result, "a-b-c");

        assert_eq!(ctx.eval::<u32>(lock, "arr.sumSlice([])").unwrap(), 0);
        Ok(())
    });
}

#[test]
fn resource_returns_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let result: Vec<f64> = ctx.eval(lock, "arr.double([1, 2, 3])").unwrap();
        assert_eq!(result, vec![2.0, 4.0, 6.0]);

        let result: Vec<String> = ctx.eval(lock, "arr.splitString('a, b, c')").unwrap();
        assert_eq!(result, vec!["a", "b", "c"]);

        let result: Vec<i32> = ctx
            .eval(lock, "arr.filterPositive([-1, 2, -3, 4, 0])")
            .unwrap();
        assert_eq!(result, vec![2, 4]);
        Ok(())
    });
}

#[test]
fn resource_accepts_typed_array_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let result: v8::Local<v8::Uint8Array> = ctx
            .eval(lock, "arr.reverseBytes(new Uint8Array([1, 2, 3]))")
            .unwrap();
        assert_eq!(result.as_slice(lock), &[3, 2, 1]);

        let result: i32 = ctx
            .eval(lock, "arr.sumI32(new Int32Array([-10, 20, -5]))")
            .unwrap();
        assert_eq!(result, 5);

        let result: v8::Local<v8::Uint8Array> = ctx
            .eval(lock, "arr.reverseBytes(new Uint8Array([]))")
            .unwrap();
        assert!(result.is_empty());
        Ok(())
    });
}

#[test]
fn resource_returns_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let is_u8: bool = ctx
            .eval(
                lock,
                "arr.reverseBytes(new Uint8Array([1, 2, 3])) instanceof Uint8Array",
            )
            .unwrap();
        assert!(is_u8);
        Ok(())
    });
}

/// A typed array parameter accepts exactly its own type: not another element type, and not an
/// `Array`.
#[test]
fn typed_array_parameter_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let err = ctx
            .eval::<v8::Local<v8::Uint8Array>>(lock, "arr.reverseBytes(new Int8Array([1, 2, 3]))")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(
            err.message,
            "Failed to execute 'reverseBytes' on 'ArrayResource': parameter 1 is not of type \
             'Uint8Array'."
        );

        let result: Result<v8::Local<v8::Uint8Array>, _> =
            ctx.eval(lock, "arr.reverseBytes([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<i32, _> = ctx.eval(lock, "arr.sumI32(new Uint32Array([1, 2, 3]))");
        assert!(result.is_err());

        let result: Result<i32, _> = ctx.eval(lock, "arr.sumI32([1, 2, 3])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn resource_accepts_and_returns_struct_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let result: Vec<Person> = ctx
            .eval(
                lock,
                "arr.filterAdults([{name: 'Alice', age: 25}, {name: 'Bob', age: 15}, {name: 'Charlie', age: 30}])",
            )
            .unwrap();
        assert_eq!(result.len(), 2);
        assert_eq!(result[0].name, "Alice");
        assert_eq!(result[1].name, "Charlie");
        Ok(())
    });
}

#[test]
fn vec_to_js_creates_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec = vec!["hello".to_owned(), "world".to_owned()];
        let js_val = vec.to_js(lock)?;
        ctx.set_global(lock, "arr", js_val);

        let is_array: bool = ctx.eval(lock, "Array.isArray(arr)").unwrap();
        assert!(is_array);
        assert_eq!(ctx.eval::<u32>(lock, "arr.length").unwrap(), 2);
        let first: String = ctx.eval(lock, "arr[0]").unwrap();
        assert_eq!(first, "hello");

        let vec = vec![Some("a".to_owned()), None];
        let js_val = vec.to_js(lock)?;
        ctx.set_global(lock, "options", js_val);
        let second_is_undefined: bool = ctx.eval(lock, "options[1] === undefined").unwrap();
        assert!(second_is_undefined);
        Ok(())
    });
}

/// A `Vec` of `bool`s or `f64`s, or of `Option`s or `Nullable`s of them, converts to an
/// `Array` of the converted elements.
#[test]
fn vec_of_bool_and_number_converts_to_an_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let bools = vec![true, false, true].to_js(lock)?;
        ctx.set_global(lock, "bools", bools);
        let numbers = vec![1.5, -0.0, f64::NAN].to_js(lock)?;
        ctx.set_global(lock, "nums", numbers);
        let options = vec![Some(2.0_f64), None].to_js(lock)?;
        ctx.set_global(lock, "options", options);
        let nullables = vec![jsg::Nullable::Some(false), jsg::Nullable::Null].to_js(lock)?;
        ctx.set_global(lock, "nullables", nullables);

        let ok: bool = ctx
            .eval(
                lock,
                "Array.isArray(bools) && bools.length === 3 && \
                 bools[0] === true && bools[1] === false && bools[2] === true && \
                 Array.isArray(nums) && nums.length === 3 && \
                 nums[0] === 1.5 && Object.is(nums[1], -0) && Number.isNaN(nums[2]) && \
                 Array.isArray(options) && options.length === 2 && \
                 options[0] === 2 && options[1] === undefined && 1 in options && \
                 Array.isArray(nullables) && nullables.length === 2 && \
                 nullables[0] === false && nullables[1] === null",
            )
            .unwrap();
        assert!(ok);

        let wrapped = jsg::Rc::new(ArrayResource).to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);
        let ok: bool = ctx
            .eval(
                lock,
                "const d = arr.double([1, 2.5]); \
                 const n = arr.negateAll([true, false]); \
                 Array.isArray(d) && d.length === 2 && d[0] === 2 && d[1] === 5 && \
                 Array.isArray(n) && n.length === 2 && n[0] === false && n[1] === true",
            )
            .unwrap();
        assert!(ok);
        Ok(())
    });
}

/// A `Vec` of Rust numbers converts to an `Array` of numbers, or of `BigInt`s for `i64`, and not
/// to a typed array, as does one that a method returns.
#[test]
fn vec_of_numbers_converts_to_an_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let bytes = vec![0u8, 7, 255].to_js(lock)?;
        ctx.set_global(lock, "bytes", bytes);
        let words = vec![1u32, u32::MAX].to_js(lock)?;
        ctx.set_global(lock, "words", words);
        let doubles = vec![0.5f64, -0.0, f64::INFINITY].to_js(lock)?;
        ctx.set_global(lock, "doubles", doubles);
        let floats = vec![0.25f32].to_js(lock)?;
        ctx.set_global(lock, "floats", floats);
        let bigints = vec![-1i64, i64::MAX].to_js(lock)?;
        ctx.set_global(lock, "bigints", bigints);
        let empty = Vec::<u8>::new().to_js(lock)?;
        ctx.set_global(lock, "empty", empty);

        let ok: bool = ctx
            .eval(
                lock,
                "Array.isArray(bytes) && bytes.length === 3 && \
                 bytes[0] === 0 && bytes[1] === 7 && bytes[2] === 255 && \
                 Array.isArray(words) && words.length === 2 && \
                 words[0] === 1 && words[1] === 4294967295 && \
                 Array.isArray(doubles) && doubles.length === 3 && doubles[0] === 0.5 && \
                 Object.is(doubles[1], -0) && doubles[2] === Infinity && \
                 Array.isArray(floats) && floats.length === 1 && floats[0] === 0.25 && \
                 Array.isArray(bigints) && bigints.length === 2 && \
                 bigints[0] === -1n && bigints[1] === 2n ** 63n - 1n && \
                 Array.isArray(empty) && empty.length === 0",
            )
            .unwrap();
        assert!(ok);

        let wrapped = jsg::Rc::new(ArrayResource).to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);
        let ok: bool = ctx
            .eval(
                lock,
                "const b = arr.bytesOf('hi'); \
                 Array.isArray(b) && b.length === 2 && b[0] === 104 && b[1] === 105",
            )
            .unwrap();
        assert!(ok);
        Ok(())
    });
}

/// A `Vec` of resource references becomes an `Array` of their wrappers.
#[test]
fn vec_of_resource_references_converts_to_an_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resources = vec![jsg::Rc::new(ArrayResource), jsg::Rc::new(ArrayResource)];
        let js_val = resources.to_js(lock)?;
        ctx.set_global(lock, "resources", js_val);

        let ok: bool = ctx
            .eval(
                lock,
                "Array.isArray(resources) && resources.length === 2 && \
                 resources[0] !== resources[1] && \
                 resources[0].sum([1, 2]) === 3 && resources[1].sum([3, 4]) === 7",
            )
            .unwrap();
        assert!(ok);
        Ok(())
    });
}

/// Converting to an `Array` defines its elements, as C++ JSG's `wrap()` does, so an indexed
/// setter on `Array.prototype` doesn't run, and one that throws doesn't fail the conversion.
#[test]
fn array_conversion_runs_no_prototype_setter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval::<bool>(
            lock,
            "globalThis.setterRan = false;
             Object.defineProperty(Array.prototype, 0, {
               set(v) { globalThis.setterRan = true; throw new Error('setter'); },
               configurable: true,
             });
             true",
        )
        .unwrap();

        let numbers = vec![1.5_f64, 2.5].to_js(lock);
        let strings = vec!["a".to_owned(), "b".to_owned()].to_js(lock);

        ctx.eval::<bool>(lock, "delete Array.prototype[0]").unwrap();

        ctx.set_global(lock, "numbers", numbers?);
        ctx.set_global(lock, "strings", strings?);
        let numbers_ok: bool = ctx
            .eval(
                lock,
                "numbers.length === 2 && numbers[0] === 1.5 && numbers[1] === 2.5",
            )
            .unwrap();
        assert!(numbers_ok);
        let strings: Vec<String> = ctx.eval(lock, "strings").unwrap();
        assert_eq!(strings, vec!["a", "b"]);
        assert!(!ctx.eval::<bool>(lock, "setterRan").unwrap());
        Ok(())
    });
}

#[test]
fn vec_from_js_parses_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let strings: Vec<String> = ctx.eval(lock, "['a', 'b', 'c']").unwrap();
        assert_eq!(strings, vec!["a", "b", "c"]);

        let numbers: Vec<u32> = ctx.eval(lock, "[1, 2, 3]").unwrap();
        assert_eq!(numbers, vec![1, 2, 3]);
        Ok(())
    });
}

#[test]
fn vec_empty_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec: Vec<String> = vec![];
        let js_val = vec.to_js(lock)?;
        ctx.set_global(lock, "arr", js_val);

        assert_eq!(ctx.eval::<u32>(lock, "arr.length").unwrap(), 0);

        let result: Vec<String> = ctx.eval(lock, "arr").unwrap();
        assert!(result.is_empty());
        Ok(())
    });
}

#[test]
fn vec_nested_arrays() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let nested = vec![vec![1_u32, 2], vec![3, 4]];
        let js_val = nested.to_js(lock)?;
        ctx.set_global(lock, "matrix", js_val);

        assert_eq!(
            ctx.eval::<u32>(lock, "matrix[0][0] + matrix[0][1]")
                .unwrap(),
            3
        );
        assert_eq!(
            ctx.eval::<u32>(lock, "matrix[1][0] + matrix[1][1]")
                .unwrap(),
            7
        );
        Ok(())
    });
}

#[test]
fn vec_from_non_array_returns_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: Result<Vec<String>, _> = ctx.eval(lock, "'not an array'");
        assert!(result.is_err());
        Ok(())
    });
}

/// `to_js_typed_array` creates the typed array of each element type, holding the slice's elements.
#[test]
fn to_js_typed_array_creates_each_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        macro_rules! check {
            ($name:literal, [$($elem:expr),*]) => {{
                let data = [$($elem),*];
                let typed = data.to_js_typed_array(lock);
                assert_eq!(typed.as_slice(lock), &data);
                ctx.set_global(lock, "typed", typed);
                let is_instance: bool = ctx
                    .eval(lock, concat!("typed instanceof ", $name))
                    .unwrap();
                assert!(is_instance, $name);
                let is_array: bool = ctx.eval(lock, "Array.isArray(typed)").unwrap();
                assert!(!is_array, $name);
                let length: u32 = ctx.eval(lock, "typed.length").unwrap();
                assert_eq!(length, 3, $name);
            }};
        }
        check!("Uint8Array", [1u8, 2, 255]);
        check!("Uint16Array", [1u16, 2, 65535]);
        check!("Uint32Array", [1u32, 2, 4_294_967_295]);
        check!("Int8Array", [-128i8, 0, 127]);
        check!("Int16Array", [-32768i16, 0, 32767]);
        check!("Int32Array", [-2_147_483_648i32, 0, 2_147_483_647]);
        check!("Float32Array", [f32::MIN, -0.0, f32::MAX]);
        check!("Float64Array", [1.5f64, 2.5, -3.5]);
        check!("BigInt64Array", [i64::MIN, 0, i64::MAX]);
        check!("BigUint64Array", [0u64, 42, u64::MAX]);

        // The elements are what JavaScript sees.
        let val: u8 = ctx.eval(lock, "new Uint8Array([1, 2, 255])[2]").unwrap();
        assert_eq!(val, 255);
        let typed = [100i64, -200].to_js_typed_array(lock);
        ctx.set_global(lock, "typed", typed);
        let val: String = ctx.eval(lock, "String(typed[1])").unwrap();
        assert_eq!(val, "-200");
        Ok(())
    });
}

/// A JavaScript function that describes an `ArrayBuffer` as "is an `ArrayBuffer`:is a view:byte
/// length:bytes".
const DESCRIBE_BUFFER: &str = concat!(
    "(b => `${b instanceof ArrayBuffer}:${ArrayBuffer.isView(b)}:",
    "${b.byteLength}:${new Uint8Array(b).join()}`)"
);

/// `to_js_array_buffer` creates an `ArrayBuffer`, not a view, holding the slice's bytes.
#[test]
fn to_js_array_buffer_creates_array_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let data = [10u8, 20, 0, 255];
        let buffer = data.to_js_array_buffer(lock);
        assert_eq!(buffer.byte_length(), 4);
        assert_eq!(buffer.as_slice(lock), &data);
        ctx.set_global(lock, "buffer", buffer);
        let description: String = ctx
            .eval(lock, &format!("{DESCRIBE_BUFFER}(buffer)"))
            .unwrap();
        assert_eq!(description, "true:false:4:10,20,0,255");

        let empty = [0u8; 0].to_js_array_buffer(lock);
        assert!(empty.is_empty());
        ctx.set_global(lock, "empty", empty);
        let description: String = ctx
            .eval(lock, &format!("{DESCRIBE_BUFFER}(empty)"))
            .unwrap();
        assert_eq!(description, "true:false:0:");
        Ok(())
    });
}

/// A method that returns `to_js_array_buffer`'s handle returns an `ArrayBuffer` to JavaScript.
#[test]
fn resource_returns_array_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let description: String = ctx
            .eval(
                lock,
                &format!("{DESCRIBE_BUFFER}(arr.reverseBytesToBuffer(new Uint8Array([1, 2, 3])))"),
            )
            .unwrap();
        assert_eq!(description, "true:false:3:3,2,1");

        let description: String = ctx
            .eval(
                lock,
                &format!("{DESCRIBE_BUFFER}(arr.reverseBytesToBuffer(new Uint8Array(0)))"),
            )
            .unwrap();
        assert_eq!(description, "true:false:0:");
        Ok(())
    });
}

/// `Local<T>` converts from a typed array of exactly its type.
#[test]
fn typed_array_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let u8_arr: v8::Local<v8::Uint8Array> =
            ctx.eval(lock, "new Uint8Array([10, 20, 255])").unwrap();
        assert_eq!(u8_arr.as_slice(lock), &[10, 20, 255]);

        let u16_arr: v8::Local<v8::Uint16Array> = ctx
            .eval(lock, "new Uint16Array([100, 200, 65535])")
            .unwrap();
        assert_eq!(u16_arr.as_slice(lock), &[100, 200, 65535]);

        let u32_arr: v8::Local<v8::Uint32Array> = ctx
            .eval(lock, "new Uint32Array([1000, 2000, 4294967295])")
            .unwrap();
        assert_eq!(u32_arr.as_slice(lock), &[1000, 2000, 4_294_967_295]);

        let i8_arr: v8::Local<v8::Int8Array> =
            ctx.eval(lock, "new Int8Array([-128, 0, 127])").unwrap();
        assert_eq!(i8_arr.as_slice(lock), &[-128, 0, 127]);

        let i16_arr: v8::Local<v8::Int16Array> = ctx
            .eval(lock, "new Int16Array([-32768, 0, 32767])")
            .unwrap();
        assert_eq!(i16_arr.as_slice(lock), &[-32768, 0, 32767]);

        let i32_arr: v8::Local<v8::Int32Array> = ctx
            .eval(lock, "new Int32Array([-2147483648, 0, 2147483647])")
            .unwrap();
        assert_eq!(i32_arr.as_slice(lock), &[-2_147_483_648, 0, 2_147_483_647]);

        let f32_arr: v8::Local<v8::Float32Array> =
            ctx.eval(lock, "new Float32Array([1.5, 2.5])").unwrap();
        assert_eq!(f32_arr.as_slice(lock), &[1.5, 2.5]);

        let f64_arr: v8::Local<v8::Float64Array> =
            ctx.eval(lock, "new Float64Array([1.1, 2.2])").unwrap();
        assert_eq!(f64_arr.as_slice(lock), &[1.1, 2.2]);

        let i64_arr: v8::Local<v8::BigInt64Array> =
            ctx.eval(lock, "new BigInt64Array([1n, -2n, 3n])").unwrap();
        assert_eq!(i64_arr.as_slice(lock), &[1, -2, 3]);

        let u64_arr: v8::Local<v8::BigUint64Array> =
            ctx.eval(lock, "new BigUint64Array([1n, 2n, 3n])").unwrap();
        assert_eq!(u64_arr.as_slice(lock), &[1, 2, 3]);
        Ok(())
    });
}

#[test]
fn typed_array_empty_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let typed = [0u8; 0].to_js_typed_array(lock);
        assert!(typed.is_empty());
        ctx.set_global(lock, "arr", typed);

        assert_eq!(ctx.eval::<u32>(lock, "arr.length").unwrap(), 0);

        let result: v8::Local<v8::Uint8Array> = ctx.eval(lock, "arr").unwrap();
        assert!(result.as_slice(lock).is_empty());
        Ok(())
    });
}

/// `Local<T>` rejects a typed array of another element type, an `Array` and a primitive.
#[test]
fn typed_array_type_mismatch_returns_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: Result<v8::Local<v8::Uint8Array>, _> =
            ctx.eval(lock, "new Int8Array([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<v8::Local<v8::Int32Array>, _> =
            ctx.eval(lock, "new Uint32Array([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<v8::Local<v8::Float32Array>, _> =
            ctx.eval(lock, "new Float64Array([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<v8::Local<v8::BigInt64Array>, _> =
            ctx.eval(lock, "new BigUint64Array([1n, 2n])");
        assert!(result.is_err());

        let result: Result<v8::Local<v8::Uint8Array>, _> = ctx.eval(lock, "[1, 2, 3]");
        assert!(result.is_err());

        let result: Result<v8::Local<v8::Uint8Array>, _> = ctx.eval(lock, "'hello'");
        assert!(result.is_err());
        Ok(())
    });
}

#[test]
fn large_typed_array_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let data: Vec<u8> = (0_u32..65536).map(|i| (i % 256) as u8).collect();
        let typed = data.to_js_typed_array(lock);
        ctx.set_global(lock, "arr", typed);

        assert_eq!(ctx.eval::<u32>(lock, "arr.length").unwrap(), 65536);

        let result: v8::Local<v8::Uint8Array> = ctx.eval(lock, "arr").unwrap();
        assert_eq!(result.as_slice(lock), data.as_slice());
        Ok(())
    });
}

#[test]
fn typed_array_iter_uint8() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [10u8, 20, 30].to_js_typed_array(lock);

        assert_eq!(typed.len(), 3);
        assert!(!typed.is_empty());

        assert_eq!(typed.get(0), 10);
        assert_eq!(typed.get(1), 20);
        assert_eq!(typed.get(2), 30);

        let sum: u8 = typed.iter().fold(0u8, u8::wrapping_add);
        assert_eq!(sum, 60);

        let collected: Vec<u8> = typed.iter().collect();
        assert_eq!(collected, vec![10, 20, 30]);

        Ok(())
    });
}

#[test]
fn typed_array_into_iter_uint8() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [1u8, 2, 3, 4, 5].to_js_typed_array(lock);

        let sum: u8 = typed.into_iter().sum();
        assert_eq!(sum, 15);

        Ok(())
    });
}

#[test]
fn typed_array_iter_int32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [-100i32, 0, 100].to_js_typed_array(lock);

        assert_eq!(typed.len(), 3);
        assert_eq!(typed.get(0), -100);
        assert_eq!(typed.get(1), 0);
        assert_eq!(typed.get(2), 100);

        let sum: i32 = typed.iter().sum();
        assert_eq!(sum, 0);

        Ok(())
    });
}

#[test]
fn typed_array_iter_reverse() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [1u8, 2, 3, 4].to_js_typed_array(lock);

        let reversed: Vec<u8> = typed.iter().rev().collect();
        assert_eq!(reversed, vec![4, 3, 2, 1]);

        Ok(())
    });
}

#[test]
fn typed_array_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [0u8; 0].to_js_typed_array(lock);

        assert_eq!(typed.len(), 0);
        assert!(typed.is_empty());
        assert_eq!(typed.iter().count(), 0);

        Ok(())
    });
}

// =============================================================================
// Float and BigInt typed arrays as parameters and return values
// =============================================================================

#[test]
fn float32_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let is_f32: bool = ctx
            .eval(
                lock,
                "arr.doubleF32(new Float32Array([1.0, 2.0])) instanceof Float32Array",
            )
            .unwrap();
        assert!(is_f32);

        let result: v8::Local<v8::Float32Array> = ctx
            .eval(lock, "arr.doubleF32(new Float32Array([1.0, 2.0, 3.0]))")
            .unwrap();
        assert_eq!(result.as_slice(lock), &[2.0, 4.0, 6.0]);

        let result: v8::Local<v8::Float32Array> = ctx
            .eval(lock, "arr.doubleF32(new Float32Array([]))")
            .unwrap();
        assert!(result.is_empty());

        // Float32Array rejects Float64Array and Array.
        let result: Result<v8::Local<v8::Float32Array>, _> =
            ctx.eval(lock, "arr.doubleF32(new Float64Array([1.0, 2.0]))");
        assert!(result.is_err());
        let result: Result<v8::Local<v8::Float32Array>, _> =
            ctx.eval(lock, "arr.doubleF32([1.0, 2.0])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn float64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let is_f64: bool = ctx
            .eval(
                lock,
                "arr.doubleF64(new Float64Array([1.0, 2.0])) instanceof Float64Array",
            )
            .unwrap();
        assert!(is_f64);

        let result: v8::Local<v8::Float64Array> = ctx
            .eval(lock, "arr.doubleF64(new Float64Array([1.0, 2.0, 3.0]))")
            .unwrap();
        assert_eq!(result.as_slice(lock), &[2.0, 4.0, 6.0]);

        // Float64Array rejects Float32Array and Array.
        let result: Result<v8::Local<v8::Float64Array>, _> =
            ctx.eval(lock, "arr.doubleF64(new Float32Array([1.0, 2.0]))");
        assert!(result.is_err());
        let result: Result<v8::Local<v8::Float64Array>, _> =
            ctx.eval(lock, "arr.doubleF64([1.0, 2.0])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn bigint64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let is_i64: bool = ctx
            .eval(
                lock,
                "arr.doubleI64(new BigInt64Array([1n, 2n])) instanceof BigInt64Array",
            )
            .unwrap();
        assert!(is_i64);

        let result: v8::Local<v8::BigInt64Array> = ctx
            .eval(lock, "arr.doubleI64(new BigInt64Array([1n, -2n, 3n]))")
            .unwrap();
        assert_eq!(result.as_slice(lock), &[2, -4, 6]);

        // BigInt64Array rejects BigUint64Array and Int32Array.
        let result: Result<v8::Local<v8::BigInt64Array>, _> =
            ctx.eval(lock, "arr.doubleI64(new BigUint64Array([1n, 2n]))");
        assert!(result.is_err());
        let result: Result<v8::Local<v8::BigInt64Array>, _> =
            ctx.eval(lock, "arr.doubleI64(new Int32Array([1, 2]))");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn biguint64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);

        let is_u64: bool = ctx
            .eval(
                lock,
                "arr.doubleU64(new BigUint64Array([1n, 2n])) instanceof BigUint64Array",
            )
            .unwrap();
        assert!(is_u64);

        let result: v8::Local<v8::BigUint64Array> = ctx
            .eval(lock, "arr.doubleU64(new BigUint64Array([1n, 2n, 3n]))")
            .unwrap();
        assert_eq!(result.as_slice(lock), &[2, 4, 6]);

        // BigUint64Array rejects BigInt64Array and Uint32Array.
        let result: Result<v8::Local<v8::BigUint64Array>, _> =
            ctx.eval(lock, "arr.doubleU64(new BigInt64Array([1n, 2n]))");
        assert!(result.is_err());
        let result: Result<v8::Local<v8::BigUint64Array>, _> =
            ctx.eval(lock, "arr.doubleU64(new Uint32Array([1, 2]))");
        assert!(result.is_err());

        Ok(())
    });
}

// =============================================================================
// as_slice / as_mut_slice / byte_offset / data / FromJS for Local<T>
// =============================================================================

/// Helper resource whose methods accept `Local<'_, T>` directly via `FromJS`.
#[jsg_resource]
struct SliceResource;

#[jsg_resource]
impl SliceResource {
    /// Accepts a `Local<Uint8Array>` directly and sums its elements via `as_slice`.
    #[jsg_method]
    pub fn sum_u8_local<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        arr: jsg::v8::Local<'s, jsg::v8::Uint8Array>,
    ) -> u32 {
        arr.as_slice(lock).iter().map(|&x| u32::from(x)).sum()
    }

    /// Accepts a `Local<Int32Array>` directly and sums its elements via `as_slice`.
    #[jsg_method]
    pub fn sum_i32_local<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        arr: jsg::v8::Local<'s, jsg::v8::Int32Array>,
    ) -> i32 {
        arr.as_slice(lock).iter().sum()
    }

    /// Accepts a `Local<Float64Array>` directly and sums its elements via `as_slice`.
    #[jsg_method]
    pub fn sum_f64_local<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        arr: jsg::v8::Local<'s, jsg::v8::Float64Array>,
    ) -> f64 {
        arr.as_slice(lock).iter().copied().sum()
    }

    /// Writes `0xFF` into every byte via `as_mut_slice` and returns the length.
    #[jsg_method]
    pub fn fill_ff<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        mut arr: jsg::v8::Local<'s, jsg::v8::Uint8Array>,
    ) -> u32 {
        // SAFETY: no other reference into this buffer is live during this call;
        // lock borrow prevents JS from running.
        unsafe { arr.as_mut_slice(lock) }.fill(0xFF);
        u32::try_from(arr.len()).unwrap()
    }
}

#[test]
fn typed_array_as_slice_zero_copy_sum() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(SliceResource);
        ctx.set_global(lock, "r", res);

        // Uint8Array
        let result: u32 = ctx
            .eval(lock, "r.sumU8Local(new Uint8Array([10, 20, 30]))")
            .unwrap();
        assert_eq!(result, 60);

        // Int32Array
        let result: i32 = ctx
            .eval(lock, "r.sumI32Local(new Int32Array([-1, 0, 1]))")
            .unwrap();
        assert_eq!(result, 0);

        // Float64Array
        let result: f64 = ctx
            .eval(lock, "r.sumF64Local(new Float64Array([1.5, 2.5]))")
            .unwrap();
        assert!((result - 4.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn typed_array_as_slice_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [0u8; 0].to_js_typed_array(lock);

        assert_eq!(typed.as_slice(lock), &[] as &[u8]);
        Ok(())
    });
}

#[test]
fn typed_array_as_mut_slice_mutations_visible_in_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(SliceResource);
        ctx.set_global(lock, "r", res);

        // fill_ff writes 0xFF into every element in-place.
        // The returned length tells us how many bytes were written.
        let len: u32 = ctx
            .eval(lock, "r.fillFf(new Uint8Array([1, 2, 3]))")
            .unwrap();
        assert_eq!(len, 3);

        // Independently verify with a captured reference: create an array in JS,
        // pass it in, then read back the same object.
        let check: u32 = ctx
            .eval(
                lock,
                r"
                    const buf = new Uint8Array([0, 0, 0]);
                    r.fillFf(buf);
                    buf[0] === 255 && buf[1] === 255 && buf[2] === 255 ? 1 : 0
                ",
            )
            .unwrap();
        assert_eq!(check, 1);

        Ok(())
    });
}

#[test]
fn typed_array_byte_offset_non_zero() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Create a view that starts 4 bytes into an 8-byte buffer.
        // byte_offset() must return 4, len() must return 1.
        let js_val = ctx
            .eval_raw(
                lock,
                r"
                    const buf = new ArrayBuffer(8);
                    const view = new Uint32Array(buf, 4, 1);
                    view
                ",
            )
            .unwrap();
        let typed: jsg::v8::Local<'_, jsg::v8::Uint32Array> =
            // SAFETY: isolate valid; js_val is a Uint32Array Local.
            unsafe { jsg::v8::Local::from_ffi(js_val.into_ffi()) };

        assert_eq!(typed.byte_offset(), 4);
        assert_eq!(typed.byte_length(), 4); // 1 element × 4 bytes/u32
        assert_eq!(typed.len(), 1);
        Ok(())
    });
}

#[test]
fn typed_array_as_slice_with_byte_offset() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Build a 12-byte buffer [0,1,2,...,11], create a Uint8Array view
        // starting at offset 4 covering 4 bytes: [4, 5, 6, 7].
        let js_val = ctx
            .eval_raw(
                lock,
                r"
                    const buf = new ArrayBuffer(12);
                    const all = new Uint8Array(buf);
                    for (let i = 0; i < 12; i++) all[i] = i;
                    new Uint8Array(buf, 4, 4)
                ",
            )
            .unwrap();
        let typed: jsg::v8::Local<'_, jsg::v8::Uint8Array> =
            // SAFETY: isolate valid; js_val is a Uint8Array Local.
            unsafe { jsg::v8::Local::from_ffi(js_val.into_ffi()) };

        assert_eq!(typed.byte_offset(), 4);
        assert_eq!(typed.len(), 4);
        assert_eq!(typed.as_slice(lock), &[4u8, 5, 6, 7]);
        Ok(())
    });
}

#[test]
fn typed_array_data_pointer_and_byte_offset() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [10u8, 20, 30, 40].to_js_typed_array(lock);

        // A freshly created TypedArray has byte_offset == 0.
        assert_eq!(typed.byte_offset(), 0);

        // data() + byte_offset() must point at the first element.
        // as_slice() is derived from exactly this computation, so they must agree.
        let slice = typed.as_slice(lock);
        // SAFETY: as_slice() contracts guarantee the pointer + len are valid here.
        let from_ptr: &[u8] = unsafe {
            std::slice::from_raw_parts(
                typed.data().byte_add(typed.byte_offset()).cast_const(),
                typed.len(),
            )
        };
        assert_eq!(slice, from_ptr);
        assert_eq!(from_ptr, &[10u8, 20, 30, 40]);
        Ok(())
    });
}

#[test]
fn typed_array_from_js_local_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(SliceResource);
        ctx.set_global(lock, "r", res);

        // sumU8Local expects Uint8Array — Int8Array must be rejected.
        let result: Result<u32, _> = ctx.eval(lock, "r.sumU8Local(new Int8Array([1, 2, 3]))");
        assert!(result.is_err());

        // Regular Array must also be rejected.
        let result: Result<u32, _> = ctx.eval(lock, "r.sumU8Local([1, 2, 3])");
        assert!(result.is_err());

        // sumI32Local expects Int32Array — Uint32Array must be rejected.
        let result: Result<i32, _> = ctx.eval(lock, "r.sumI32Local(new Uint32Array([1, 2, 3]))");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn typed_array_element_size_and_is_integer_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // u8 / Uint8Array: 1 byte per element, integer type.
        let arr = [0u8, 0].to_js_typed_array(lock);
        assert_eq!(arr.element_size(), 1);
        assert!(arr.is_integer_type());

        // u32 / Uint32Array: 4 bytes per element, integer type.
        let arr = [0u32, 0].to_js_typed_array(lock);
        assert_eq!(arr.element_size(), 4);
        assert!(arr.is_integer_type());

        // f32 / Float32Array: 4 bytes per element, NOT an integer type.
        let arr = [0.0f32, 0.0].to_js_typed_array(lock);
        assert_eq!(arr.element_size(), 4);
        assert!(!arr.is_integer_type());

        // f64 / Float64Array: 8 bytes per element, NOT an integer type.
        let arr = [0.0f64, 0.0].to_js_typed_array(lock);
        assert_eq!(arr.element_size(), 8);
        assert!(!arr.is_integer_type());

        Ok(())
    });
}

#[test]
fn uint8clamped_array_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        use jsg::v8::Local;
        use jsg::v8::Uint8ClampedArray;

        let val = ctx
            .eval_raw(lock, "new Uint8ClampedArray([0, 128, 255])")
            .unwrap();
        assert!(val.is_uint8clamped_array());
        assert!(!val.is_uint8_array());

        let arr: Local<Uint8ClampedArray> =
            // SAFETY: isolate valid; val is a Uint8ClampedArray Local.
            unsafe { Local::from_ffi(val.into_ffi()) };
        assert_eq!(arr.len(), 3);
        assert_eq!(arr.get(0), 0u8);
        assert_eq!(arr.get(1), 128u8);
        assert_eq!(arr.get(2), 255u8);
        assert_eq!(arr.as_slice(lock), &[0u8, 128, 255]);
        assert!(arr.is_integer_type());
        assert_eq!(arr.element_size(), 1);

        Ok(())
    });
}

#[test]
fn uint8clamped_array_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        use jsg::FromJS;
        use jsg::v8::Local;
        use jsg::v8::Uint8ClampedArray;

        let val = ctx.eval_raw(lock, "new Uint8Array([1, 2, 3])").unwrap();
        let result = Local::<Uint8ClampedArray>::from_js(lock, val);
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn is_float16_array_check() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let f16_val = ctx.eval_raw(lock, "new Float16Array([1.0, 2.0])").unwrap();
        assert!(f16_val.is_float16_array());
        assert!(!f16_val.is_float32_array());

        let f32_val = ctx.eval_raw(lock, "new Float32Array([1.0])").unwrap();
        assert!(!f32_val.is_float16_array());

        Ok(())
    });
}

/// With `fast_jsg_struct`, converting a `Vec` or a `#[jsg_struct]` to JavaScript defines the
/// elements and fields as own data properties, as C++ JSG does: it runs no JavaScript, so no
/// setter a script put on `Array.prototype` or `Object.prototype` is called, not even one that
/// throws.
#[test]
fn to_js_with_fast_jsg_struct_calls_no_prototype_setters() {
    let harness = crate::Harness::with_flags(|mut flags| flags.set_fast_jsg_struct(true));
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);
        ctx.eval_raw(
            lock,
            // A string, since pushing to an array would run the setters on Array.prototype.
            "globalThis.settersRan = '';
             for (const [proto, key] of [[Array.prototype, 0], [Array.prototype, 1],
                                         [Object.prototype, 'name'], [Object.prototype, 'age']]) {
               Object.defineProperty(proto, key, {
                 set(v) { settersRan += `${key};`; throw new Error('setter ran'); },
                 configurable: true,
               });
             }",
        )
        .expect("setup should evaluate");

        let people = vec![
            Person {
                name: "Alice".to_owned(),
                age: 25,
            },
            Person {
                name: "Bob".to_owned(),
                age: 30,
            },
        ];
        let value = people.to_js(lock)?;
        ctx.set_global(lock, "people", value);
        let ok: bool = ctx
            .eval(
                lock,
                "Array.isArray(people) && people.length === 2 &&
                 Object.hasOwn(people, 0) && Object.hasOwn(people, 1) &&
                 people.every(p => Object.keys(p).join() === 'name,age') &&
                 people[0].name === 'Alice' && people[0].age === 25 &&
                 people[1].name === 'Bob' && people[1].age === 30",
            )
            .unwrap();
        assert!(ok);

        // The same through a method's arguments and return value.
        let ok: bool = ctx
            .eval(
                lock,
                "const adults = arr.filterAdults([{name: 'Carol', age: 40}, {name: 'Dan', age: 50}]);
                 const doubled = arr.double([1, 2]);
                 Object.hasOwn(adults, 0) && Object.hasOwn(adults[0], 'name') &&
                 adults[1].name === 'Dan' && Object.hasOwn(doubled, 1) && doubled[1] === 4",
            )
            .unwrap();
        assert!(ok);

        let setters_ran: String = ctx.eval(lock, "settersRan").unwrap();
        assert_eq!(setters_ran, "");
        Ok(())
    });
}

/// Without `fast_jsg_struct`, a `#[jsg_struct]`'s fields are set one by one, as C++ JSG sets
/// them with `Set()`, so a setter on `Object.prototype` runs and takes the field's value. A
/// `Vec`'s elements are still defined as data properties, as C++ JSG defines them at every
/// compatibility date.
#[test]
fn struct_to_js_without_fast_jsg_struct_runs_object_prototype_setters() {
    // `Harness::new()` starts with every flag off, `fast_jsg_struct` included.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(
            lock,
            // A string, since pushing to an array would run the setters on Array.prototype.
            "globalThis.settersRan = '';
             for (const key of [0, 1]) {
               Object.defineProperty(Array.prototype, key, {
                 set(v) { settersRan += `[${key}]`; throw new Error('setter ran'); },
                 configurable: true,
               });
             }
             Object.defineProperty(Object.prototype, 'name', {
               set(v) { settersRan += `${v};`; },
               configurable: true,
             });",
        )
        .expect("setup should evaluate");

        let people = vec![
            Person {
                name: "Alice".to_owned(),
                age: 25,
            },
            Person {
                name: "Bob".to_owned(),
                age: 30,
            },
        ];
        let value = people.to_js(lock)?;
        ctx.set_global(lock, "people", value);
        let ok: bool = ctx
            .eval(
                lock,
                "Object.hasOwn(people, 0) && Object.hasOwn(people, 1) &&
                 people.every(p => Object.keys(p).join() === 'age') &&
                 people[0].age === 25 && people[1].age === 30",
            )
            .unwrap();
        assert!(ok, "the setter did not take the name");
        let setters_ran: String = ctx.eval(lock, "settersRan").unwrap();
        assert_eq!(setters_ran, "Alice;Bob;");
        Ok(())
    });
}

/// Without `fast_jsg_struct`, what a setter on `Object.prototype` throws while a `#[jsg_struct]`
/// is converted reaches the caller unchanged, from Rust and from a method's return value.
#[test]
fn struct_to_js_without_fast_jsg_struct_passes_on_what_a_setter_throws() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ArrayResource);
        let wrapped = resource.to_js(lock)?;
        ctx.set_global(lock, "arr", wrapped);
        ctx.eval_raw(
            lock,
            "globalThis.thrown = new RangeError('setter');
             Object.defineProperty(Object.prototype, 'age', {
               set(v) { throw thrown; },
               configurable: true,
             });",
        )
        .expect("setup should evaluate");

        let person = Person {
            name: "Alice".to_owned(),
            age: 25,
        };
        let caught = lock.try_catch(|lock| person.to_js(lock))?;
        let Err(exception) = caught else {
            panic!("converting the struct did not throw");
        };
        ctx.set_global(lock, "caught", exception);
        assert!(ctx.eval::<bool>(lock, "caught === thrown").unwrap());

        let ok: bool = ctx
            .eval(
                lock,
                "let fromMethod;
                 try { arr.filterAdults([{name: 'Carol', age: 40}]); }
                 catch (e) { fromMethod = e; }
                 fromMethod === thrown",
            )
            .unwrap();
        assert!(
            ok,
            "the method's caller did not catch what the setter threw"
        );
        Ok(())
    });
}

/// Converting a `Vec` stops at the first element whose conversion fails: without
/// `fast_jsg_struct`, a struct after the one whose setter threw is not converted.
#[test]
fn vec_to_js_stops_at_the_first_element_that_fails() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.eval_raw(
            lock,
            "globalThis.names = '';
             Object.defineProperty(Object.prototype, 'name', {
               set(v) { names += `${v};`; if (v === 'Bob') throw new Error('Bob'); },
               configurable: true,
             });",
        )
        .expect("setup should evaluate");
        let people = ["Alice", "Bob", "Carol"]
            .into_iter()
            .map(|name| Person {
                name: name.to_owned(),
                age: 30,
            })
            .collect::<Vec<_>>();
        let caught = lock.try_catch(|lock| people.to_js(lock))?;
        assert!(caught.is_err(), "converting the Vec did not throw");
        let names: String = ctx.eval(lock, "names").unwrap();
        assert_eq!(names, "Alice;Bob;");
        Ok(())
    });
}
