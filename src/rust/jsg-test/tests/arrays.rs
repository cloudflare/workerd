use jsg::Number;
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct Person {
    pub name: String,
    pub age: Number,
}

#[jsg_resource]
struct ArrayResource {
    _state: ResourceState,
}

#[jsg_resource]
impl ArrayResource {
    #[jsg_method]
    pub fn sum(&self, numbers: Vec<Number>) -> Number {
        Number::new(numbers.iter().map(|n| n.value()).sum())
    }

    #[jsg_method]
    pub fn sum_slice(&self, numbers: &[Number]) -> Number {
        Number::new(numbers.iter().map(|n| n.value()).sum())
    }

    #[jsg_method]
    pub fn join_strings(&self, strings: &[String]) -> String {
        strings.join("-")
    }

    #[jsg_method]
    pub fn double(&self, numbers: Vec<Number>) -> Vec<Number> {
        numbers
            .into_iter()
            .map(|n| Number::new(n.value() * 2.0))
            .collect()
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
    pub fn filter_positive(&self, numbers: Vec<Number>) -> Vec<Number> {
        numbers.into_iter().filter(|n| n.value() > 0.0).collect()
    }

    #[jsg_method]
    pub fn reverse_bytes(&self, bytes: Vec<u8>) -> Vec<u8> {
        bytes.into_iter().rev().collect()
    }

    #[jsg_method]
    pub fn reverse_bytes_slice(&self, bytes: &[u8]) -> Vec<u8> {
        bytes.iter().copied().rev().collect()
    }

    #[jsg_method]
    pub fn sum_i32(&self, numbers: Vec<i32>) -> Number {
        Number::new(numbers.iter().map(|&n| f64::from(n)).sum())
    }

    #[jsg_method]
    pub fn sum_i32_slice(&self, numbers: &[i32]) -> Number {
        Number::new(numbers.iter().map(|&n| f64::from(n)).sum())
    }

    #[jsg_method]
    pub fn filter_adults(&self, people: Vec<Person>) -> Vec<Person> {
        people
            .into_iter()
            .filter(|p| p.age.value() >= 18.0)
            .collect()
    }

    // Float32Array methods
    #[jsg_method]
    pub fn sum_f32(&self, numbers: Vec<f32>) -> Number {
        Number::new(numbers.iter().map(|&n| f64::from(n)).sum())
    }

    #[jsg_method]
    pub fn sum_f32_slice(&self, numbers: &[f32]) -> Number {
        Number::new(numbers.iter().map(|&n| f64::from(n)).sum())
    }

    #[jsg_method]
    pub fn double_f32(&self, numbers: Vec<f32>) -> Vec<f32> {
        numbers.into_iter().map(|n| n * 2.0).collect()
    }

    // Float64Array methods
    #[jsg_method]
    pub fn sum_f64(&self, numbers: Vec<f64>) -> Number {
        Number::new(numbers.iter().sum())
    }

    #[jsg_method]
    pub fn sum_f64_slice(&self, numbers: &[f64]) -> Number {
        Number::new(numbers.iter().sum())
    }

    #[jsg_method]
    pub fn double_f64(&self, numbers: Vec<f64>) -> Vec<f64> {
        numbers.into_iter().map(|n| n * 2.0).collect()
    }

    // BigInt64Array methods
    #[jsg_method]
    pub fn sum_i64(&self, numbers: Vec<i64>) -> Number {
        Number::new(numbers.iter().map(|&n| n as f64).sum())
    }

    #[jsg_method]
    pub fn sum_i64_slice(&self, numbers: &[i64]) -> Number {
        Number::new(numbers.iter().map(|&n| n as f64).sum())
    }

    #[jsg_method]
    pub fn double_i64(&self, numbers: Vec<i64>) -> Vec<i64> {
        numbers.into_iter().map(|n| n * 2).collect()
    }

    // BigUint64Array methods
    #[jsg_method]
    pub fn sum_u64(&self, numbers: Vec<u64>) -> Number {
        Number::new(numbers.iter().map(|&n| n as f64).sum())
    }

    #[jsg_method]
    pub fn sum_u64_slice(&self, numbers: &[u64]) -> Number {
        Number::new(numbers.iter().map(|&n| n as f64).sum())
    }

    #[jsg_method]
    pub fn double_u64(&self, numbers: Vec<u64>) -> Vec<u64> {
        numbers.into_iter().map(|n| n * 2).collect()
    }
}

#[test]
fn resource_accepts_array_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        let result: Number = ctx.eval(lock, "arr.sum([1, 2, 3, 4, 5])").unwrap();
        assert!((result.value() - 15.0).abs() < f64::EPSILON);

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
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        let result: Number = ctx.eval(lock, "arr.sumSlice([1, 2, 3, 4, 5])").unwrap();
        assert!((result.value() - 15.0).abs() < f64::EPSILON);

