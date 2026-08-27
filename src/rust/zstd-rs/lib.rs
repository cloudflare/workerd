// Exposes ruzstd (the memory-safe pure-Rust zstd decoder) to C++ under
// zstd_rs_-prefixed C symbols implementing the ZSTD_decompressStream contract.
// The unprefixed ZSTD_* names are owned by the routing layer
// (src/workerd/util/zstd-router.c++), which forwards decompression to these or
// to the C implementation (zstd_c_*) based on the compression-rs autogate.
// Compression is not implemented here; the router keeps it on the C
// implementation on both sides of the gate.
#![allow(non_snake_case, non_camel_case_types)]
// FFI layer over ruzstd: each entry point has exactly the safety contract of
// the libzstd function it replaces.
#![allow(clippy::missing_safety_doc)]
#![allow(clippy::undocumented_unsafe_blocks)]

use core::ffi::c_int;
use core::ffi::c_void;
use std::io::Read;

use ruzstd::decoding::FrameDecoder;
use ruzstd::decoding::errors::FrameDecoderError;
use ruzstd::decoding::errors::FrameHeaderError;
use ruzstd::decoding::errors::ReadFrameHeaderError;

// Matches the public ZSTD_inBuffer/ZSTD_outBuffer ABI from zstd.h.
#[repr(C)]
pub struct ZSTD_inBuffer {
    pub src: *const c_void,
    pub size: usize,
    pub pos: usize,
}

#[repr(C)]
pub struct ZSTD_outBuffer {
    pub dst: *mut c_void,
    pub size: usize,
    pub pos: usize,
}

// Error results use the standard libzstd encoding, (size_t)-ZSTD_ErrorCode, so
// the routed ZSTD_isError/ZSTD_getErrorCode/ZSTD_getErrorName (always served
// by the C implementation) interpret them identically on both sides of the
// gate.
const ZSTD_ERROR_GENERIC: usize = 1;
const ZSTD_ERROR_PREFIX_UNKNOWN: usize = 10;
const ZSTD_ERROR_FRAME_PARAMETER_UNSUPPORTED: usize = 14;
const ZSTD_ERROR_FRAME_PARAMETER_WINDOW_TOO_LARGE: usize = 16;
const ZSTD_ERROR_CORRUPTION_DETECTED: usize = 20;
const ZSTD_ERROR_CHECKSUM_WRONG: usize = 22;
const ZSTD_ERROR_DICTIONARY_WRONG: usize = 32;
const ZSTD_ERROR_PARAMETER_UNSUPPORTED: usize = 40;
const ZSTD_ERROR_PARAMETER_OUT_OF_BOUND: usize = 42;

const ZSTD_RESET_SESSION_ONLY: c_int = 1;
const ZSTD_RESET_PARAMETERS: c_int = 2;
const ZSTD_RESET_SESSION_AND_PARAMETERS: c_int = 3;

const ZSTD_D_WINDOW_LOG_MAX: c_int = 100;
const ZSTD_WINDOWLOG_MIN: c_int = 10;
const ZSTD_WINDOWLOG_MAX: c_int = 31;
// libzstd's ZSTD_WINDOWLOG_LIMIT_DEFAULT.
const DEFAULT_WINDOW_LOG_MAX: c_int = 27;

// Suggested-more-input hint; callers only distinguish 0 / error / other.
const HINT: usize = 1;

// Largest input slice fed to the decoder per step: max block content (128K)
// plus block and frame header slack. This bounds how much data is decoded
// ahead of the output buffer in a single step.
const INPUT_STEP: usize = 128 * 1024 + 32;

// A frame header is at most magic(4) + descriptor(1) + window(1) + dictID(4)
// + contentSize(8) bytes; a header read that fails with more than this
// buffered is corruption rather than a short read.
const MAX_FRAME_HEADER_SIZE: usize = 18;

const fn err(code: usize) -> usize {
    code.wrapping_neg()
}

struct Dctx {
    decoder: FrameDecoder,
    // Unconsumed input: bytes handed to us are taken eagerly (modulo the
    // hostage byte below) and buffered here until decodable.
    buf: Vec<u8>,
    frame_active: bool,
    // Mirrors libzstd's hostage byte: whenever the output buffer fills before
    // the stream is fully flushed, one already-buffered input byte is reported
    // unconsumed so that callers tracking availIn re-offer it, guaranteeing
    // the call that finally returns 0 sees a non-empty input. The byte is
    // dropped when offered again.
    hostage: bool,
    sticky_error: usize,
    window_log_max: c_int,
}

