// Exposes rust-brotli (the memory-safe Rust implementation of brotli) to C++
// under brotli_rs_-prefixed C symbols matching the brotli C API. The standard
// unprefixed names are owned by the routing layer
// (src/workerd/util/brotli-router.c++), which forwards to these or to the C
// implementation (brotli_c_*) based on the compression-rs autogate.
//
// Allocation uses the Rust global allocator; the custom allocator callbacks
// accepted by the create functions are ignored, mirroring zlib-rs' rust-allocator
// configuration. The crate's ffi-api feature stays off since it would export the
// unprefixed names.
#![allow(non_snake_case)]
// Thin forwarders over the rust-brotli streaming API: each wrapper has exactly the safety
// contract of the brotli C API function it implements.
#![allow(clippy::missing_safety_doc)]
#![allow(clippy::undocumented_unsafe_blocks)]

use core::ffi::c_char;
use core::ffi::c_int;
use core::ffi::c_void;
use core::slice;

use brotli::enc::StandardAlloc;
use brotli::enc::encode::BrotliEncoderDestroyInstance;
use brotli::enc::encode::BrotliEncoderMaxCompressedSize;
use brotli::enc::encode::BrotliEncoderOperation;
use brotli::enc::encode::BrotliEncoderParameter;
use brotli::enc::encode::BrotliEncoderStateStruct;
use brotli_decompressor::BrotliDecoderHasMoreOutput;
use brotli_decompressor::BrotliDecompressStream;
use brotli_decompressor::BrotliResult;
use brotli_decompressor::BrotliState;

type EncoderState = BrotliEncoderStateStruct<StandardAlloc>;
type DecoderState = BrotliState<StandardAlloc, StandardAlloc, StandardAlloc>;

// The custom allocator callback types from the C API; accepted for signature
// compatibility and ignored.
type BrotliAllocFunc = Option<unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void>;
type BrotliFreeFunc = Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>;

// BrotliDecoderResult values.
const BROTLI_DECODER_RESULT_ERROR: c_int = 0;
const BROTLI_DECODER_RESULT_SUCCESS: c_int = 1;
const BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: c_int = 2;
const BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: c_int = 3;

fn decoder_result(result: &BrotliResult) -> c_int {
    match result {
        BrotliResult::ResultSuccess => BROTLI_DECODER_RESULT_SUCCESS,
        BrotliResult::NeedsMoreInput => BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT,
        BrotliResult::NeedsMoreOutput => BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT,
        BrotliResult::ResultFailure => BROTLI_DECODER_RESULT_ERROR,
    }
}

unsafe fn slice_or_nil<'a>(data: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        return &[];
    }
    unsafe { slice::from_raw_parts(data, len) }
}

unsafe fn slice_or_nil_mut<'a>(data: *mut u8, len: usize) -> &'a mut [u8] {
    if len == 0 {
        return &mut [];
    }
    unsafe { slice::from_raw_parts_mut(data, len) }
}

fn encoder_parameter(param: c_int) -> Option<BrotliEncoderParameter> {
    use BrotliEncoderParameter as P;
    Some(match param {
        0 => P::BROTLI_PARAM_MODE,
        1 => P::BROTLI_PARAM_QUALITY,
        2 => P::BROTLI_PARAM_LGWIN,
        3 => P::BROTLI_PARAM_LGBLOCK,
        4 => P::BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING,
        5 => P::BROTLI_PARAM_SIZE_HINT,
        6 => P::BROTLI_PARAM_LARGE_WINDOW,
        _ => return None,
    })
}

fn encoder_operation(op: c_int) -> Option<BrotliEncoderOperation> {
    use BrotliEncoderOperation as Op;
    Some(match op {
        0 => Op::BROTLI_OPERATION_PROCESS,
        1 => Op::BROTLI_OPERATION_FLUSH,
        2 => Op::BROTLI_OPERATION_FINISH,
        3 => Op::BROTLI_OPERATION_EMIT_METADATA,
        _ => return None,
    })
}

// =======================================================================================
// Encoder

