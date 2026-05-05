// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `ArrayBuffer`, `ArrayBufferView`, and `BackingStore`.

use jsg::ToJS;
use jsg::v8;

// =============================================================================
// ArrayBuffer::new / byte_length / is_empty
// =============================================================================

#[test]
fn array_buffer_new_copies_data() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Create from a mutable source, then mutate the source afterwards.
        // The ArrayBuffer must retain the original values (copy semantics).
        let mut src = [10u8, 20, 30, 40];
        let buf = v8::ArrayBuffer::new(lock, &src);

        // Mutate the source after creation — must not affect the ArrayBuffer.
        src.fill(0xFF);

        ctx.set_global("buf", buf.into());

        let is_ab: bool = ctx.eval(lock, "buf instanceof ArrayBuffer").unwrap();
        assert!(is_ab);

        let len: jsg::Number = ctx.eval(lock, "buf.byteLength").unwrap();
        assert!((len.value() - 4.0).abs() < f64::EPSILON);

        // Values must be the originals, not 0xFF.
        let v0: jsg::Number = ctx.eval(lock, "new Uint8Array(buf)[0]").unwrap();
        assert!((v0.value() - 10.0).abs() < f64::EPSILON);

        let v3: jsg::Number = ctx.eval(lock, "new Uint8Array(buf)[3]").unwrap();
        assert!((v3.value() - 40.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn array_buffer_new_zeroed_is_zeroed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 8);
        ctx.set_global("buf", buf.into());

        let len: jsg::Number = ctx.eval(lock, "buf.byteLength").unwrap();
        assert!((len.value() - 8.0).abs() < f64::EPSILON);

        // V8 zero-initializes new backing stores
        let all_zero: bool = ctx
            .eval(lock, "new Uint8Array(buf).every((i) => i === 0)")
            .unwrap();
        assert!(all_zero);

        Ok(())
    });
}

#[test]
fn array_buffer_is_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 0);
        assert!(buf.is_empty());
        assert_eq!(buf.byte_length(), 0);
        Ok(())
    });
}

#[test]
fn array_buffer_byte_length() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3]);
        assert_eq!(buf.byte_length(), 3);
        assert!(!buf.is_empty());
        Ok(())
    });
}

// =============================================================================
// ArrayBuffer as_slice / as_mut_slice / to_vec
// =============================================================================

#[test]
fn array_buffer_as_slice_reads_data() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3, 4]);
        assert_eq!(buf.as_slice(), &[1u8, 2, 3, 4]);
        Ok(())
    });
}

#[test]
fn array_buffer_as_slice_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 0);
        assert_eq!(buf.as_slice(), &[] as &[u8]);
        Ok(())
    });
}

#[test]
fn array_buffer_as_mut_slice_mutations_visible_in_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let mut buf = v8::ArrayBuffer::new(lock, &[0u8, 0, 0]);
        // SAFETY: no other reference into this buffer is live during this call.
        unsafe { buf.as_mut_slice(lock) }.copy_from_slice(&[10, 20, 30]);
        ctx.set_global("buf", buf.into());

        let v1: jsg::Number = ctx.eval(lock, "new Uint8Array(buf)[1]").unwrap();
        assert!((v1.value() - 20.0).abs() < f64::EPSILON);

        Ok(())
    });
}

#[test]
fn array_buffer_to_vec() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[5u8, 6, 7]);
        assert_eq!(buf.to_vec(), vec![5u8, 6, 7]);
        Ok(())
    });
}

// =============================================================================
// ArrayBuffer detach / is_detachable / was_detached / is_shared
// =============================================================================

#[test]
fn array_buffer_detach() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3]);
        assert!(buf.is_detachable());
        assert!(!buf.was_detached());
        assert_eq!(buf.byte_length(), 3);

        buf.detach();
        assert!(buf.was_detached());
        assert_eq!(buf.byte_length(), 0);

        Ok(())
    });
}

#[test]
fn array_buffer_is_shared() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1u8]);
        assert!(!buf.is_shared());
        Ok(())
    });
}

