# Directory-module fixture

`mod.rs` and `leaf.rs` exercise out-of-line directory modules and nested module
loading. Each module has its own local unsafe-code policy. They are inputs to
[the parent compiler-plugin test](../../README.md), not standalone crates.