#[unsafe(no_mangle)]
pub extern "C" fn brotli_rs_BrotliEncoderCreateInstance(
    _alloc_func: BrotliAllocFunc,
    _free_func: BrotliFreeFunc,
    _opaque: *mut c_void,
) -> *mut EncoderState {
    Box::into_raw(Box::new(BrotliEncoderStateStruct::new(
        StandardAlloc::default(),
    )))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderDestroyInstance(state: *mut EncoderState) {
    if state.is_null() {
        return;
    }
    let mut boxed = unsafe { Box::from_raw(state) };
    BrotliEncoderDestroyInstance(&mut boxed);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderSetParameter(
    state: *mut EncoderState,
    param: c_int,
    value: u32,
) -> c_int {
    let Some(param) = encoder_parameter(param) else {
        return 0;
    };
    unsafe { c_int::from((*state).set_parameter(param, value)) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderCompressStream(
    state: *mut EncoderState,
    op: c_int,
    available_in: *mut usize,
    next_in: *mut *const u8,
    available_out: *mut usize,
    next_out: *mut *mut u8,
    total_out: *mut usize,
) -> c_int {
    let Some(op) = encoder_operation(op) else {
        return 0;
    };
    unsafe {
        let input = slice_or_nil(*next_in, *available_in);
        let output = slice_or_nil_mut(*next_out, *available_out);
        let mut input_offset = 0usize;
        let mut output_offset = 0usize;
        let mut to = Some(0usize);
        let result = (*state).compress_stream(
            op,
            &mut *available_in,
            input,
            &mut input_offset,
            &mut *available_out,
            output,
            &mut output_offset,
            &mut to,
            &mut |_a, _b, _c, _d| (),
        );
        if !total_out.is_null() {
            *total_out = to.unwrap_or(0);
        }
        if input_offset != 0 {
            *next_in = (*next_in).add(input_offset);
        }
        if output_offset != 0 {
            *next_out = (*next_out).add(output_offset);
        }
        c_int::from(result)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderIsFinished(state: *mut EncoderState) -> c_int {
    unsafe { c_int::from((*state).is_finished()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderHasMoreOutput(state: *mut EncoderState) -> c_int {
    unsafe { c_int::from((*state).has_more_output()) }
}

#[unsafe(no_mangle)]
pub extern "C" fn brotli_rs_BrotliEncoderMaxCompressedSize(input_size: usize) -> usize {
    BrotliEncoderMaxCompressedSize(input_size)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliEncoderCompress(
    quality: c_int,
    lgwin: c_int,
    mode: c_int,
    input_size: usize,
    input_buffer: *const u8,
    encoded_size: *mut usize,
    encoded_buffer: *mut u8,
) -> c_int {
    unsafe {
        let mut state = BrotliEncoderStateStruct::new(StandardAlloc::default());
        state.set_parameter(
            BrotliEncoderParameter::BROTLI_PARAM_QUALITY,
            quality.cast_unsigned(),
        );
        state.set_parameter(
            BrotliEncoderParameter::BROTLI_PARAM_LGWIN,
            lgwin.cast_unsigned(),
        );
        state.set_parameter(
            BrotliEncoderParameter::BROTLI_PARAM_MODE,
            mode.cast_unsigned(),
        );
        state.set_parameter(
            BrotliEncoderParameter::BROTLI_PARAM_SIZE_HINT,
            input_size as u32,
        );
        let input = slice_or_nil(input_buffer, input_size);
        let output = slice_or_nil_mut(encoded_buffer, *encoded_size);
        let mut available_in = input_size;
        let mut input_offset = 0usize;
        let mut available_out = *encoded_size;
        let mut output_offset = 0usize;
        let mut to = Some(0usize);
        let result = state.compress_stream(
            BrotliEncoderOperation::BROTLI_OPERATION_FINISH,
            &mut available_in,
            input,
            &mut input_offset,
            &mut available_out,
            output,
            &mut output_offset,
            &mut to,
            &mut |_a, _b, _c, _d| (),
        );
        let ok = result && state.is_finished();
        BrotliEncoderDestroyInstance(&mut state);
        if !ok {
            return 0;
        }
        *encoded_size = output_offset;
        1
    }
}

// =======================================================================================
// Decoder

#[unsafe(no_mangle)]
pub extern "C" fn brotli_rs_BrotliDecoderCreateInstance(
    _alloc_func: BrotliAllocFunc,
    _free_func: BrotliFreeFunc,
    _opaque: *mut c_void,
) -> *mut DecoderState {
    Box::into_raw(Box::new(BrotliState::new(
        StandardAlloc::default(),
        StandardAlloc::default(),
        StandardAlloc::default(),
    )))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderDestroyInstance(state: *mut DecoderState) {
    if state.is_null() {
        return;
    }
    drop(unsafe { Box::from_raw(state) });
}

// The Rust decoder has no equivalent of the C decoder parameters
// (disable-ring-buffer-reallocation and large-window are internal to it); accept
// and ignore them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderSetParameter(
    _state: *mut DecoderState,
    _param: c_int,
    _value: u32,
) -> c_int {
    1
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderDecompressStream(
    state: *mut DecoderState,
    available_in: *mut usize,
    next_in: *mut *const u8,
    available_out: *mut usize,
    next_out: *mut *mut u8,
    total_out: *mut usize,
) -> c_int {
    unsafe {
        let input = slice_or_nil(*next_in, *available_in);
        let output = slice_or_nil_mut(*next_out, *available_out);
        let mut input_offset = 0usize;
        let mut output_offset = 0usize;
        let mut fallback_total_out = 0usize;
        let total_out = if total_out.is_null() {
            &mut fallback_total_out
        } else {
            &mut *total_out
        };
        let result = BrotliDecompressStream(
            &mut *available_in,
            &mut input_offset,
            input,
            &mut *available_out,
            &mut output_offset,
            output,
            total_out,
            &mut *state,
        );
        if input_offset != 0 {
            *next_in = (*next_in).add(input_offset);
        }
        if output_offset != 0 {
            *next_out = (*next_out).add(output_offset);
        }
        decoder_result(&result)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderHasMoreOutput(state: *const DecoderState) -> c_int {
    unsafe { c_int::from(BrotliDecoderHasMoreOutput(&*state)) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderGetErrorCode(state: *const DecoderState) -> c_int {
    unsafe { (*state).error_code as c_int }
}

// Matches the strings produced by the C implementation's BrotliDecoderErrorString
// (see BROTLI_DECODER_ERROR_CODES_LIST in brotli/decode.h).
#[unsafe(no_mangle)]
pub extern "C" fn brotli_rs_BrotliDecoderErrorString(code: c_int) -> *const c_char {
    let name: &'static str = match code {
        0 => "_NO_ERROR\0",
        1 => "_SUCCESS\0",
        2 => "_NEEDS_MORE_INPUT\0",
        3 => "_NEEDS_MORE_OUTPUT\0",
        -1 => "_ERROR_FORMAT_EXUBERANT_NIBBLE\0",
        -2 => "_ERROR_FORMAT_RESERVED\0",
        -3 => "_ERROR_FORMAT_EXUBERANT_META_NIBBLE\0",
        -4 => "_ERROR_FORMAT_SIMPLE_HUFFMAN_ALPHABET\0",
        -5 => "_ERROR_FORMAT_SIMPLE_HUFFMAN_SAME\0",
        -6 => "_ERROR_FORMAT_CL_SPACE\0",
        -7 => "_ERROR_FORMAT_HUFFMAN_SPACE\0",
        -8 => "_ERROR_FORMAT_CONTEXT_MAP_REPEAT\0",
        -9 => "_ERROR_FORMAT_BLOCK_LENGTH_1\0",
        -10 => "_ERROR_FORMAT_BLOCK_LENGTH_2\0",
        -11 => "_ERROR_FORMAT_TRANSFORM\0",
        -12 => "_ERROR_FORMAT_DICTIONARY\0",
        -13 => "_ERROR_FORMAT_WINDOW_BITS\0",
        -14 => "_ERROR_FORMAT_PADDING_1\0",
        -15 => "_ERROR_FORMAT_PADDING_2\0",
        -16 => "_ERROR_FORMAT_DISTANCE\0",
        -18 => "_ERROR_COMPOUND_DICTIONARY\0",
        -19 => "_ERROR_DICTIONARY_NOT_SET\0",
        -20 => "_ERROR_INVALID_ARGUMENTS\0",
        -21 => "_ERROR_ALLOC_CONTEXT_MODES\0",
        -22 => "_ERROR_ALLOC_TREE_GROUPS\0",
        -25 => "_ERROR_ALLOC_CONTEXT_MAP\0",
        -26 => "_ERROR_ALLOC_RING_BUFFER_1\0",
        -27 => "_ERROR_ALLOC_RING_BUFFER_2\0",
        -30 => "_ERROR_ALLOC_BLOCK_TYPE_TREES\0",
        -31 => "_ERROR_UNREACHABLE\0",
        _ => "INVALID\0",
    };
    name.as_ptr().cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn brotli_rs_BrotliDecoderDecompress(
    encoded_size: usize,
    encoded_buffer: *const u8,
    decoded_size: *mut usize,
    decoded_buffer: *mut u8,
) -> c_int {
    unsafe {
        let mut state = BrotliState::new(
            StandardAlloc::default(),
            StandardAlloc::default(),
            StandardAlloc::default(),
        );
        let input = slice_or_nil(encoded_buffer, encoded_size);
        let output = slice_or_nil_mut(decoded_buffer, *decoded_size);
        let mut available_in = encoded_size;
        let mut input_offset = 0usize;
        let mut available_out = *decoded_size;
        let mut output_offset = 0usize;
        let mut total_out = 0usize;
        let result = BrotliDecompressStream(
            &mut available_in,
            &mut input_offset,
            input,
            &mut available_out,
            &mut output_offset,
            output,
            &mut total_out,
            &mut state,
        );
        match result {
            BrotliResult::ResultSuccess => {
                *decoded_size = output_offset;
                BROTLI_DECODER_RESULT_SUCCESS
            }
            _ => BROTLI_DECODER_RESULT_ERROR,
        }
    }
}
