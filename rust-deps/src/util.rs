use std::fmt;
use std::io::Write;
use backtrace::Backtrace;

#[cxx::bridge(namespace = "workerd::rust")]
mod ffi {
    extern "Rust" {
        fn set_panic_hook();
    }
}

fn set_panic_hook() {
    std::panic::set_hook(Box::new(|panic_info| {
        let (file, line) = if let Some(location) = panic_info.location() {
            (location.file(), location.line())
        } else {
            (file!(), line!())
        };
        let tmp;
        let msg = if let Some(payload) = panic_info.payload().downcast_ref::<&str>() {
            payload
        } else if let Some(payload) = panic_info.payload().downcast_ref::<String>() {
            tmp = payload;
            &tmp
        } else {
            // not much we can do ...
            "unknown panic payload"
        };

        // don't use `eprintln`, it can panic if writing fails
        drop(writeln!(
            std::io::stderr(),
            "{version} error {file}:{line} {msg}|stack: {stack:?}",
            version = env!("WORKERD_VERSION"),
            file = file,
            line = line,
            // Sentry doesn't understand logging that covers more than a single line.
            msg = msg.replace('\n', "|"),
            // Trying to collect symbols immediately hits the sandbox block when reading /proc/self/exe
            stack = DisplayBacktrace(Backtrace::new_unresolved()),
        ));
    }))
}

/// A wrapper around Backtrace that doesn't call getcwd, which is blocked by the sandbox.
struct DisplayBacktrace(Backtrace);

impl fmt::Debug for DisplayBacktrace {
    fn fmt(&self, fmt: &mut fmt::Formatter) -> fmt::Result {
        use std::ffi::CStr;
        use std::ptr::{null, null_mut};

        for frame in self.0.frames() {
            let addr = frame.ip();
            if addr != null_mut() {
                // For some reason, BacktraceFrame doesn't expose the name of the module.
                // Get it from libc instead.
                // TODO(cleanup): Upstream this to backtrace-rs. (They'd accept it, see https://github.com/rust-lang/backtrace-rs/issues/366.)
                let mut info = libc::Dl_info {
                    dli_fname: null(),
                    dli_fbase: null_mut(),
                    dli_sname: null(),
                    dli_saddr: null_mut(),
                };
                let status = unsafe { libc::dladdr(addr, &mut info) };
                if status != 0 && !info.dli_fname.is_null() {
                    let fname = unsafe { CStr::from_ptr(info.dli_fname) }.to_string_lossy();
                    let relative_addr = addr as usize - info.dli_fbase as usize;
                    write!(fmt, "{}@0x{:x} ", fname, relative_addr)?;
                } else {
                    write!(fmt, "0x{:x} ", addr as usize)?;
                }
            }
        }
        Ok(())
    }
}
