use std::fs;
use std::path::Path;
use std::path::PathBuf;

use clap::Parser;
use clap::ValueEnum;

/// Generate V8 compile caches
#[derive(Parser, Debug)]
struct Args {
    /// Contains `<input_path> <output_path>` lines
    file_list: PathBuf,

    /// How the runtime will compile these sources. Must match the consumer.
    #[arg(long, value_enum, default_value_t = Kind::Module)]
    kind: Kind,

    /// Extra V8 flags to start V8 with (repeatable). Also must match.
    #[arg(long = "v8-flag")]
    v8_flags: Vec<String>,

    /// Compile inner functions eagerly so their bytecode is part of the cache too.
    #[arg(long, default_value_t = false)]
    eager: bool,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum Kind {
    /// `v8::ScriptCompiler::CompileModule` (ESM bundles)
    Module,
    /// `v8::ScriptCompiler::CompileFunction` with one context extension
    /// (per-isolate bootstrap scripts)
    Function,
}

fn main() {
    let args = Args::parse();

    let file_list = fs::read_to_string(&args.file_list).expect("can't read file list");
    for line in file_list.split('\n').filter(|l| !l.is_empty()) {
        let [input_path, output_path] = line
            .split(' ')
            .collect::<Vec<_>>()
            .try_into()
            .expect("incorrect input line");

        let input = fs::read_to_string(input_path).expect("error reading input file");
        let output = ffi::compile(
            input_path,
            &input,
            &args.v8_flags,
            args.kind == Kind::Function,
            args.eager,
        );

        fs::write(Path::new(output_path), output).expect("error writing output file");
    }
}

#[cxx::bridge(namespace = "workerd::rust::gen_compile_cache")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/gen-compile-cache/cxx-bridge.h");

        /// `v8_flags` only take effect on the first call.
        fn compile(
            path: &str,
            source_code: &str,
            v8_flags: &[String],
            as_function: bool,
            eager: bool,
        ) -> Vec<u8>;
    }
}
