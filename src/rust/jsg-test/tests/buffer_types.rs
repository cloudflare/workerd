// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `ArrayBuffer`, `ArrayBufferView`, and `BackingStore`.

use jsg::ToJsTypedArray;
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

        ctx.set_global(lock, "buf", buf);

        let is_ab: bool = ctx.eval(lock, "buf instanceof ArrayBuffer").unwrap();
        assert!(is_ab);

        assert_eq!(ctx.eval::<u32>(lock, "buf.byteLength").unwrap(), 4);

        // Values must be the originals, not 0xFF.
        assert_eq!(ctx.eval::<u8>(lock, "new Uint8Array(buf)[0]").unwrap(), 10);
        assert_eq!(ctx.eval::<u8>(lock, "new Uint8Array(buf)[3]").unwrap(), 40);

        Ok(())
    });
}

#[test]
fn array_buffer_new_zeroed_is_zeroed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 8);
        ctx.set_global(lock, "buf", buf);

        assert_eq!(ctx.eval::<u32>(lock, "buf.byteLength").unwrap(), 8);

        // V8 zero-initializes new backing stores
        let all_zero: bool = ctx
            .eval(lock, "new Uint8Array(buf).every((i) => i === 0)")
            .unwrap();
        assert!(all_zero);

        Ok(())
    });
}

#[test]
fn array_buffer_new_zeroed_with_fills_before_js_sees_it() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let (buf, result) = v8::ArrayBuffer::new_zeroed_with(lock, 4, |bytes| {
            assert_eq!(bytes, &[0, 0, 0, 0], "must be zeroed before fill runs");
            bytes[1..3].copy_from_slice(&[10, 20]);
            "done"
        })
        .unwrap();
        assert_eq!(result, "done");
        assert_eq!(buf.as_slice(lock), &[0, 10, 20, 0]);

        // The written bytes are visible to JavaScript and the rest stay zero.
        ctx.set_global(lock, "buf", buf);
        let contents: String = ctx.eval(lock, "new Uint8Array(buf).join(',')").unwrap();
        assert_eq!(contents, "0,10,20,0");

        Ok(())
    });
}

#[test]
fn array_buffer_new_zeroed_with_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let (buf, len) = v8::ArrayBuffer::new_zeroed_with(lock, 0, |bytes| bytes.len()).unwrap();
        assert_eq!(len, 0);
        assert!(buf.is_empty());
        Ok(())
    });
}

#[test]
fn array_buffer_new_zeroed_with_allocation_failure_skips_fill() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let mut called = false;
        let result = v8::ArrayBuffer::new_zeroed_with(lock, usize::MAX, |_| called = true);
        assert!(result.is_none());
        assert!(!called);
        Ok(())
    });
}

#[test]
fn uint8_array_from_buffer_views_a_range() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1, 2, 3, 4]);
        let view = v8::Uint8Array::from_buffer(lock, &buf, 1, 2).unwrap();
        assert_eq!(view.as_slice(lock), &[2, 3]);
        let whole = v8::Uint8Array::from_buffer(lock, &buf, 0, 4).unwrap();
        assert_eq!(whole.as_slice(lock), &[1, 2, 3, 4]);
        let empty_at_end = v8::Uint8Array::from_buffer(lock, &buf, 4, 0).unwrap();
        assert!(empty_at_end.is_empty());
        Ok(())
    });
}

#[test]
fn uint8_array_from_buffer_out_of_bounds_is_an_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new(lock, &[1, 2, 3, 4]);
        assert!(v8::Uint8Array::from_buffer(lock, &buf, 0, 5).is_err());
        assert!(v8::Uint8Array::from_buffer(lock, &buf, 5, 0).is_err());
        assert!(v8::Uint8Array::from_buffer(lock, &buf, 3, 2).is_err());
        assert!(v8::Uint8Array::from_buffer(lock, &buf, 1, usize::MAX).is_err());
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
        assert_eq!(buf.as_slice(lock), &[1u8, 2, 3, 4]);
        Ok(())
    });
}

#[test]
fn array_buffer_as_slice_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let buf = v8::ArrayBuffer::new_zeroed(lock, 0);
        assert_eq!(buf.as_slice(lock), &[] as &[u8]);
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
        ctx.set_global(lock, "buf", buf);

        assert_eq!(ctx.eval::<u8>(lock, "new Uint8Array(buf)[1]").unwrap(), 20);

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
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3]);
        assert!(buf.is_detachable());
        assert!(!buf.was_detached());
        assert_eq!(buf.byte_length(), 3);

        buf.detach(lock);
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
        let back: v8::Local<v8::ArrayBuffer> = val.try_into().unwrap();
        assert_eq!(back.byte_length(), 2);
        assert_eq!(back.as_slice(lock), &[1u8, 2]);
        Ok(())
    });
}

