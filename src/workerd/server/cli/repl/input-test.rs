// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// Whether `input`, scanned a line at a time as the console does, continues on another line.
fn unfinished(input: &str) -> bool {
    let mut scanner = Scanner::default();
    for line in input.split('\n') {
        scanner.scan(&format!("{line}\n"));
    }
    scanner.is_unfinished()
}

#[test]
fn finished_inputs() {
    for input in [
        "",
        "1 + 2",
        "foo()",
        "const x = { a: 1 }",
        "'unterminated string",
        "\"a ( b\"",
        "`template ${1 + 2} done`",
        "// comment (",
        "/* ( */ 1",
        "f(\n  1,\n  2\n)",
        "})",
    ] {
        assert!(!unfinished(input), "{input:?}");
    }
}

#[test]
fn unfinished_inputs() {
    for input in [
        "function f() {",
        "[1, 2,",
        "f(",
        "`multi\nline",
        "`a ${ {",
        "`a ${ b",
        "/* comment",
        "'continued \\",
    ] {
        assert!(unfinished(input), "{input:?}");
    }
}

#[test]
fn regular_expression_literals() {
    // Brackets, quotes and comment openers in a regular expression are not code.
    for input in [
        r"const re = /\{/",
        r"x.replace(/[(]/g, '')",
        "/[/]/.test(s)",
        "x = /[\"(]/.test(y)",
        "return /\\//",
    ] {
        assert!(!unfinished(input), "{input:?}");
    }
    // After a value, `/` divides.
    assert!(!unfinished("a / b / c"));
    assert!(!unfinished("(a) / (b)"));
    assert!(unfinished("i++ / (2"));
    assert!(unfinished("a / (b"));
    assert!(unfinished("x[1] / (2"));
}

#[test]
fn scanning_continues_across_lines() {
    let mut scanner = Scanner::default();
    scanner.scan("`first ${\n");
    assert!(scanner.is_unfinished());
    scanner.scan("1 + {a: 2}.a\n");
    assert!(scanner.is_unfinished());
    scanner.scan("} last`\n");
    assert!(!scanner.is_unfinished());
}

#[test]
fn rewrites_top_level_declarations() {
    assert_eq!(
        rewrite_declarations("const w = await f()"),
        "      w = await f()"
    );
    assert_eq!(
        rewrite_declarations("let a=1; var b = 2"),
        "    a=1;     b = 2"
    );
    // Every name has an initializer; the commas in brackets are not the declaration's.
    assert_eq!(
        rewrite_declarations("let a = f(1, 2), b = { c: 3, d }, e = (g, h) => g"),
        "    a = f(1, 2), b = { c: 3, d }, e = (g, h) => g"
    );
    // A declaration ends at its `;`.
    assert_eq!(
        rewrite_declarations("let a = 1, b; const c = 2, d = 3"),
        "let a = 1, b;       c = 2, d = 3"
    );
}

#[test]
fn keeps_other_declarations() {
    for source in [
        "const { a } = await f()",
        "let [a] = await f()",
        "let x",
        "let a = await f(), b",
        "var a = 1, [b] = c",
        "let a = await f()\nb, c",
        "if (y) { const x = await f() }",
        "f(() => { let x = 1 })",
        "x.let = 1",
        "'const x = 1'",
        "`${ (() => { const x = 1 })() }`",
        "// const x = 1",
        "/const x = 1/.test(s)",
        "let x == y",
        "const f => 1",
    ] {
        assert_eq!(rewrite_declarations(source), source, "{source:?}");
    }
}

#[test]
fn candidates_wrap_inputs_that_await() {
    assert_eq!(
        candidates("1 + 2"),
        vec![ReplCandidate {
            source: "1 + 2".to_owned(),
            awaited: false,
        }]
    );
    let wrapped = candidates("const x = await f() // done");
    assert_eq!(wrapped.len(), 3);
    assert!(!wrapped[0].awaited);
    assert_eq!(
        wrapped[1].source,
        "(async () => {return (      x = await f() // done\n)})()"
    );
    assert!(wrapped[1].awaited);
    assert_eq!(
        wrapped[2].source,
        "(async () => {      x = await f() // done\n})()"
    );
    assert!(wrapped[2].awaited);
}

#[test]
fn returns_the_last_statement() {
    assert_eq!(
        returning_last_statement("a = await f(); a.b"),
        "(async () => {a = await f();return ( a.b\n)})()"
    );
    // Semicolons that end the input do not end a statement of their own.
    assert_eq!(
        returning_last_statement("await f(); g() ; ;"),
        "(async () => {await f();return ( g() \n)})()"
    );
    assert_eq!(
        returning_last_statement("await f();"),
        "(async () => {return (await f()\n)})()"
    );
    // Only top-level semicolons separate statements.
    assert_eq!(
        returning_last_statement("for (;;) { await f(); }"),
        "(async () => {return (for (;;) { await f(); }\n)})()"
    );
    assert_eq!(
        returning_last_statement("x = ';'; `${a;}`"),
        "(async () => {x = ';';return ( `${a;}`\n)})()"
    );
}

#[test]
fn completion_targets() {
    assert_eq!(
        completion_target("cons", 4),
        Some(CompletionTarget {
            start: 0,
            partial: "cons",
            base: "",
        })
    );
    assert_eq!(
        completion_target("await w.fe", 10),
        Some(CompletionTarget {
            start: 8,
            partial: "fe",
            base: "w",
        })
    );
    assert_eq!(
        completion_target("x = a.b.", 8),
        Some(CompletionTarget {
            start: 8,
            partial: "",
            base: "a.b",
        })
    );
    // Only what is before the cursor counts.
    assert_eq!(
        completion_target("foo.bar(1)", 5),
        Some(CompletionTarget {
            start: 4,
            partial: "b",
            base: "foo",
        })
    );
    assert_eq!(
        completion_target("f(", 2),
        Some(CompletionTarget {
            start: 2,
            partial: "",
            base: "",
        })
    );
    // Properties of what is not an identifier chain are not completed.
    for line in ["f().x", "1.5", "a..b", ".x"] {
        assert_eq!(completion_target(line, line.len()), None, "{line:?}");
    }
}

#[test]
fn identifiers() {
    assert!(is_identifier("fetch"));
    assert!(is_identifier("$_a1"));
    assert!(is_identifier("π"));
    assert!(!is_identifier(""));
    assert!(!is_identifier("1a"));
    assert!(!is_identifier("a-b"));
    assert!(!is_identifier("Symbol(foo)"));
}
