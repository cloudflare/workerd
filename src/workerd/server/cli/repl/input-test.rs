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

/// What keeps `input`, scanned a line at a time as the console does, unfinished.
fn unclosed(input: &str) -> Option<Unclosed> {
    let mut scanner = Scanner::default();
    for line in input.split('\n') {
        scanner.scan(&format!("{line}\n"));
    }
    scanner.unclosed()
}

#[test]
fn unclosed_constructs() {
    let at = |what, line| Some(Unclosed { what, line });
    assert_eq!(unclosed("f(1)"), None);
    assert_eq!(unclosed("f("), at("`(`", 1));
    assert_eq!(unclosed("f(\n  [1,"), at("`[`", 2));
    assert_eq!(unclosed("x = 1\n\nfunction f() {"), at("`{`", 3));
    // The innermost construct still open: those inside it have closed.
    assert_eq!(unclosed("f({\n  a: [1]\n}"), at("`(`", 1));
    assert_eq!(unclosed("`multi\nline"), at("template literal", 1));
    assert_eq!(unclosed("`a ${\n  b"), at("`${`", 1));
    assert_eq!(unclosed("`a ${ {"), at("`{`", 1));
    // A template literal goes on once its substitution closes.
    assert_eq!(unclosed("x\n`a ${ b }\nc"), at("template literal", 2));
    assert_eq!(unclosed("1 /* comment\nmore"), at("`/*` comment", 1));
    assert_eq!(unclosed("1\n'continued \\"), at("string literal", 2));
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

fn rewritten(source: &str, declarations: &str, ends_with_declaration: bool) -> Rewritten {
    Rewritten {
        source: source.to_owned(),
        declarations: declarations.to_owned(),
        ends_with_declaration,
    }
}

#[test]
fn rewrites_top_level_declarations() {
    assert_eq!(
        rewrite_declarations("const w = await f()"),
        rewritten("      w = await f()", "let w;", true)
    );
    assert_eq!(
        rewrite_declarations("let a=1; var b = 2; a + b"),
        rewritten("    a=1;     b = 2; a + b", "let a;var b;", false)
    );
    // Every name has an initializer; the commas in brackets are not the declaration's.
    assert_eq!(
        rewrite_declarations("let a = f(1, 2), b = { c: 3, d }, e = (g, h) => g"),
        rewritten(
            "    a = f(1, 2), b = { c: 3, d }, e = (g, h) => g",
            "let a, b, e;",
            true
        )
    );
    // A declaration ends at its `;`.
    assert_eq!(
        rewrite_declarations("let a = 1, b; const c = 2, d = 3;"),
        rewritten("let a = 1, b;       c = 2, d = 3;", "let c, d;", true)
    );
    assert_eq!(
        rewrite_declarations("const \u{e9}t\u{e9} = await f()\n\nx"),
        rewritten(
            "      \u{e9}t\u{e9} = await f()\n\nx",
            "let \u{e9}t\u{e9};",
            true
        )
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
        assert_eq!(
            rewrite_declarations(source),
            rewritten(source, "", false),
            "{source:?}"
        );
    }
}

#[test]
fn candidates_wrap_inputs_that_await() {
    assert_eq!(
        candidates("1 + 2"),
        vec![ReplCandidate {
            source: "1 + 2".to_owned(),
            awaited: false,
            parenthesized: false,
        }]
    );
    let wrapped = candidates("const x = await f(); x // done");
    assert_eq!(wrapped.len(), 3);
    assert!(!wrapped[0].awaited);
    assert_eq!(
        wrapped[1].source,
        "let x;(async () => {      x = await f();return ( x // done\n)})()"
    );
    assert!(wrapped[1].awaited);
    assert_eq!(
        wrapped[2].source,
        "let x;(async () => {      x = await f(); x // done\n})()"
    );
    assert!(wrapped[2].awaited);
}

#[test]
fn candidates_of_an_input_that_ends_with_a_declaration_return_nothing() {
    let wrapped = candidates("x = 1; var y = await f()\n");
    assert_eq!(
        wrapped
            .into_iter()
            .map(|c| (c.source, c.awaited))
            .collect::<Vec<_>>(),
        [
            ("x = 1; var y = await f()\n".to_owned(), false),
            (
                "var y;(async () => {x = 1;     y = await f()\n\n})()".to_owned(),
                true
            ),
        ]
    );
}

#[test]
fn candidates_parenthesize_a_leading_brace() {
    let sources = |input| {
        candidates(input)
            .into_iter()
            .map(|c| (c.source, c.awaited, c.parenthesized))
            .collect::<Vec<_>>()
    };
    assert_eq!(
        sources("  { a: 1 } // c"),
        [
            ("(  { a: 1 } // c\n)".to_owned(), false, true),
            ("  { a: 1 } // c".to_owned(), false, false),
        ]
    );
    let awaiting = sources("{ a: await f() }");
    assert_eq!(
        awaiting[..2],
        [
            ("({ a: await f() }\n)".to_owned(), false, true),
            (
                "(async () => {return (({ a: await f() }\n)\n)})()".to_owned(),
                true,
                true
            ),
        ]
    );
    assert_eq!(awaiting.len(), 5);
    assert!(
        awaiting[2..]
            .iter()
            .all(|&(_, _, parenthesized)| !parenthesized)
    );
    // One that ends with `;`, or doesn't start with `{`, is left as it is, as in Node's REPL.
    for input in ["{ a: 1 };", "{ f() }; ", "x = { a: 1 }", "({ a: 1 })"] {
        assert!(
            candidates(input).iter().all(|c| !c.parenthesized),
            "{input:?}"
        );
    }
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