        let result: String = ctx.eval(lock, "arr.joinStrings(['a', 'b', 'c'])").unwrap();
        assert_eq!(result, "a-b-c");

        let result: Number = ctx.eval(lock, "arr.sumSlice([])").unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn resource_returns_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        let result: Vec<Number> = ctx.eval(lock, "arr.double([1, 2, 3])").unwrap();
        let values: Vec<f64> = result.iter().map(|n| n.value()).collect();
        assert_eq!(values, vec![2.0, 4.0, 6.0]);

        let result: Vec<String> = ctx.eval(lock, "arr.splitString('a, b, c')").unwrap();
        assert_eq!(result, vec!["a", "b", "c"]);

        let result: Vec<Number> = ctx
            .eval(lock, "arr.filterPositive([-1, 2, -3, 4, 0])")
            .unwrap();
        let values: Vec<f64> = result.iter().map(|n| n.value()).collect();
        assert_eq!(values, vec![2.0, 4.0]);
        Ok(())
    });
}

#[test]
fn resource_accepts_typed_array_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        let result: Vec<u8> = ctx
            .eval(lock, "arr.reverseBytes(new Uint8Array([1, 2, 3]))")
            .unwrap();
        assert_eq!(result, vec![3, 2, 1]);

        let result: Number = ctx
            .eval(lock, "arr.sumI32(new Int32Array([-10, 20, -5]))")
            .unwrap();
        assert!((result.value() - 5.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn resource_returns_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

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

#[test]
fn resource_accepts_typed_array_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test &[u8] accepts Uint8Array
        let result: Vec<u8> = ctx
            .eval(lock, "arr.reverseBytesSlice(new Uint8Array([1, 2, 3]))")
            .unwrap();
        assert_eq!(result, vec![3, 2, 1]);

        // Test &[i32] accepts Int32Array
        let result: Number = ctx
            .eval(lock, "arr.sumI32Slice(new Int32Array([-10, 20, -5]))")
            .unwrap();
        assert!((result.value() - 5.0).abs() < f64::EPSILON);

        // Test empty TypedArray
        let result: Vec<u8> = ctx
            .eval(lock, "arr.reverseBytesSlice(new Uint8Array([]))")
            .unwrap();
        assert!(result.is_empty());

        Ok(())
    });
}

#[test]
fn typed_array_slice_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // &[u8] should reject Int8Array
        let result: Result<Vec<u8>, _> =
            ctx.eval(lock, "arr.reverseBytesSlice(new Int8Array([1, 2, 3]))");
        assert!(result.is_err());

        // &[u8] should reject regular Array
        let result: Result<Vec<u8>, _> = ctx.eval(lock, "arr.reverseBytesSlice([1, 2, 3])");
        assert!(result.is_err());

        // &[i32] should reject Uint32Array
        let result: Result<Number, _> =
            ctx.eval(lock, "arr.sumI32Slice(new Uint32Array([1, 2, 3]))");
        assert!(result.is_err());

        // &[i32] should reject regular Array
        let result: Result<Number, _> = ctx.eval(lock, "arr.sumI32Slice([1, 2, 3])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn resource_accepts_and_returns_struct_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

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
        let js_val = vec.to_js(lock);
        ctx.set_global("arr", js_val);

        let is_array: bool = ctx.eval(lock, "Array.isArray(arr)").unwrap();
        assert!(is_array);
        let length: Number = ctx.eval(lock, "arr.length").unwrap();
        assert!((length.value() - 2.0).abs() < f64::EPSILON);
        let first: String = ctx.eval(lock, "arr[0]").unwrap();
        assert_eq!(first, "hello");

        let vec = vec![Number::new(1.5), Number::new(2.5), Number::new(3.5)];
        let js_val = vec.to_js(lock);
        ctx.set_global("nums", js_val);

        let sum: Number = ctx.eval(lock, "nums[0] + nums[1] + nums[2]").unwrap();
        assert!((sum.value() - 7.5).abs() < f64::EPSILON);

        let vec = vec![true, false, true];
        let js_val = vec.to_js(lock);
        ctx.set_global("bools", js_val);

        let result: Vec<bool> = ctx.eval(lock, "bools").unwrap();
        assert_eq!(result, vec![true, false, true]);
        Ok(())
    });
}