// =============================================================================
// ArrayBuffer cast: Value <-> ArrayBuffer, Object <-> ArrayBuffer
// =============================================================================

#[test]
fn array_buffer_into_value_and_back() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2]);
        let val: v8::Local<v8::Value> = buf.into();
        assert!(val.is_array_buffer());
        assert!(!val.is_array_buffer_view());
        let back: v8::Local<v8::ArrayBuffer> = val.into();
        assert_eq!(back.byte_length(), 2);
        assert_eq!(back.as_slice(), &[1u8, 2]);
        Ok(())
    });
}

#[test]
fn array_buffer_try_as_from_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new ArrayBuffer(4)").unwrap();
        let buf = val
            .try_as::<v8::ArrayBuffer>()
            .expect("should cast to ArrayBuffer");
        assert_eq!(buf.byte_length(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_try_as_fails_for_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([1,2,3])").unwrap();
        let result = val.try_as::<v8::ArrayBuffer>();
        assert!(result.is_none());
        Ok(())
    });
}

#[test]
fn array_buffer_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[42u8]);
        let obj: v8::Local<v8::Object> = buf.into();
        // ArrayBuffer is an Object — verify we can read byteLength as a property
        let bl = obj.get(lock, "byteLength").unwrap();
        let n: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, bl)?;
        assert!((n.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// FromJS for Local<ArrayBuffer>
// =============================================================================

#[test]
fn array_buffer_from_js_rejects_typed_array() {
    use jsg_macros::jsg_method;
    use jsg_macros::jsg_resource;

    #[jsg_resource]
    struct AbResource;
    #[jsg_resource]
    impl AbResource {
        #[jsg_method]
        pub fn byte_len(&self, buf: jsg::v8::Local<jsg::v8::ArrayBuffer>) -> jsg::Number {
            #[expect(clippy::cast_precision_loss)]
            jsg::Number::new(buf.byte_length() as f64)
        }
    }

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(AbResource);
        ctx.set_global("r", res.to_js(lock));

        let ok: jsg::Number = ctx.eval(lock, "r.byteLen(new ArrayBuffer(5))").unwrap();
        assert!((ok.value() - 5.0).abs() < f64::EPSILON);

        let err: Result<jsg::Number, _> = ctx.eval(lock, "r.byteLen(new Uint8Array([1,2,3]))");
        assert!(err.is_err());

        Ok(())
    });
}

// =============================================================================
// BackingStore
// =============================================================================

#[test]
fn backing_store_data_and_byte_length() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[11u8, 22, 33]);
        let bs = buf.backing_store();
        assert_eq!(bs.byte_length(), 3);
        assert!(!bs.is_empty());
        // SAFETY: non-shared backing store; no concurrent access.
        assert_eq!(unsafe { bs.as_slice(lock) }, &[11u8, 22, 33]);
        Ok(())
    });
}

#[test]
fn backing_store_max_byte_length_equals_byte_length_for_fixed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[0u8; 16]);
        let bs = buf.backing_store();
        assert_eq!(bs.max_byte_length(), bs.byte_length());
        Ok(())
    });
}

#[test]
fn backing_store_is_not_resizable_for_regular_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2]);
        let bs = buf.backing_store();
        assert!(!bs.is_resizable_by_user_javascript());
        Ok(())
    });
}

#[test]
fn backing_store_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 0);
        let bs = buf.backing_store();
        assert_eq!(bs.byte_length(), 0);
        assert!(bs.is_empty());
        // SAFETY: non-shared backing store; no concurrent access.
        assert_eq!(unsafe { bs.as_slice(lock) }, &[] as &[u8]);
        Ok(())
    });
}

