use std::cell::RefCell;
use std::rc::Rc;

use swc_common::SourceMap;
use swc_common::errors::DiagnosticBuilder;
use swc_common::errors::Emitter;
use swc_common::errors::HANDLER;
use swc_common::errors::Handler;
use swc_common::errors::Level;

use crate::ffi::Output;

#[cxx::bridge(namespace = "workerd::rust::transpiler")]
#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
mod ffi {
    #[derive(Debug)]
    struct Output {
        success: bool,
        // empty when error
        code: String,
        error: String,
        diagnostics: Vec<Message>,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    enum Level {
        Error,
        Warning,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    struct Message {
        level: Level,
        message: String,
    }
    extern "Rust" {

        /// Strip typescript types from the source code.
        /// Other typescript constructs line enum will result in error `Output`.
        fn ts_strip(filename: &str, src: &[u8]) -> Output;
    }
}

fn ts_strip(filename: &str, src: &[u8]) -> Output {
    tr_strip_string(filename, String::from_utf8_lossy(src).to_string())
}

fn tr_strip_string(filename: &str, src: String) -> Output {
    let cm: Rc<SourceMap> = Rc::default();
    let errors = Box::new(MessagesCollector::default());
    let messages = errors.messages.clone();
    let handler = Handler::with_emitter(false, false, errors);

    let output = HANDLER.set(&handler, || {
        swc_ts_fast_strip::operate(
            &cm,
            &handler,
            src,
            swc_ts_fast_strip::Options {
                filename: Some(filename.to_owned()),
                mode: swc_ts_fast_strip::Mode::StripOnly,
                ..Default::default()
            },
        )
    });

    match output {
        Ok(output) => Output {
            success: true,
            code: output.code,
            error: String::new(),
            diagnostics: messages.borrow().clone(),
        },
        Err(err) => Output {
            success: false,
            code: String::new(),
            error: err.message,
            diagnostics: messages.borrow().clone(),
        },
    }
}

/// Collects all swc emitted error message.
#[derive(Default)]
struct MessagesCollector {
    messages: Rc<RefCell<Vec<ffi::Message>>>,
}

impl Emitter for MessagesCollector {
    fn emit(&mut self, db: &mut DiagnosticBuilder<'_>) {
        if db.is_error() {
            self.messages.borrow_mut().push(ffi::Message {
                level: ffi::Level::Error,
                message: db.message(),
            });
        } else if db.level == Level::Warning {
            self.messages.borrow_mut().push(ffi::Message {
                level: ffi::Level::Warning,
                message: db.message(),
            });
        }
    }
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
