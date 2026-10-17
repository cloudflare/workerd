// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Checks a compile test: runs rustc on one snippet and compares the errors it reports with the
//! snippet's `//~` annotations. See `build/wd_rust_compile_test.bzl` for the annotation syntax.
//!
//! Usage: `checker [--expect-mismatch] <snippet.rs> <stamp> -- <rustc> <rustc args...>`
//!
//! On success it writes rustc's output to <stamp> and exits 0. On a mismatch it prints what
//! differed, followed by rustc's output, and exits 1, which fails the build action.
//! `--expect-mismatch` inverts that, for the harness's own tests
//! (`//build/rust/compile_test_harness/tests`): there a mismatch is the success, and the report
//! goes to <stamp>.

use std::fmt::Write as _;
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::process::ExitCode;

/// Where an annotation expects its error.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Location {
    /// On this 1-based line of the snippet.
    Line(usize),
    /// Anywhere, including outside the snippet or with no location at all (`//~?`).
    Anywhere,
}

struct Expected {
    /// The 1-based line of the annotation itself, for messages.
    annotation_line: usize,
    location: Location,
    /// `E0597` and the like, matched exactly against the error's code.
    code: Option<String>,
    /// Matched as a substring of the error's message.
    message: Option<String>,
}

struct Actual {
    code: Option<String>,
    message: String,
    /// (file, 1-based line) of the primary span, if the error has one.
    location: Option<(String, usize)>,
}

fn parse_annotations(src: &str) -> Result<Vec<Expected>, String> {
    let mut expected = Vec::new();
    let mut previous_location: Option<Location> = None;
    for (index, line) in src.lines().enumerate() {
        let line_number = index + 1;
        let Some(start) = line.find("//~") else {
            continue;
        };
        let rest = &line[start + 3..];
        let (location, rest) = if let Some(rest) = rest.strip_prefix('?') {
            (Location::Anywhere, rest)
        } else if let Some(rest) = rest.strip_prefix('|') {
            let Some(location) = previous_location else {
                return Err(format!(
                    "line {line_number}: `//~|` must follow another `//~` annotation"
                ));
            };
            (location, rest)
        } else {
            let carets = rest.chars().take_while(|c| *c == '^').count();
            if carets >= line_number {
                return Err(format!(
                    "line {line_number}: `//~{}` points before the first line",
                    "^".repeat(carets)
                ));
            }
            (Location::Line(line_number - carets), &rest[carets..])
        };
        let Some(pattern) = rest.trim_start().strip_prefix("ERROR") else {
            return Err(format!(
                "line {line_number}: expected `ERROR` after `//~` (only errors can be annotated)"
            ));
        };
        let pattern = pattern.trim();
        let (code, message) = match pattern.split_once(|c: char| c.is_whitespace() || c == ':') {
            Some((first, rest)) if is_error_code(first) => {
                (Some(first.to_owned()), rest.trim_start_matches(':').trim())
            }
            None if is_error_code(pattern) => (Some(pattern.to_owned()), ""),
            _ => (None, pattern),
        };
        if code.is_none() && message.is_empty() {
            return Err(format!(
                "line {line_number}: `ERROR` needs an error code (E0597) or message text"
            ));
        }
        expected.push(Expected {
            annotation_line: line_number,
            location,
            code,
            message: (!message.is_empty()).then(|| message.to_owned()),
        });
        previous_location = Some(location);
    }
    Ok(expected)
}

fn is_error_code(s: &str) -> bool {
    s.len() == 5 && s.starts_with('E') && s[1..].bytes().all(|b| b.is_ascii_digit())
}