#[test]
fn vec_from_js_parses_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let strings: Vec<String> = ctx.eval(lock, "['a', 'b', 'c']").unwrap();
        assert_eq!(strings, vec!["a", "b", "c"]);

        let numbers: Vec<Number> = ctx.eval(lock, "[1, 2, 3]").unwrap();
        let values: Vec<f64> = numbers.iter().map(|n| n.value()).collect();
        assert_eq!(values, vec![1.0, 2.0, 3.0]);
        Ok(())
    });
}

#[test]
fn vec_empty_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec: Vec<String> = vec![];
        let js_val = vec.to_js(lock);
        ctx.set_global("arr", js_val);

        let length: Number = ctx.eval(lock, "arr.length").unwrap();
        assert!(length.value().abs() < f64::EPSILON);

        let result: Vec<String> = ctx.eval(lock, "arr").unwrap();
        assert!(result.is_empty());
        Ok(())
    });
}

#[test]
fn vec_nested_arrays() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let nested = vec![
            vec![Number::new(1.0), Number::new(2.0)],
            vec![Number::new(3.0), Number::new(4.0)],
        ];
        let js_val = nested.to_js(lock);
        ctx.set_global("matrix", js_val);

        let first_row_sum: Number = ctx.eval(lock, "matrix[0][0] + matrix[0][1]").unwrap();
        assert!((first_row_sum.value() - 3.0).abs() < f64::EPSILON);

        let second_row_sum: Number = ctx.eval(lock, "matrix[1][0] + matrix[1][1]").unwrap();
        assert!((second_row_sum.value() - 7.0).abs() < f64::EPSILON);
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

#[test]
fn typed_array_to_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec: Vec<u8> = vec![1, 2, 255];
        ctx.set_global("u8_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "u8_arr instanceof Uint8Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "u8_arr[2]").unwrap();
        assert!((val.value() - 255.0).abs() < f64::EPSILON);

        let vec: Vec<u16> = vec![1, 2, 65535];
        ctx.set_global("u16_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "u16_arr instanceof Uint16Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "u16_arr[2]").unwrap();
        assert!((val.value() - 65535.0).abs() < f64::EPSILON);

        let vec: Vec<u32> = vec![1, 2, 4_294_967_295];
        ctx.set_global("u32_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "u32_arr instanceof Uint32Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "u32_arr[2]").unwrap();
        assert!((val.value() - 4_294_967_295.0).abs() < f64::EPSILON);

        let vec: Vec<i8> = vec![-128, 0, 127];
        ctx.set_global("i8_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i8_arr instanceof Int8Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "i8_arr[0]").unwrap();
        assert!((val.value() - (-128.0)).abs() < f64::EPSILON);

        let vec: Vec<i16> = vec![-32768, 0, 32767];
        ctx.set_global("i16_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i16_arr instanceof Int16Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "i16_arr[0]").unwrap();
        assert!((val.value() - (-32768.0)).abs() < f64::EPSILON);

        let vec: Vec<i32> = vec![-2_147_483_648, 0, 2_147_483_647];
        ctx.set_global("i32_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i32_arr instanceof Int32Array").unwrap();
        assert!(check);
        let val: Number = ctx.eval(lock, "i32_arr[0]").unwrap();
        assert!((val.value() - (-2_147_483_648.0)).abs() < f64::EPSILON);

        let is_array: bool = ctx.eval(lock, "Array.isArray(u8_arr)").unwrap();
        assert!(!is_array);
        Ok(())
    });
}

