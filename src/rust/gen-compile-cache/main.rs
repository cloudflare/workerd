use clap::Parser;
use std::path::Path;
use std::{fs, path::PathBuf};

/// Generate V8 compile caches
#[derive(Parser, Debug)]
struct Args {
    /// Contains `<input_path> <output_path>` lines
    file_list: PathBuf,
}

fn main() {
    let args = Args::parse();

    let file_list = fs::read_to_string(&args.file_list).expect("can't read file list");
    for line in file_list.split("\n").filter(|l| l.len() > 0) {
        let [input_path, output_path] = line
            .split(" ")
            .collect::<Vec<_>>()
            .try_into()
            .expect("incorrect input line");

        let input = fs::read_to_string(input_path).expect("error reading input file");
        let output = ffi::compile(&input_path, &input);

        fs::write(Path::new(output_path), output).expect("error writing output file");
    }
}

#[cxx::bridge(namespace = "workerd::rust::gen_compile_cache")]
mod ffi {
    unsafe extern "C++" {
        include!("rust/gen-compile-cache/cxx-bridge.h");

        fn compile(path: &str, source_code: &str) -> Vec<u8>;
    }
}
