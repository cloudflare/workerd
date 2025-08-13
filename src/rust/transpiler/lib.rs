use std::cell::RefCell;
use std::rc::Rc;

use swc_core::common::FileName;
use swc_core::common::GLOBALS;
use swc_core::common::Globals;
use swc_core::common::Mark;
use swc_core::common::SourceMap;
use swc_core::common::comments::SingleThreadedComments;
use swc_core::common::errors::DiagnosticBuilder;
use swc_core::common::errors::Emitter;
use swc_core::common::errors::HANDLER;
use swc_core::common::errors::Handler;
use swc_core::common::errors::Level;
use swc_core::common::sync::Lrc;
use swc_core::ecma::ast::Pass;
use swc_core::ecma::ast::Program;
use swc_core::ecma::codegen::to_code_default;
use swc_core::ecma::parser::PResult;
use swc_core::ecma::parser::Parser;
use swc_core::ecma::parser::StringInput;
use swc_core::ecma::parser::Syntax;
use swc_core::ecma::parser::TsSyntax;
use swc_core::ecma::transforms::typescript::strip;

use crate::ffi::Output;
use crate::ffi::TranspileOptions;

#[cxx::bridge(namespace = "workerd::rust::transpiler")]
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

    #[derive(Debug, Clone, PartialEq, Eq, Default)]
    struct TranspileOptions {
        /// Enable transpiling of tsx
        tsx: bool,
    }

    extern "Rust" {

        /// Strip typescript types from the source code.
        /// Other typescript constructs line enum will result in error `Output`.
        fn ts_strip(filename: &str, src: &[u8]) -> Output;

        /// Transpile typescript to javascript.
        // TODO: pass source map information out.
        fn ts_transpile(filename: &str, src: &[u8], options: &TranspileOptions) -> Output;
    }
}

fn ts_strip(filename: &str, src: &[u8]) -> Output {
    ts_strip_string(filename, String::from_utf8_lossy(src).to_string())
}

fn ts_strip_string(filename: &str, src: String) -> Output {
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

fn ts_transpile(filename: &str, src: &[u8], options: &TranspileOptions) -> Output {
    ts_transpile_string(filename, String::from_utf8_lossy(src).to_string(), options)
}

fn ts_transpile_string(filename: &str, src: String, options: &TranspileOptions) -> Output {
    let cm: Lrc<SourceMap> = Lrc::new(SourceMap::default());
    let source_file = cm.new_source_file(Lrc::new(FileName::Custom(filename.into())), src);

    let comments = SingleThreadedComments::default();

    let mut parser = Parser::new(
        Syntax::Typescript(TsSyntax {
            tsx: options.tsx,
            ..Default::default()
        }),
        StringInput::from(&*source_file),
        Some(&comments),
    );

    let globals = Globals::default();
    let code = GLOBALS.set(&globals, || -> PResult<String> {
        let module = parser.parse_module()?;
        let unresolved_mark = Mark::new();
        let top_level_mark = Mark::new();
        let mut program = Program::Module(module);
        strip(unresolved_mark, top_level_mark).process(&mut program);
        Ok(to_code_default(cm, Some(&comments), &program))
    });

    match code {
        Ok(code) => Output {
            success: true,
            code,
            error: String::new(),
            diagnostics: Vec::new(),
        },
        Err(err) => Output {
            success: false,
            code: String::new(),
            error: format!("parse error: {err:?}"),
            diagnostics: Vec::new(),
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
mod tests {
    use crate::ffi::Output;
    use crate::ffi::TranspileOptions;
    use crate::ffi::{self};
    use crate::ts_strip;
    use crate::ts_transpile;

    fn strip(src: &str) -> Output {
        ts_strip("foo.ts", src.as_bytes())
    }

    fn transpile(src: &str) -> Output {
        ts_transpile("foo.ts", src.as_bytes(), &TranspileOptions::default())
    }

    #[test]
    fn strip_js() {
        let out = strip("let x = 42;");
        assert!(out.success);
        assert_eq!("let x = 42;", out.code);
        assert!(out.diagnostics.is_empty());
    }

    #[test]
    fn strip_ts() {
        let out = strip("let x: Number = 42;");
        assert!(out.success);
        assert_eq!("let x         = 42;", out.code);
        assert!(out.diagnostics.is_empty());
    }

    #[test]
    fn strip_worker() {
        assert_eq!(
            r"
export default {
    async fetch(request, env, ctx)                    {
        return new Response('Hello World from Typescript!');
    },
}                               ;",
            strip(
                r"
export default {
    async fetch(request, env, ctx): Promise<Response> {
        return new Response('Hello World from Typescript!');
    },
} satisfies ExportedHandler<Env>;"
            )
            .code
        );
    }

    #[test]
    fn strip_enum() {
        // only types are stripped, unsupported typescript construct are reported as errors
        let out = strip(r"enum Foo { A,B,C }");
        assert!(!out.success);
        assert_eq!("", out.code);
        assert_eq!("Unsupported syntax", out.error);
        assert_eq!(
            vec![ffi::Message {
                level: ffi::Level::Error,
                message: "TypeScript enum is not supported in strip-only mode".to_owned()
            }],
            out.diagnostics
        );
    }

    #[test]
    fn transpile_js() {
        let out = transpile("let x = 42;");
        assert!(out.success);
        assert_eq!("let x = 42;\n", out.code);
        assert!(out.diagnostics.is_empty());
    }

    #[test]
    fn transpile_ts() {
        let out = transpile("let x: Number = 42;");
        assert!(out.success);
        assert_eq!("let x = 42;\n", out.code);
        assert!(out.diagnostics.is_empty());
    }

    #[test]
    fn transpile_worker() {
        assert_eq!(
            r"export default {
    async fetch (request, env, ctx) {
        return new Response('Hello World from Typescript!');
    }
};
",
            transpile(
                r"
export default {
    async fetch(request, env, ctx): Promise<Response> {
        return new Response('Hello World from Typescript!');
    },
} satisfies ExportedHandler<Env>;"
            )
            .code
        );
    }

    #[test]
    fn transpile_enum() {
        let out = transpile("enum Foo { A,B,C }");
        assert!(out.success);
        assert!(out.code.starts_with("let Foo = "));
        assert!(out.diagnostics.is_empty());
    }
}
