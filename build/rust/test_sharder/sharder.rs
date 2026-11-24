// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Runs one Bazel test shard of a Rust libtest binary. See `wd_rust_test`'s `shard_count` in
//! `build/wd_rust_test.bzl`.
//!
//! The binary is the runfile whose rlocation path is in `WD_SHARDED_RUST_TEST`. The sharder lists
//! its tests, keeps every `TEST_TOTAL_SHARDS`th one starting at `TEST_SHARD_INDEX`, and runs those
//! by exact name. Its own arguments are passed on to the binary, both when listing (so that a
//! filter narrows the list) and when running. Without `TEST_TOTAL_SHARDS` it runs the binary
//! unchanged.

use std::ffi::OsString;
use std::path::Path;
use std::path::PathBuf;
use std::process::Command;
use std::process::ExitCode;

/// The bytes of test names given to one run of the binary. Windows limits a command line to 32767
/// UTF-16 units, so a shard with more names than fit runs the binary once per batch.
const MAX_NAME_BYTES_PER_RUN: usize = 16 * 1024;

/// The tests of shard `index` of `total`, from the output of `--list --format=terse`, which has
/// one `<name>: test` line per test, in a stable order.
fn tests_of_shard(list: &str, index: usize, total: usize) -> Vec<&str> {
    list.lines()
        .filter_map(|line| line.strip_suffix(": test"))
        .enumerate()
        .filter(|(i, _)| i % total == index)
        .map(|(_, name)| name)
        .collect()
}

/// Splits `names` into batches whose names add up to at most `max_bytes` each, except that a
/// name longer than that is a batch by itself.
fn batches<'a>(names: &[&'a str], max_bytes: usize) -> Vec<Vec<&'a str>> {
    let mut batches: Vec<Vec<&str>> = Vec::new();
    let mut bytes = 0;
    for &name in names {
        match batches.last_mut() {
            Some(batch) if bytes + name.len() <= max_bytes => batch.push(name),
            _ => {
                batches.push(vec![name]);
                bytes = 0;
            }
        }
        bytes += name.len();
    }
    batches
}

/// Reads a non-negative integer from the environment variable `name`, if it is set.
fn env_usize(name: &str) -> Result<Option<usize>, String> {
    match std::env::var(name) {
        Ok(value) => value
            .parse()
            .map(Some)
            .map_err(|e| format!("{name}={value}: {e}")),
        Err(std::env::VarError::NotPresent) => Ok(None),
        Err(e) => Err(format!("{name}: {e}")),
    }
}

fn test_binary() -> Result<PathBuf, String> {
    let rlocation =
        std::env::var("WD_SHARDED_RUST_TEST").map_err(|e| format!("WD_SHARDED_RUST_TEST: {e}"))?;
    let runfiles = runfiles::Runfiles::create().map_err(|e| format!("runfiles: {e}"))?;
    // The path is canonical, so it needs no repository mapping.
    runfiles
        .rlocation_from(&rlocation, "")
        .ok_or_else(|| format!("{rlocation} is not a runfile"))
}

/// Runs `binary` with `args` and returns its exit code, or 1 if a signal ended it.
fn run(binary: &Path, args: &[OsString]) -> Result<u8, String> {
    let status = Command::new(binary)
        .args(args)
        .status()
        .map_err(|e| format!("running {}: {e}", binary.display()))?;
    match status.code() {
        Some(0) => Ok(0),
        Some(code) => Ok(u8::try_from(code).unwrap_or(1).max(1)),
        None => {
            eprintln!("{} ended with {status}", binary.display());
            Ok(1)
        }
    }
}

fn run_shard() -> Result<u8, String> {
    let binary = test_binary()?;
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();

    // Tells Bazel that this test shards, which it checks when TEST_TOTAL_SHARDS > 1.
    if let Some(status_file) = std::env::var_os("TEST_SHARD_STATUS_FILE") {
        std::fs::write(&status_file, b"").map_err(|e| format!("TEST_SHARD_STATUS_FILE: {e}"))?;
    }
    let total = env_usize("TEST_TOTAL_SHARDS")?.unwrap_or(1);
    let index = env_usize("TEST_SHARD_INDEX")?.unwrap_or(0);
    if total <= 1 {
        return run(&binary, &args);
    }
    if index >= total {
        return Err(format!(
            "TEST_SHARD_INDEX={index} with TEST_TOTAL_SHARDS={total}"
        ));
    }

    let listing = Command::new(&binary)
        .args(&args)
        .args(["--list", "--format=terse"])
        .output()
        .map_err(|e| format!("listing the tests of {}: {e}", binary.display()))?;
    if !listing.status.success() {
        eprint!("{}", String::from_utf8_lossy(&listing.stderr));
        return Err(format!("listing the tests failed with {}", listing.status));
    }
    let list = String::from_utf8(listing.stdout).map_err(|e| format!("test list: {e}"))?;
    let names = tests_of_shard(&list, index, total);
    // Without a name the binary would run every test.
    if names.is_empty() {
        println!("shard {index} of {total} has no tests");
        return Ok(0);
    }

    let mut result = 0;
    for batch in batches(&names, MAX_NAME_BYTES_PER_RUN) {
        let mut batch_args = args.clone();
        batch_args.push("--exact".into());
        batch_args.extend(batch.into_iter().map(OsString::from));
        let code = run(&binary, &batch_args)?;
        if result == 0 {
            result = code;
        }
    }
    Ok(result)
}

fn main() -> ExitCode {
    match run_shard() {
        Ok(code) => ExitCode::from(code),
        Err(message) => {
            eprintln!("sharder: {message}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
#[path = "sharder-test.rs"]
mod tests;