#[test]
fn backing_store_as_mut_slice_mutations_visible_via_local() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[0u8, 0, 0]);
        let mut bs = buf.backing_store();
        // SAFETY: single-threaded test; no concurrent access to this non-shared backing store.
        unsafe { bs.as_mut_slice(lock) }.copy_from_slice(&[7, 8, 9]);
        // The Local<ArrayBuffer> shares the same backing store
        ctx.set_global("buf", buf.into());
        let v: jsg::Number = ctx.eval(lock, "new Uint8Array(buf)[0]").unwrap();
        assert!((v.value() - 7.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn backing_store_outlives_local() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // BackingStore keeps backing memory alive even after the Local is dropped
        let bs = {
            let buf = v8::ArrayBuffer::new(lock, &[42u8, 43, 44]);
            buf.backing_store()
            // buf (Local<ArrayBuffer>) dropped here
        };
        // The shared_ptr in BackingStore keeps the memory alive
        assert_eq!(bs.byte_length(), 3);
        // SAFETY: single-threaded test; the backing store outlives this borrow.
        // SAFETY: non-shared backing store; no concurrent access.
        assert_eq!(unsafe { bs.as_slice(lock) }, &[42u8, 43, 44]);
        Ok(())
    });
}

// =============================================================================
// ArrayBufferView
// =============================================================================

#[test]
fn array_buffer_view_from_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([10, 20, 30])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val
            .try_as::<v8::ArrayBufferView>()
            .expect("Uint8Array is ArrayBufferView");
        assert_eq!(view.byte_length(), 3);
        assert_eq!(view.byte_offset(), 0);
        assert_eq!(view.as_slice(), &[10u8, 20, 30]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_with_byte_offset() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx
            .eval_raw(
                r"
                    const buf = new ArrayBuffer(12);
                    const all = new Uint8Array(buf);
                    for (let i = 0; i < 12; i++) all[i] = i;
                    new Uint8Array(buf, 4, 4)
                ",
            )
            .unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.byte_offset(), 4);
        assert_eq!(view.byte_length(), 4);
        assert_eq!(view.as_slice(), &[4u8, 5, 6, 7]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_as_mut_slice() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([1, 2, 3])").unwrap();
        let mut view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        // SAFETY: no other reference into this buffer is live during this call.
        unsafe { view.as_mut_slice(lock) }.fill(0xFF);
        ctx.set_global("arr", view.into());
        let v: jsg::Number = ctx.eval(lock, "arr[0]").unwrap();
        assert!((v.value() - 255.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn array_buffer_view_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint16Array([100, 200])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        let backing = view.buffer();
        // Uint16Array([100,200]) → 4 bytes
        assert_eq!(backing.byte_length(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_to_vec() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([7, 8, 9])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.to_vec(), vec![7u8, 8, 9]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_is_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert!(view.is_empty());
        assert_eq!(view.byte_length(), 0);
        Ok(())
    });
}

#[test]
fn array_buffer_view_into_value_and_back() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Int32Array([1, 2, 3])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        let val2: v8::Local<v8::Value> = view.into();
        assert!(val2.is_array_buffer_view());
        Ok(())
    });
}

#[test]
fn array_buffer_view_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([1])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_as::<v8::ArrayBufferView>().unwrap();
        let obj: v8::Local<v8::Object> = view.into();
        let bl = obj.get(lock, "byteLength").unwrap();
        let n: jsg::Number = <jsg::Number as jsg::FromJS>::from_js(lock, bl)?;
        assert!((n.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn array_buffer_view_try_as_fails_for_array_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new ArrayBuffer(4)").unwrap();
        assert!(val.try_as::<v8::ArrayBufferView>().is_none());
        Ok(())
    });
}

#[test]
fn array_buffer_view_from_js_rejects_array_buffer() {
    use jsg_macros::jsg_method;
    use jsg_macros::jsg_resource;

    #[jsg_resource]
    struct ViewResource;
    #[jsg_resource]
    impl ViewResource {
        #[jsg_method]
        pub fn byte_len(&self, view: jsg::v8::Local<jsg::v8::ArrayBufferView>) -> jsg::Number {
            #[expect(clippy::cast_precision_loss)]
            jsg::Number::new(view.byte_length() as f64)
        }
    }

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(ViewResource);
        ctx.set_global("r", res.to_js(lock));

        let ok: jsg::Number = ctx
            .eval(lock, "r.byteLen(new Uint8Array([1,2,3]))")
            .unwrap();
        assert!((ok.value() - 3.0).abs() < f64::EPSILON);

        // Plain ArrayBuffer is not an ArrayBufferView
        let err: Result<jsg::Number, _> = ctx.eval(lock, "r.byteLen(new ArrayBuffer(3))");
        assert!(err.is_err());

        Ok(())
    });
}