impl Dctx {
    fn new() -> Self {
        let mut dctx = Self {
            decoder: FrameDecoder::new(),
            buf: Vec::new(),
            frame_active: false,
            hostage: false,
            sticky_error: 0,
            window_log_max: 0,
        };
        dctx.apply_window_log_max();
        dctx
    }

    fn apply_window_log_max(&mut self) {
        let log = if self.window_log_max == 0 {
            DEFAULT_WINDOW_LOG_MAX
        } else {
            self.window_log_max
        };
        self.decoder.set_max_window_size(1u64 << log);
    }

    fn reset_session(&mut self) {
        self.buf.clear();
        self.frame_active = false;
        self.hostage = false;
        self.sticky_error = 0;
        // Drop any half-decoded frame state; the next init() rebuilds it.
        self.decoder = FrameDecoder::new();
        self.apply_window_log_max();
    }

    fn fail(&mut self, code: usize) -> usize {
        self.sticky_error = err(code);
        self.sticky_error
    }

    // Runs header parsing, block decoding, and draining over the buffered
    // input until the output fills or no further progress is possible.
    // Returns (result, bytes written, whether the output filled).
    fn pump(&mut self, out_slice: &mut [u8]) -> (usize, usize, bool) {
        let mut written_total = 0usize;
        let mut out_full = false;

        let result = 'stream: loop {
            if !self.frame_active {
                if self.buf.is_empty() {
                    break 'stream HINT;
                }
                let mut cursor: &[u8] = &self.buf;
                match self.decoder.init(&mut cursor) {
                    Ok(()) => {
                        let consumed = self.decoder.bytes_read_from_source() as usize;
                        self.buf.drain(..consumed);
                        self.frame_active = true;
                    }
                    Err(FrameDecoderError::ReadFrameHeaderError(
                        ReadFrameHeaderError::SkipFrame { length, .. },
                    )) => {
                        let total = 8 + length as usize;
                        if self.buf.len() < total {
                            break 'stream HINT;
                        }
                        self.buf.drain(..total);
                    }
                    Err(e) => match header_error_code(&e, self.buf.len()) {
                        Some(code) => break 'stream self.fail(code),
                        None => break 'stream HINT,
                    },
                }
                continue;
            }

            let step = self.buf.len().min(INPUT_STEP);
            let (read, written) = match self
                .decoder
                .decode_from_to(&self.buf[..step], &mut out_slice[written_total..])
            {
                Ok(progress) => progress,
                Err(e) => break 'stream self.fail(decode_error_code(&e)),
            };
            let (read, written) = if read > step {
                // ruzstd's decode_from_to claims the 4 checksum bytes as read even
                // when fewer are available; nothing was consumed in that case, but
                // finished output can still be drained directly.
                match self.decoder.read(&mut out_slice[written_total..]) {
                    Ok(w) => (0, w),
                    Err(_) => break 'stream self.fail(ZSTD_ERROR_GENERIC),
                }
            } else {
                (read, written)
            };
            self.buf.drain(..read);
            written_total += written;

            if self.decoder.is_finished() && self.decoder.can_collect() == 0 {
                if let (Some(from_data), Some(calculated)) = (
                    self.decoder.get_checksum_from_data(),
                    self.decoder.get_calculated_checksum(),
                ) && from_data != calculated
                {
                    break 'stream self.fail(ZSTD_ERROR_CHECKSUM_WRONG);
                }
                self.frame_active = false;
                if self.buf.is_empty() {
                    break 'stream 0;
                }
                continue;
            }

            if written_total == out_slice.len() && !out_slice.is_empty() {
                out_full = true;
                break 'stream HINT;
            }

            if read == 0 && written == 0 {
                break 'stream HINT;
            }
        };

        (result, written_total, out_full)
    }
}

// None means the header is still incomplete and more input is needed.
fn header_error_code(e: &FrameDecoderError, buffered: usize) -> Option<usize> {
    match e {
        FrameDecoderError::ReadFrameHeaderError(re) => match re {
            ReadFrameHeaderError::BadMagicNumber(_) => Some(ZSTD_ERROR_PREFIX_UNKNOWN),
            ReadFrameHeaderError::InvalidFrameDescriptor(_) => {
                Some(ZSTD_ERROR_FRAME_PARAMETER_UNSUPPORTED)
            }
            _ => (buffered >= MAX_FRAME_HEADER_SIZE).then_some(ZSTD_ERROR_CORRUPTION_DETECTED),
        },
        FrameDecoderError::FrameHeaderError(he) => match he {
            FrameHeaderError::WindowTooBig { .. } => {
                Some(ZSTD_ERROR_FRAME_PARAMETER_WINDOW_TOO_LARGE)
            }
            _ => Some(ZSTD_ERROR_FRAME_PARAMETER_UNSUPPORTED),
        },
        FrameDecoderError::WindowSizeTooBig { .. } => {
            Some(ZSTD_ERROR_FRAME_PARAMETER_WINDOW_TOO_LARGE)
        }
        FrameDecoderError::DictNotProvided { .. } => Some(ZSTD_ERROR_DICTIONARY_WRONG),
        _ => Some(ZSTD_ERROR_GENERIC),
    }
}