#[test]
fn typed_array_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let u8_arr: Vec<u8> = ctx.eval(lock, "new Uint8Array([10, 20, 255])").unwrap();
        assert_eq!(u8_arr, vec![10, 20, 255]);

        let u16_arr: Vec<u16> = ctx
            .eval(lock, "new Uint16Array([100, 200, 65535])")
            .unwrap();
        assert_eq!(u16_arr, vec![100, 200, 65535]);

        let u32_arr: Vec<u32> = ctx
            .eval(lock, "new Uint32Array([1000, 2000, 4294967295])")
            .unwrap();
        assert_eq!(u32_arr, vec![1000, 2000, 4_294_967_295]);

        let i8_arr: Vec<i8> = ctx.eval(lock, "new Int8Array([-128, 0, 127])").unwrap();
        assert_eq!(i8_arr, vec![-128, 0, 127]);

        let i16_arr: Vec<i16> = ctx
            .eval(lock, "new Int16Array([-32768, 0, 32767])")
            .unwrap();
        assert_eq!(i16_arr, vec![-32768, 0, 32767]);

        let i32_arr: Vec<i32> = ctx
            .eval(lock, "new Int32Array([-2147483648, 0, 2147483647])")
            .unwrap();
        assert_eq!(i32_arr, vec![-2_147_483_648, 0, 2_147_483_647]);
        Ok(())
    });
}

#[test]
fn typed_array_empty_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec: Vec<u8> = vec![];
        ctx.set_global("arr", vec.to_js(lock));

        let length: Number = ctx.eval(lock, "arr.length").unwrap();
        assert!(length.value().abs() < f64::EPSILON);

        let result: Vec<u8> = ctx.eval(lock, "arr").unwrap();
        assert!(result.is_empty());
        Ok(())
    });
}

#[test]
fn typed_array_type_mismatch_returns_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let result: Result<Vec<u8>, _> = ctx.eval(lock, "new Int8Array([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<Vec<i32>, _> = ctx.eval(lock, "new Uint32Array([1, 2, 3])");
        assert!(result.is_err());

        let result: Result<Vec<u8>, _> = ctx.eval(lock, "[1, 2, 3]");
        assert!(result.is_err());

        let result: Result<Vec<u8>, _> = ctx.eval(lock, "'hello'");
        assert!(result.is_err());
        Ok(())
    });
}

#[test]
fn large_typed_array_roundtrip() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let vec: Vec<u8> = (0_u32..65536).map(|i| (i % 256) as u8).collect();
        ctx.set_global("arr", vec.clone().to_js(lock));

        let length: Number = ctx.eval(lock, "arr.length").unwrap();
        assert!((length.value() - 65536.0).abs() < f64::EPSILON);

        let result: Vec<u8> = ctx.eval(lock, "arr").unwrap();
        assert_eq!(result, vec);
        Ok(())
    });
}

#[test]
fn typed_array_iter_uint8() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u8> = vec![10, 20, 30];
        let js_val = data.to_js(lock);
        let typed: jsg::v8::Local<'_, jsg::v8::Uint8Array> =
            unsafe { jsg::v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };

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
        let data: Vec<u8> = vec![1, 2, 3, 4, 5];
        let js_val = data.to_js(lock);
        let typed: jsg::v8::Local<'_, jsg::v8::Uint8Array> =
            unsafe { jsg::v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };

        let sum: u8 = typed.into_iter().sum();
        assert_eq!(sum, 15);

        Ok(())
    });
}

#[test]
fn typed_array_iter_int32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<i32> = vec![-100, 0, 100];
        let js_val = data.to_js(lock);
        let typed: jsg::v8::Local<'_, jsg::v8::Int32Array> =
            unsafe { jsg::v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };

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
        let data: Vec<u8> = vec![1, 2, 3, 4];
        let js_val = data.to_js(lock);
        let typed: jsg::v8::Local<'_, jsg::v8::Uint8Array> =
            unsafe { jsg::v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };

        let reversed: Vec<u8> = typed.iter().rev().collect();
        assert_eq!(reversed, vec![4, 3, 2, 1]);

        Ok(())
    });
}