/// Extracts the errors from rustc's human-readable output: each `error[E1234]: message` header,
/// with the ` --> file:line:col` line that follows it when the error has a primary span.
fn parse_errors(stderr: &str) -> Vec<Actual> {
    let lines: Vec<&str> = stderr.lines().collect();
    let mut errors = Vec::new();
    for (index, line) in lines.iter().enumerate() {
        let Some(rest) = line.strip_prefix("error") else {
            continue;
        };
        let (code, message) = if let Some(rest) = rest.strip_prefix('[') {
            let Some((code, message)) = rest.split_once("]: ") else {
                continue;
            };
            (Some(code.to_owned()), message)
        } else if let Some(message) = rest.strip_prefix(": ") {
            (None, message)
        } else {
            continue;
        };
        let location = lines.get(index + 1).and_then(|next| {
            let (file, position) = next.trim_start().strip_prefix("--> ")?.rsplit_once(':')?;
            position.parse::<usize>().ok()?;
            let (file, line) = file.rsplit_once(':')?;
            Some((file.to_owned(), line.parse().ok()?))
        });
        // The summary rustc prints after the errors themselves, which has no location.
        if code.is_none() && location.is_none() && is_abort_summary(message) {
            continue;
        }
        errors.push(Actual {
            code,
            message: message.to_owned(),
            location,
        });
    }
    errors
}

fn matches(expected: &Expected, actual: &Actual, snippet: &str) -> bool {
    if let Location::Line(line) = expected.location {
        match &actual.location {
            Some((file, actual_line)) if file == snippet && *actual_line == line => {}
            _ => return false,
        }
    }
    if let Some(code) = &expected.code
        && actual.code.as_deref() != Some(code.as_str())
    {
        return false;
    }
    if let Some(message) = &expected.message
        && !actual.message.contains(message.as_str())
    {
        return false;
    }
    true
}

/// Pairs expectations with the errors they match, each error with at most one expectation,
/// pairing as many as possible: one expectation may match several errors, so a greedy choice can
/// take the only error another expectation matches. Returns, for each error, the index of its
/// expectation.
fn assign(expected: &[Expected], actual: &[Actual], snippet: &str) -> Vec<Option<usize>> {
    // Finds an error for expectation `e`, moving other expectations to other errors if needed.
    fn augment(
        e: usize,
        candidates: &[Vec<usize>],
        owner: &mut [Option<usize>],
        visited: &mut [bool],
    ) -> bool {
        for &a in &candidates[e] {
            if visited[a] {
                continue;
            }
            visited[a] = true;
            if owner[a].is_none_or(|other| augment(other, candidates, owner, visited)) {
                owner[a] = Some(e);
                return true;
            }
        }
        false
    }

    let candidates: Vec<Vec<usize>> = expected
        .iter()
        .map(|expectation| {
            (0..actual.len())
                .filter(|&a| matches(expectation, &actual[a], snippet))
                .collect()
        })
        .collect();
    let mut owner = vec![None; actual.len()];
    for e in 0..expected.len() {
        augment(e, &candidates, &mut owner, &mut vec![false; actual.len()]);
    }
    owner
}

/// Whether `message` reads as rustc's closing summary, "aborting due to N previous errors" or
/// "aborting due to 1 previous error", optionally followed by "; N warning(s) emitted".
fn is_abort_summary(message: &str) -> bool {
    let Some(rest) = message.strip_prefix("aborting due to ") else {
        return false;
    };
    let (errors, warnings) = rest.split_once("; ").unwrap_or((rest, ""));
    let count_of = |text: &str, singular: &str, plural: &str| {
        text.split_once(' ').is_some_and(|(count, noun)| {
            count.parse::<usize>().is_ok() && (noun == singular || noun == plural)
        })
    };
    (errors == "previous error" || count_of(errors, "previous error", "previous errors"))
        && (warnings.is_empty() || count_of(warnings, "warning emitted", "warnings emitted"))
}

fn describe_expected(expected: &Expected) -> String {
    let at = match expected.location {
        Location::Line(line) => format!("on line {line}"),
        Location::Anywhere => "anywhere".to_owned(),
    };
    let what = match (&expected.code, &expected.message) {
        (Some(code), Some(message)) => format!("error[{code}] containing {message:?}"),
        (Some(code), None) => format!("error[{code}]"),
        (None, Some(message)) => format!("error containing {message:?}"),
        (None, None) => "error".to_owned(),
    };
    format!(
        "{what} {at} (annotation on line {})",
        expected.annotation_line
    )
}

fn describe_actual(actual: &Actual) -> String {
    let code = actual
        .code
        .as_ref()
        .map_or_else(String::new, |code| format!("[{code}]"));
    let at = actual
        .location
        .as_ref()
        .map_or_else(String::new, |(file, line)| format!(" at {file}:{line}"));
    format!("error{code}: {}{at}", actual.message)
}