fn decode_error_code(e: &FrameDecoderError) -> usize {
    match e {
        FrameDecoderError::FailedToReadBlockHeader(_)
        | FrameDecoderError::FailedToReadBlockBody(_)
        | FrameDecoderError::FailedToReadChecksum(_) => ZSTD_ERROR_CORRUPTION_DETECTED,
        _ => ZSTD_ERROR_GENERIC,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn zstd_rs_ZSTD_createDCtx() -> *mut c_void {
    Box::into_raw(Box::new(Dctx::new())).cast::<c_void>()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zstd_rs_ZSTD_freeDCtx(dctx: *mut c_void) -> usize {
    if !dctx.is_null() {
        drop(unsafe { Box::from_raw(dctx.cast::<Dctx>()) });
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zstd_rs_ZSTD_DCtx_reset(dctx: *mut c_void, directive: c_int) -> usize {
    let Some(dctx) = (unsafe { dctx.cast::<Dctx>().as_mut() }) else {
        return err(ZSTD_ERROR_GENERIC);
    };
    match directive {
        ZSTD_RESET_SESSION_ONLY => dctx.reset_session(),
        ZSTD_RESET_PARAMETERS => {
            dctx.window_log_max = 0;
            dctx.apply_window_log_max();
        }
        ZSTD_RESET_SESSION_AND_PARAMETERS => {
            dctx.reset_session();
            dctx.window_log_max = 0;
            dctx.apply_window_log_max();
        }
        _ => return err(ZSTD_ERROR_PARAMETER_OUT_OF_BOUND),
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zstd_rs_ZSTD_DCtx_setParameter(
    dctx: *mut c_void,
    param: c_int,
    value: c_int,
) -> usize {
    let Some(dctx) = (unsafe { dctx.cast::<Dctx>().as_mut() }) else {
        return err(ZSTD_ERROR_GENERIC);
    };
    if param != ZSTD_D_WINDOW_LOG_MAX {
        return err(ZSTD_ERROR_PARAMETER_UNSUPPORTED);
    }
    if value != 0 && !(ZSTD_WINDOWLOG_MIN..=ZSTD_WINDOWLOG_MAX).contains(&value) {
        return err(ZSTD_ERROR_PARAMETER_OUT_OF_BOUND);
    }
    dctx.window_log_max = value;
    dctx.apply_window_log_max();
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn zstd_rs_ZSTD_decompressStream(
    dctx: *mut c_void,
    output: *mut ZSTD_outBuffer,
    input: *mut ZSTD_inBuffer,
) -> usize {
    let Some(dctx) = (unsafe { dctx.cast::<Dctx>().as_mut() }) else {
        return err(ZSTD_ERROR_GENERIC);
    };
    let (Some(output), Some(input)) = (unsafe { output.as_mut() }, unsafe { input.as_mut() })
    else {
        return err(ZSTD_ERROR_GENERIC);
    };
    if input.pos > input.size || output.pos > output.size {
        return err(ZSTD_ERROR_GENERIC);
    }
    if dctx.sticky_error != 0 {
        return dctx.sticky_error;
    }

    let offered = input.size - input.pos;
    let mut avail_in = unsafe {
        if offered > 0 {
            core::slice::from_raw_parts(input.src.cast::<u8>().add(input.pos), offered)
        } else {
            &[]
        }
    };
    if dctx.hostage && !avail_in.is_empty() {
        // The first offered byte was already consumed when it was taken
        // hostage; it may be re-held below if the output fills again.
        avail_in = &avail_in[1..];
        dctx.hostage = false;
    }
    dctx.buf.extend_from_slice(avail_in);
    input.pos = input.size;

    let out_slice = unsafe {
        if output.size > output.pos {
            core::slice::from_raw_parts_mut(
                output.dst.cast::<u8>().add(output.pos),
                output.size - output.pos,
            )
        } else {
            &mut []
        }
    };
    let (result, written_total, out_full) = dctx.pump(out_slice);

    output.pos += written_total;

    if result == HINT && out_full && offered > 0 {
        // Not fully flushed: hold one byte hostage so the flush-completing
        // call still sees input (see the field comment).
        dctx.hostage = true;
        input.pos = input.size - 1;
    }

    result
}