#[test]
fn typed_array_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u8> = vec![];
        let js_val = data.to_js(lock);
        let typed: jsg::v8::Local<'_, jsg::v8::Uint8Array> =
            unsafe { jsg::v8::Local::from_ffi(lock.isolate(), js_val.into_ffi()) };

        assert_eq!(typed.len(), 0);
        assert!(typed.is_empty());
        assert_eq!(typed.iter().count(), 0);

        Ok(())
    });
}

// =============================================================================
// Float32Array tests
// =============================================================================

#[test]
fn float32_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test Vec<f32> parameter
        let result: Number = ctx
            .eval(lock, "arr.sumF32(new Float32Array([1.5, 2.5, 3.0]))")
            .unwrap();
        assert!((result.value() - 7.0).abs() < f64::EPSILON);

        // Test Vec<f32> return value
        let is_f32: bool = ctx
            .eval(
                lock,
                "arr.doubleF32(new Float32Array([1.0, 2.0])) instanceof Float32Array",
            )
            .unwrap();
        assert!(is_f32);

        let result: Vec<f32> = ctx
            .eval(lock, "arr.doubleF32(new Float32Array([1.0, 2.0, 3.0]))")
            .unwrap();
        assert_eq!(result, vec![2.0, 4.0, 6.0]);

        Ok(())
    });
}

#[test]
fn float32_array_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test &[f32] parameter
        let result: Number = ctx
            .eval(lock, "arr.sumF32Slice(new Float32Array([1.5, 2.5, 3.0]))")
            .unwrap();
        assert!((result.value() - 7.0).abs() < f64::EPSILON);

        // Test empty Float32Array
        let result: Number = ctx
            .eval(lock, "arr.sumF32Slice(new Float32Array([]))")
            .unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn float32_array_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Float32Array should reject Float64Array
        let result: Result<Number, _> =
            ctx.eval(lock, "arr.sumF32Slice(new Float64Array([1.0, 2.0]))");
        assert!(result.is_err());

        // Float32Array should reject regular Array
        let result: Result<Number, _> = ctx.eval(lock, "arr.sumF32Slice([1.0, 2.0])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn float32_array_to_js_and_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Test ToJS: Vec<f32> -> Float32Array
        let data: Vec<f32> = vec![1.5, 2.5, 3.5, 4.5];
        let js_value = data.to_js(lock);
        ctx.set_global("f32arr", js_value);

        let is_f32: bool = ctx.eval(lock, "f32arr instanceof Float32Array").unwrap();
        assert!(is_f32);

        let len: Number = ctx.eval(lock, "f32arr.length").unwrap();
        assert_eq!(len.value() as usize, 4);

        // Test FromJS: Float32Array -> Vec<f32>
        let result: Vec<f32> = ctx.eval(lock, "f32arr").unwrap();
        assert_eq!(result.len(), 4);
        assert!((result[0] - 1.5).abs() < f32::EPSILON);
        assert!((result[1] - 2.5).abs() < f32::EPSILON);
        assert!((result[2] - 3.5).abs() < f32::EPSILON);
        assert!((result[3] - 4.5).abs() < f32::EPSILON);

        // Test roundtrip with special values
        let special: Vec<f32> = vec![f32::MIN, f32::MAX, 0.0, -0.0];
        let js_special = special.to_js(lock);
        ctx.set_global("special", js_special);
        let roundtrip: Vec<f32> = ctx.eval(lock, "special").unwrap();
        assert_eq!(roundtrip[0], f32::MIN);
        assert_eq!(roundtrip[1], f32::MAX);

        Ok(())
    });
}

// =============================================================================
// Float64Array tests
// =============================================================================

