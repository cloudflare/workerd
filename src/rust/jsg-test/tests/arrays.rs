use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
struct Person {
    pub name: String,
    pub age: f64,
}

#[jsg_resource]
struct ArrayResource {
    _state: ResourceState,
}

#[jsg_resource]
impl ArrayResource {
    #[jsg_method]
    pub fn sum(&self, numbers: Vec<f64>) -> f64 {
        numbers.iter().sum()
    }

    #[jsg_method]
    pub fn sum_slice(&self, numbers: &[f64]) -> f64 {
        numbers.iter().sum()
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
    pub fn filter_positive(&self, numbers: Vec<f64>) -> Vec<f64> {
        numbers.into_iter().filter(|&n| n > 0.0).collect()
    }

    #[jsg_method]
    pub fn reverse_bytes(&self, bytes: Vec<u8>) -> Vec<u8> {
        bytes.into_iter().rev().collect()
    }

    #[jsg_method]
    pub fn sum_i32(&self, numbers: Vec<i32>) -> f64 {
        numbers.iter().map(|&n| f64::from(n)).sum()
    }

    #[jsg_method]
    pub fn filter_adults(&self, people: Vec<Person>) -> Vec<Person> {
        people.into_iter().filter(|p| p.age >= 18.0).collect()
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

        let result: f64 = ctx.eval(lock, "arr.sum([1, 2, 3, 4, 5])").unwrap();
        assert!((result - 15.0).abs() < f64::EPSILON);

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

        let result: f64 = ctx.eval(lock, "arr.sumSlice([1, 2, 3, 4, 5])").unwrap();
        assert!((result - 15.0).abs() < f64::EPSILON);

        let result: String = ctx.eval(lock, "arr.joinStrings(['a', 'b', 'c'])").unwrap();
        assert_eq!(result, "a-b-c");

        let result: f64 = ctx.eval(lock, "arr.sumSlice([])").unwrap();
        assert!((result - 0.0).abs() < f64::EPSILON);
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

        let result: Vec<f64> = ctx.eval(lock, "arr.double([1, 2, 3])").unwrap();
        assert_eq!(result, vec![2.0, 4.0, 6.0]);

        let result: Vec<String> = ctx.eval(lock, "arr.splitString('a, b, c')").unwrap();
        assert_eq!(result, vec!["a", "b", "c"]);

        let result: Vec<f64> = ctx
            .eval(lock, "arr.filterPositive([-1, 2, -3, 4, 0])")
            .unwrap();
        assert_eq!(result, vec![2.0, 4.0]);
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

        let result: f64 = ctx
            .eval(lock, "arr.sumI32(new Int32Array([-10, 20, -5]))")
            .unwrap();
        assert!((result - 5.0).abs() < f64::EPSILON);
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
        let length: f64 = ctx.eval(lock, "arr.length").unwrap();
        assert!((length - 2.0).abs() < f64::EPSILON);
        let first: String = ctx.eval(lock, "arr[0]").unwrap();
        assert_eq!(first, "hello");

        let vec = vec![1.5, 2.5, 3.5];
        let js_val = vec.to_js(lock);
        ctx.set_global("nums", js_val);

        let sum: f64 = ctx.eval(lock, "nums[0] + nums[1] + nums[2]").unwrap();
        assert!((sum - 7.5).abs() < f64::EPSILON);

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

        let numbers: Vec<f64> = ctx.eval(lock, "[1, 2, 3]").unwrap();
        assert_eq!(numbers, vec![1.0, 2.0, 3.0]);
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

        let length: f64 = ctx.eval(lock, "arr.length").unwrap();
        assert!(length.abs() < f64::EPSILON);

        let result: Vec<String> = ctx.eval(lock, "arr").unwrap();
        assert!(result.is_empty());
        Ok(())
    });
}

#[test]
fn vec_nested_arrays() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let nested = vec![vec![1.0, 2.0], vec![3.0, 4.0]];
        let js_val = nested.to_js(lock);
        ctx.set_global("matrix", js_val);

        let first_row_sum: f64 = ctx.eval(lock, "matrix[0][0] + matrix[0][1]").unwrap();
        assert!((first_row_sum - 3.0).abs() < f64::EPSILON);

        let second_row_sum: f64 = ctx.eval(lock, "matrix[1][0] + matrix[1][1]").unwrap();
        assert!((second_row_sum - 7.0).abs() < f64::EPSILON);
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
        let val: f64 = ctx.eval(lock, "u8_arr[2]").unwrap();
        assert!((val - 255.0).abs() < f64::EPSILON);

        let vec: Vec<u16> = vec![1, 2, 65535];
        ctx.set_global("u16_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "u16_arr instanceof Uint16Array").unwrap();
        assert!(check);
        let val: f64 = ctx.eval(lock, "u16_arr[2]").unwrap();
        assert!((val - 65535.0).abs() < f64::EPSILON);

        let vec: Vec<u32> = vec![1, 2, 4_294_967_295];
        ctx.set_global("u32_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "u32_arr instanceof Uint32Array").unwrap();
        assert!(check);
        let val: f64 = ctx.eval(lock, "u32_arr[2]").unwrap();
        assert!((val - 4_294_967_295.0).abs() < f64::EPSILON);

        let vec: Vec<i8> = vec![-128, 0, 127];
        ctx.set_global("i8_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i8_arr instanceof Int8Array").unwrap();
        assert!(check);
        let val: f64 = ctx.eval(lock, "i8_arr[0]").unwrap();
        assert!((val - (-128.0)).abs() < f64::EPSILON);

        let vec: Vec<i16> = vec![-32768, 0, 32767];
        ctx.set_global("i16_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i16_arr instanceof Int16Array").unwrap();
        assert!(check);
        let val: f64 = ctx.eval(lock, "i16_arr[0]").unwrap();
        assert!((val - (-32768.0)).abs() < f64::EPSILON);

        let vec: Vec<i32> = vec![-2_147_483_648, 0, 2_147_483_647];
        ctx.set_global("i32_arr", vec.to_js(lock));
        let check: bool = ctx.eval(lock, "i32_arr instanceof Int32Array").unwrap();
        assert!(check);
        let val: f64 = ctx.eval(lock, "i32_arr[0]").unwrap();
        assert!((val - (-2_147_483_648.0)).abs() < f64::EPSILON);

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

        let length: f64 = ctx.eval(lock, "arr.length").unwrap();
        assert!(length.abs() < f64::EPSILON);

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

        let length: f64 = ctx.eval(lock, "arr.length").unwrap();
        assert!((length - 65536.0).abs() < f64::EPSILON);

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