// =============================================================================
// ArrayBufferView element_size
// =============================================================================

#[test]
fn array_buffer_view_element_size_uint8() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint8Array([1, 2])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 1);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_uint16() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Uint16Array([1])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 2);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_int32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Int32Array([1])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_float32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Float32Array([1.0])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_float64() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Float64Array([1.0])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 8);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_bigint64() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new BigInt64Array([1n])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 8);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_data_view() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new DataView(new ArrayBuffer(8))").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        assert_eq!(view.element_size(), 0); // DataView has no fixed element size
        Ok(())
    });
}

// =============================================================================
// ArrayBuffer detach effects
// =============================================================================

#[test]
fn array_buffer_detach_zeroes_slices() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3]);
        assert_eq!(buf.as_slice(), &[1, 2, 3]);
        buf.detach();
        assert!(buf.as_slice().is_empty());
        assert!(buf.to_vec().is_empty());
        Ok(())
    });
}

// =============================================================================
// ArrayBufferView to_vec with multi-byte typed arrays
// =============================================================================

#[test]
fn array_buffer_view_to_vec_uint32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        // Uint32Array([1]) is 4 bytes: 0x01 0x00 0x00 0x00 on little-endian
        let val = ctx.eval_raw("new Uint32Array([1])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        let bytes = view.to_vec();
        assert_eq!(bytes.len(), 4);
        // Verify it's the raw bytes (little-endian u32 value 1)
        assert_eq!(
            u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
            1
        );
        Ok(())
    });
}

#[test]
fn array_buffer_view_to_vec_float64() {
    let harness = crate::Harness::new();
    harness.run_in_context(|_lock, ctx| {
        let val = ctx.eval_raw("new Float64Array([1.5])").unwrap();
        let view = val.try_as::<v8::ArrayBufferView>().unwrap();
        let bytes = view.to_vec();
        assert_eq!(bytes.len(), 8);
        let reconstructed = f64::from_le_bytes(bytes.try_into().unwrap());
        assert!((reconstructed - 1.5).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Typed array -> ArrayBufferView casts
// =============================================================================

#[test]
fn typed_arrays_cast_to_array_buffer_view() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // Uint8Array -> ArrayBufferView
        let u8_data: Vec<u8> = vec![1, 2, 3];
        let js = u8_data.to_js(lock);
        let typed: v8::Local<v8::Uint8Array> =
            // SAFETY: js was just created by to_js within the current HandleScope.
            unsafe { v8::Local::from_ffi(lock.isolate(), js.into_ffi()) };
        let view: v8::Local<v8::ArrayBufferView> = typed.into();
        assert_eq!(view.byte_length(), 3);

        // Float64Array -> ArrayBufferView
        let f64_data: Vec<f64> = vec![1.0, 2.0];
        let js2 = f64_data.to_js(lock);
        let typed2: v8::Local<v8::Float64Array> =
            // SAFETY: js2 was just created by to_js within the current HandleScope.
            unsafe { v8::Local::from_ffi(lock.isolate(), js2.into_ffi()) };
        let view2: v8::Local<v8::ArrayBufferView> = typed2.into();
        assert_eq!(view2.byte_length(), 16); // 2 × 8 bytes

        Ok(())
    });
}

#[test]
fn array_buffer_view_back_to_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let data: Vec<u8> = vec![10, 20, 30];
        let js = data.to_js(lock);
        let typed: v8::Local<v8::Uint8Array> =
            // SAFETY: js was just created by to_js within the current HandleScope.
            unsafe { v8::Local::from_ffi(lock.isolate(), js.into_ffi()) };
        let view: v8::Local<v8::ArrayBufferView> = typed.into();
        // Cast back to Uint8Array
        let back: v8::Local<v8::Uint8Array> = view.into();
        assert_eq!(back.as_slice(), &[10u8, 20, 30]);
        Ok(())
    });
}