#[test]
fn float64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test Vec<f64> parameter
        let result: Number = ctx
            .eval(lock, "arr.sumF64(new Float64Array([1.5, 2.5, 3.0]))")
            .unwrap();
        assert!((result.value() - 7.0).abs() < f64::EPSILON);

        // Test Vec<f64> return value
        let is_f64: bool = ctx
            .eval(
                lock,
                "arr.doubleF64(new Float64Array([1.0, 2.0])) instanceof Float64Array",
            )
            .unwrap();
        assert!(is_f64);

        let result: Vec<f64> = ctx
            .eval(lock, "arr.doubleF64(new Float64Array([1.0, 2.0, 3.0]))")
            .unwrap();
        assert_eq!(result, vec![2.0, 4.0, 6.0]);

        Ok(())
    });
}

#[test]
fn float64_array_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test &[f64] parameter
        let result: Number = ctx
            .eval(lock, "arr.sumF64Slice(new Float64Array([1.5, 2.5, 3.0]))")
            .unwrap();
        assert!((result.value() - 7.0).abs() < f64::EPSILON);

        // Test empty Float64Array
        let result: Number = ctx
            .eval(lock, "arr.sumF64Slice(new Float64Array([]))")
            .unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn float64_array_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Float64Array should reject Float32Array
        let result: Result<Number, _> =
            ctx.eval(lock, "arr.sumF64Slice(new Float32Array([1.0, 2.0]))");
        assert!(result.is_err());

        // Float64Array should reject regular Array
        let result: Result<Number, _> = ctx.eval(lock, "arr.sumF64Slice([1.0, 2.0])");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn float64_array_to_js_and_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Test Vec<f64> to JavaScript
        let vec: Vec<f64> = vec![1.5, 2.5, -3.5];
        ctx.set_global("f64_arr", vec.to_js(lock));

        let is_f64: bool = ctx.eval(lock, "f64_arr instanceof Float64Array").unwrap();
        assert!(is_f64);

        let val: Number = ctx.eval(lock, "f64_arr[0]").unwrap();
        assert!((val.value() - 1.5).abs() < f64::EPSILON);

        let val: Number = ctx.eval(lock, "f64_arr[2]").unwrap();
        assert!((val.value() - (-3.5)).abs() < f64::EPSILON);

        // Test Float64Array from JavaScript
        let result: Vec<f64> = ctx.eval(lock, "new Float64Array([1.1, 2.2, 3.3])").unwrap();
        assert!((result[0] - 1.1).abs() < f64::EPSILON);
        assert!((result[1] - 2.2).abs() < f64::EPSILON);
        assert!((result[2] - 3.3).abs() < f64::EPSILON);

        Ok(())
    });
}

// =============================================================================
// BigInt64Array tests
// =============================================================================

#[test]
fn bigint64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test Vec<i64> parameter
        let result: Number = ctx
            .eval(lock, "arr.sumI64(new BigInt64Array([1n, 2n, 3n]))")
            .unwrap();
        assert!((result.value() - 6.0).abs() < f64::EPSILON);

        // Test Vec<i64> return value
        let is_i64: bool = ctx
            .eval(
                lock,
                "arr.doubleI64(new BigInt64Array([1n, 2n])) instanceof BigInt64Array",
            )
            .unwrap();
        assert!(is_i64);

        let result: Vec<i64> = ctx
            .eval(lock, "arr.doubleI64(new BigInt64Array([1n, 2n, 3n]))")
            .unwrap();
        assert_eq!(result, vec![2, 4, 6]);

        // Test with negative values
        let result: Number = ctx
            .eval(lock, "arr.sumI64(new BigInt64Array([-10n, 20n, -5n]))")
            .unwrap();
        assert!((result.value() - 5.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn bigint64_array_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test &[i64] parameter
        let result: Number = ctx
            .eval(lock, "arr.sumI64Slice(new BigInt64Array([10n, 20n, 30n]))")
            .unwrap();
        assert!((result.value() - 60.0).abs() < f64::EPSILON);

        // Test empty BigInt64Array
        let result: Number = ctx
            .eval(lock, "arr.sumI64Slice(new BigInt64Array([]))")
            .unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn bigint64_array_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // BigInt64Array should reject BigUint64Array
        let result: Result<Number, _> =
            ctx.eval(lock, "arr.sumI64Slice(new BigUint64Array([1n, 2n]))");
        assert!(result.is_err());

        // BigInt64Array should reject Int32Array
        let result: Result<Number, _> = ctx.eval(lock, "arr.sumI64Slice(new Int32Array([1, 2]))");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn bigint64_array_to_js_and_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Test Vec<i64> to JavaScript
        let vec: Vec<i64> = vec![100, -200, 300];
        ctx.set_global("i64_arr", vec.to_js(lock));

        let is_i64: bool = ctx.eval(lock, "i64_arr instanceof BigInt64Array").unwrap();
        assert!(is_i64);

        // BigInt64Array returns BigInt values, verify via string
        let val: String = ctx.eval(lock, "String(i64_arr[0])").unwrap();
        assert_eq!(val, "100");

        let val: String = ctx.eval(lock, "String(i64_arr[1])").unwrap();
        assert_eq!(val, "-200");

        // Test BigInt64Array from JavaScript
        let result: Vec<i64> = ctx.eval(lock, "new BigInt64Array([1n, -2n, 3n])").unwrap();
        assert_eq!(result, vec![1, -2, 3]);

        Ok(())
    });
}