/// Lists, one per line, where rustc's errors disagree with the snippet's annotations; `compiled`
/// is whether rustc succeeded. Empty when they agree.
fn find_problems(
    expected: &[Expected],
    actual: &[Actual],
    compiled: bool,
    snippet: &str,
) -> String {
    let mut problems = String::new();
    if expected.is_empty() {
        if !compiled {
            let _ = writeln!(
                problems,
                "{snippet} has no `//~ ERROR` annotations, so it must compile, but rustc failed."
            );
        }
    } else {
        if compiled {
            let _ = writeln!(
                problems,
                "{snippet} compiled, but its annotations expect {} error(s).",
                expected.len()
            );
        }
        let owner = assign(expected, actual, snippet);
        for (index, expectation) in expected.iter().enumerate() {
            if !owner.contains(&Some(index)) {
                let _ = writeln!(problems, "missing: {}", describe_expected(expectation));
            }
        }
        for (error, _) in actual
            .iter()
            .zip(&owner)
            .filter(|(_, owner)| owner.is_none())
        {
            let _ = writeln!(problems, "unexpected: {}", describe_actual(error));
        }
    }
    problems
}

/// Decides a compile test from its problems: `Ok` with notes for the stamp, or `Err` with why it
/// fails. A snippet marked as a mismatch is a self-test of this checker whose annotations are
/// deliberately wrong, so it fails when there are no problems.
fn verdict(problems: String, expect_mismatch: bool) -> Result<String, String> {
    match (expect_mismatch, problems.is_empty()) {
        (false, true) | (true, false) => Ok(problems),
        (false, false) => Err(problems),
        (true, true) => {
            Err("it is marked as a mismatch, but rustc's errors match its annotations.\n".into())
        }
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (ours, rustc_command) = match args.iter().position(|arg| arg == "--") {
        Some(separator) => (&args[..separator], &args[separator + 1..]),
        None => (&args[..], &[][..]),
    };
    let (expect_mismatch, ours) = match ours {
        [flag, rest @ ..] if flag == "--expect-mismatch" => (true, rest),
        _ => (false, ours),
    };
    let ([snippet, stamp], [rustc, rustc_args @ ..]) = (ours, rustc_command) else {
        return Err(
            "usage: checker [--expect-mismatch] <snippet.rs> <stamp> -- \
                    <rustc> <args...>"
                .into(),
        );
    };

    let source =
        fs::read_to_string(snippet).map_err(|error| format!("cannot read {snippet}: {error}"))?;
    let expected = parse_annotations(&source).map_err(|error| format!("{snippet}: {error}"))?;

    // rustc writes the crate's metadata here; only the diagnostics matter.
    let out_dir = PathBuf::from(format!("{stamp}.out"));
    fs::create_dir_all(&out_dir)
        .map_err(|error| format!("cannot create {}: {error}", out_dir.display()))?;
    let output = Command::new(rustc)
        .args(rustc_args)
        .arg("--out-dir")
        .arg(&out_dir)
        .output()
        .map_err(|error| format!("cannot run {rustc}: {error}"))?;
    let _ = fs::remove_dir_all(&out_dir);
    let stderr = String::from_utf8_lossy(&output.stderr);
    // rustc also exits non-zero when it crashes; that is never the expected failure, not even for
    // a snippet marked as a mismatch.
    if !output.status.success() && output.status.code() != Some(1) {
        return Err(format!(
            "compile test {snippet} FAILED:\nrustc exited abnormally ({}).\n\n--- rustc output \
             ---\n{stderr}",
            output.status
        ));
    }
    let actual = parse_errors(&stderr);

    let problems = find_problems(&expected, &actual, output.status.success(), snippet);
    match verdict(problems, expect_mismatch) {
        Ok(notes) => fs::write(stamp, format!("{notes}\n--- rustc output ---\n{stderr}"))
            .map_err(|error| format!("cannot write {stamp}: {error}")),
        Err(failure) => Err(format!(
            "compile test {snippet} FAILED:\n{failure}\n--- rustc output ---\n{stderr}"
        )),
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("{message}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
#[path = "checker-test.rs"]
mod tests;
