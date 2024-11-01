use std::collections::HashSet;

use ruff_python_ast::{Stmt, StmtImportFrom};
use ruff_python_parser::parse_module;

#[cxx::bridge(namespace = "edgeworker::rust::python_parser")]
mod ffi {

    extern "Rust" {
        fn get_imports(sources: &[&str]) -> Vec<String>;
    }
}

#[must_use]
pub fn get_imports(sources: &[&str]) -> Vec<String> {
    let mut names: HashSet<String> = HashSet::new();
    for src in sources {
        // Just skip it if it doesn't parse.
        let Ok(module) = parse_module(src) else {
            continue;
        };
        for stmt in &module.syntax().body {
            match stmt {
                Stmt::Import(s) => {
                    names.extend(s.names.iter().map(|x| x.name.id.as_str().to_owned()));
                }
                Stmt::ImportFrom(StmtImportFrom {
                    module: Some(module),
                    level: 0,
                    ..
                }) => {
                    names.insert(module.id.as_str().to_owned());
                }
                _ => {}
            }
        }
    }
    let mut result: Vec<_> = names.drain().collect();
    result.sort();
    result
}