// =============================================================================
// BigUint64Array tests
// =============================================================================

#[test]
fn biguint64_array_parameter_and_return() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test Vec<u64> parameter
        let result: Number = ctx
            .eval(lock, "arr.sumU64(new BigUint64Array([1n, 2n, 3n]))")
            .unwrap();
        assert!((result.value() - 6.0).abs() < f64::EPSILON);

        // Test Vec<u64> return value
        let is_u64: bool = ctx
            .eval(
                lock,
                "arr.doubleU64(new BigUint64Array([1n, 2n])) instanceof BigUint64Array",
            )
            .unwrap();
        assert!(is_u64);

        let result: Vec<u64> = ctx
            .eval(lock, "arr.doubleU64(new BigUint64Array([1n, 2n, 3n]))")
            .unwrap();
        assert_eq!(result, vec![2, 4, 6]);

        Ok(())
    });
}

#[test]
fn biguint64_array_slice_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // Test &[u64] parameter
        let result: Number = ctx
            .eval(lock, "arr.sumU64Slice(new BigUint64Array([10n, 20n, 30n]))")
            .unwrap();
        assert!((result.value() - 60.0).abs() < f64::EPSILON);

        // Test empty BigUint64Array
        let result: Number = ctx
            .eval(lock, "arr.sumU64Slice(new BigUint64Array([]))")
            .unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn biguint64_array_rejects_wrong_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(ArrayResource {
            _state: ResourceState::default(),
        });
        let mut template = ArrayResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("arr", wrapped);

        // BigUint64Array should reject BigInt64Array
        let result: Result<Number, _> =
            ctx.eval(lock, "arr.sumU64Slice(new BigInt64Array([1n, 2n]))");
        assert!(result.is_err());

        // BigUint64Array should reject Uint32Array
        let result: Result<Number, _> = ctx.eval(lock, "arr.sumU64Slice(new Uint32Array([1, 2]))");
        assert!(result.is_err());

        Ok(())
    });
}

#[test]
fn biguint64_array_to_js_and_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Test Vec<u64> to JavaScript
        let vec: Vec<u64> = vec![100, 200, 300];
        ctx.set_global("u64_arr", vec.to_js(lock));

        let is_u64: bool = ctx.eval(lock, "u64_arr instanceof BigUint64Array").unwrap();
        assert!(is_u64);

        // BigUint64Array returns BigInt values, verify via string
        let val: String = ctx.eval(lock, "String(u64_arr[0])").unwrap();
        assert_eq!(val, "100");

        let val: String = ctx.eval(lock, "String(u64_arr[2])").unwrap();
        assert_eq!(val, "300");

        // Test BigUint64Array from JavaScript
        let result: Vec<u64> = ctx.eval(lock, "new BigUint64Array([1n, 2n, 3n])").unwrap();
        assert_eq!(result, vec![1, 2, 3]);

        Ok(())
    });
}
