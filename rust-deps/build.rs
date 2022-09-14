// To include another module for cxx code-gen, simply add it to this list
static FILES: &[&str] = &[
    "src/addr2line.rs",
    "src/util.rs"
];

fn main() {
    cxx_build::bridges(FILES) // returns a cc::Build
        .flag_if_supported("-std=c++20")
        .compile("rust-deps");

    FILES.iter().for_each(|file| println!("cargo:rerun-if-changed={}", file));
}