#[test]
fn array_buffer_try_from_from_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new ArrayBuffer(4)").unwrap();
        let buf: v8::Local<v8::ArrayBuffer> = val.try_into().expect("should cast to ArrayBuffer");
        assert_eq!(buf.byte_length(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_try_from_fails_for_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([1,2,3])").unwrap();
        let result = v8::Local::<v8::ArrayBuffer>::try_from(val);
        assert!(result.is_err());
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
        assert_eq!(<u32 as jsg::FromJS>::from_js(lock, bl)?, 1);
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
        pub fn byte_len(&self, buf: jsg::v8::Local<jsg::v8::ArrayBuffer>) -> u32 {
            u32::try_from(buf.byte_length()).unwrap()
        }
    }

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(AbResource);
        ctx.set_global(lock, "r", res);

        assert_eq!(
            ctx.eval::<u32>(lock, "r.byteLen(new ArrayBuffer(5))")
                .unwrap(),
            5
        );

        let err: Result<u32, _> = ctx.eval(lock, "r.byteLen(new Uint8Array([1,2,3]))");
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
        ctx.set_global(lock, "buf", buf);
        assert_eq!(ctx.eval::<u8>(lock, "new Uint8Array(buf)[0]").unwrap(), 7);
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
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([10, 20, 30])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> =
            val.try_into().expect("Uint8Array is ArrayBufferView");
        assert_eq!(view.byte_length(), 3);
        assert_eq!(view.byte_offset(), 0);
        assert_eq!(view.as_slice(lock), &[10u8, 20, 30]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_with_byte_offset() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx
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
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        assert_eq!(view.byte_offset(), 4);
        assert_eq!(view.byte_length(), 4);
        assert_eq!(view.as_slice(lock), &[4u8, 5, 6, 7]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_as_mut_slice() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([1, 2, 3])").unwrap();
        let mut view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        // SAFETY: no other reference into this buffer is live during this call.
        unsafe { view.as_mut_slice(lock) }.fill(0xFF);
        ctx.set_global(lock, "arr", view);
        assert_eq!(ctx.eval::<u8>(lock, "arr[0]").unwrap(), 255);
        Ok(())
    });
}

#[test]
fn array_buffer_view_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint16Array([100, 200])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        let backing = view.buffer(lock);
        // Uint16Array([100,200]) → 4 bytes
        assert_eq!(backing.byte_length(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_to_vec() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([7, 8, 9])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        assert_eq!(view.to_vec(), vec![7u8, 8, 9]);
        Ok(())
    });
}

#[test]
fn array_buffer_view_is_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        assert!(view.is_empty());
        assert_eq!(view.byte_length(), 0);
        Ok(())
    });
}

#[test]
fn array_buffer_view_into_value_and_back() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Int32Array([1, 2, 3])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        let val2: v8::Local<v8::Value> = view.into();
        assert!(val2.is_array_buffer_view());
        Ok(())
    });
}

#[test]
fn array_buffer_view_into_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([1])").unwrap();
        let view: v8::Local<v8::ArrayBufferView> = val.try_into().unwrap();
        let obj: v8::Local<v8::Object> = view.into();
        let bl = obj.get(lock, "byteLength").unwrap();
        assert_eq!(<u32 as jsg::FromJS>::from_js(lock, bl)?, 1);
        Ok(())
    });
}

#[test]
fn array_buffer_view_try_from_fails_for_array_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new ArrayBuffer(4)").unwrap();
        assert!(v8::Local::<v8::ArrayBufferView>::try_from(val).is_err());
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
        pub fn byte_len(&self, view: jsg::v8::Local<jsg::v8::ArrayBufferView>) -> u32 {
            u32::try_from(view.byte_length()).unwrap()
        }
    }

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let res = jsg::Rc::new(ViewResource);
        ctx.set_global(lock, "r", res);

        assert_eq!(
            ctx.eval::<u32>(lock, "r.byteLen(new Uint8Array([1,2,3]))")
                .unwrap(),
            3
        );

        // Plain ArrayBuffer is not an ArrayBufferView
        let err: Result<u32, _> = ctx.eval(lock, "r.byteLen(new ArrayBuffer(3))");
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
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint8Array([1, 2])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 1);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_uint16() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Uint16Array([1])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 2);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_int32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Int32Array([1])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_float32() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Float32Array([1.0])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 4);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_float64() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Float64Array([1.0])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 8);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_bigint64() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new BigInt64Array([1n])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
        assert_eq!(view.element_size(), 8);
        Ok(())
    });
}

#[test]
fn array_buffer_view_element_size_data_view() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx
            .eval_raw(lock, "new DataView(new ArrayBuffer(8))")
            .unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
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
        let buf = v8::ArrayBuffer::new(lock, &[1u8, 2, 3]);
        assert_eq!(buf.as_slice(lock), &[1, 2, 3]);
        buf.detach(lock);
        assert!(buf.as_slice(lock).is_empty());
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
    harness.run_in_context(|lock, ctx| {
        // Uint32Array([1]) is 4 bytes: 0x01 0x00 0x00 0x00 on little-endian
        let val = ctx.eval_raw(lock, "new Uint32Array([1])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
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
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw(lock, "new Float64Array([1.5])").unwrap();
        let view = v8::Local::<v8::ArrayBufferView>::try_from(val).unwrap();
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
        let typed = [1u8, 2, 3].to_js_typed_array(lock);
        let view: v8::Local<v8::ArrayBufferView> = typed.into();
        assert_eq!(view.byte_length(), 3);

        // Float64Array -> ArrayBufferView
        let typed2 = [1.0f64, 2.0].to_js_typed_array(lock);
        let view2: v8::Local<v8::ArrayBufferView> = typed2.into();
        assert_eq!(view2.byte_length(), 16); // 2 × 8 bytes

        Ok(())
    });
}

#[test]
fn array_buffer_view_back_to_typed_array() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let typed = [10u8, 20, 30].to_js_typed_array(lock);
        let view: v8::Local<v8::ArrayBufferView> = typed.into();
        // Cast back to Uint8Array
        let back: v8::Local<v8::Uint8Array> = view.try_into().unwrap();
        assert_eq!(back.as_slice(lock), &[10u8, 20, 30]);
        Ok(())
    });
}
