// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! What the REPL reads in its input without parsing JavaScript: whether an input is finished or
//! continues on the next line, how an input that awaits at top level may compile, and what tab
//! completion completes.
//!
//! A [`Scanner`] follows JavaScript's lexical structure closely enough to know when it is inside
//! a string, a template literal and its `${}` substitutions, a comment, a regular expression
//! literal, or brackets. Whether a `/` starts a regular expression or divides depends on the
//! token before it. After a `)`, a `/` starts one only if the parentheses hold the condition of an
//! `if`, a loop or a `with`. After a `}`, it starts one only if the braces are a block rather than
//! an object literal, which the scanner judges by what precedes the `{`: a `{` that starts a
//! statement, or follows a token after which an expression can't start (as the `)` before a
//! function body), opens a block; any other `{`, such as one after a `:`, opens an object literal.
//! A block's `}` can also end a function or class expression, after which `/` divides, so a
//! regular expression after one that reaches a line break, which it can't contain, is scanned
//! again as a division. A closing bracket that doesn't match the open one finishes the input, as
//! a syntax error the evaluation reports.

use std::mem;
use std::ops::Range;

use super::bridge::ffi::ReplCandidate;

/// What the last token of code was, as far as that decides how the next is read: whether a `/`
/// starts a regular expression or divides, and whether a `{` opens a block or an object literal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum After {
    /// The start of a statement, as after a `;` or a block's `{`: a `/` starts a regular
    /// expression, and a `{` opens a block.
    Statement,
    /// A block's `}`, after which a statement starts. But the `}` may end a function or class
    /// expression instead, after which a `/` divides: a `/` starts a regular expression that is
    /// scanned again as a division if it reaches a line break ([`Mode::Regex`]).
    Block,
    /// A token that an expression follows, such as an operator, a `(` or `return`: a `/` starts a
    /// regular expression, and a `{` opens an object literal.
    Operator,
    /// The end of a value, such as a name, a literal or an expression's `)`: a `/` divides, and a
    /// `{` opens a body, such as a function's.
    Value,
    /// `if`, `while`, `for` or `with`, whose `(` opens a condition.
    Condition,
    /// A `.`, after which a word is a property name.
    Dot,
}

impl After {
    /// What a word that is not a property name is.
    fn word(word: &[u8]) -> Self {
        match word {
            b"do" | b"else" => Self::Statement,
            b"for" | b"if" | b"while" | b"with" => Self::Condition,
            b"await" | b"case" | b"delete" | b"in" | b"instanceof" | b"new" | b"of" | b"return"
            | b"throw" | b"typeof" | b"void" | b"yield" => Self::Operator,
            _ => Self::Value,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Code,
    /// A string literal and its quote.
    String(u8),
    Template,
    LineComment,
    BlockComment,
    Regex {
        in_class: bool,
        /// The offset in the piece scanned of the `/` that started it, if that `/` may divide
        /// instead: one after a block's `}`. A regular expression that reaches a line break or
        /// the end of the piece is then scanned again from there, so this never outlives the
        /// piece.
        slash: Option<usize>,
    },
}

/// An open bracket or substitution, and the line of the input it opens on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Open {
    /// `(`, `[` or `{`. After the closing bracket a statement starts, so that a `/` starts a
    /// regular expression, if `statement_after`: this `(` holds the condition of an `if`, a loop
    /// or a `with`, or this `{` opens a block.
    Bracket {
        byte: u8,
        line: usize,
        statement_after: bool,
    },
    /// A template literal's `${`, whose `}` returns to the template, which starts on
    /// `template_line`.
    Substitution { line: usize, template_line: usize },
}

/// What keeps an input unfinished: the innermost construct it leaves open, such as "`(`" or
/// "template literal", and the line of the input that construct starts on, 1 for the first.
#[derive(Debug, PartialEq, Eq)]
pub struct Unclosed {
    pub what: &'static str,
    pub line: usize,
}

/// Scans input in pieces as it is typed, keeping the lexical state between them.
#[derive(Clone, Debug)]
pub struct Scanner {
    mode: Mode,
    /// The line the string, template literal or block comment `mode` is in starts on.
    mode_line: usize,
    open: Vec<Open>,
    /// The line of the input being scanned, 1 for the first.
    line: usize,
    /// The next byte is escaped by a backslash.
    escaped: bool,
    /// What the last token of code was.
    after: After,
    /// A closing bracket didn't match the open one: the input is finished, as a syntax error.
    mismatched: bool,
}

impl Default for Scanner {
    fn default() -> Self {
        Self {
            mode: Mode::Code,
            mode_line: 1,
            open: Vec::new(),
            line: 1,
            escaped: false,
            after: After::Statement,
            mismatched: false,
        }
    }
}

/// A token of code the scanner reports.
#[derive(Clone, Copy)]
enum Token {
    /// A word: its byte range in the piece scanned, and whether it is at the top level of the
    /// input and not a property name.
    Word {
        start: usize,
        end: usize,
        top_level: bool,
    },
    /// A `;` at the top level of the input, and its offset in the piece scanned.
    Semicolon(usize),
    /// A `,` at the top level of the input, and its offset in the piece scanned.
    Comma(usize),
}

impl Scanner {
    /// Whether the input scanned so far continues on another line: a bracket, a template
    /// literal or a block comment is open, or a string literal whose line ends in a backslash. A
    /// line break ends any other string literal, as a syntax error the evaluation reports.
    pub fn is_unfinished(&self) -> bool {
        self.unclosed().is_some()
    }

    /// What keeps the input scanned so far unfinished, if it is ([`Self::is_unfinished`]).
    pub fn unclosed(&self) -> Option<Unclosed> {
        if self.mismatched {
            return None;
        }
        let literal = match self.mode {
            Mode::String(_) => Some("string literal"),
            Mode::Template => Some("template literal"),
            Mode::BlockComment => Some("`/*` comment"),
            Mode::Code | Mode::LineComment | Mode::Regex { .. } => None,
        };
        if let Some(what) = literal {
            return Some(Unclosed {
                what,
                line: self.mode_line,
            });
        }
        self.open.last().map(|open| match *open {
            Open::Bracket { byte, line, .. } => Unclosed {
                what: match byte {
                    b'(' => "`(`",
                    b'[' => "`[`",
                    _ => "`{`",
                },
                line,
            },
            Open::Substitution { line, .. } => Unclosed { what: "`${`", line },
        })
    }

    /// Scans the next line of input, which ends with its line break. A token does not continue
    /// from one piece into the next, and the line break ends a string literal unless escaped.
    pub fn scan(&mut self, piece: &str) {
        self.scan_tokens(piece.as_bytes(), |_| {});
    }

    fn scan_tokens(&mut self, bytes: &[u8], mut on_token: impl FnMut(Token)) {
        let mut i = 0;
        loop {
            let Some(&byte) = bytes.get(i) else {
                // The end of the piece ends a regular expression literal, as a line break does.
                match self.reconsider_regex() {
                    Some(slash) => {
                        i = slash;
                        continue;
                    }
                    None => return,
                }
            };
            let next = bytes.get(i + 1).copied();
            match self.mode {
                Mode::Code => {
                    if is_word_byte(byte) {
                        i = self.scan_word(bytes, i, &mut on_token);
                        continue;
                    }
                    if self.open.is_empty() {
                        match byte {
                            b';' => on_token(Token::Semicolon(i)),
                            b',' => on_token(Token::Comma(i)),
                            _ => {}
                        }
                    }
                    self.scan_code(byte, next, &mut i);
                }
                Mode::String(quote) => {
                    if self.escaped {
                        self.escaped = false;
                    } else if byte == b'\\' {
                        self.escaped = true;
                    } else if byte == quote || byte == b'\n' {
                        self.end_literal();
                    }
                }
                Mode::Template => {
                    if self.escaped {
                        self.escaped = false;
                    } else if byte == b'\\' {
                        self.escaped = true;
                    } else if byte == b'`' {
                        self.end_literal();
                    } else if byte == b'$' && next == Some(b'{') {
                        self.open.push(Open::Substitution {
                            line: self.line,
                            template_line: self.mode_line,
                        });
                        self.mode = Mode::Code;
                        self.after = After::Operator;
                        i += 1;
                    }
                }
                Mode::LineComment => {
                    if byte == b'\n' {
                        self.mode = Mode::Code;
                    }
                }
                Mode::BlockComment => {
                    if byte == b'*' && next == Some(b'/') {
                        self.mode = Mode::Code;
                        i += 1;
                    }
                }
                Mode::Regex { in_class, slash } => {
                    // A regular expression can't contain a line break, even after a `\`.
                    if byte == b'\n' {
                        if let Some(at) = self.reconsider_regex() {
                            i = at;
                            continue;
                        }
                        self.end_literal();
                    } else if self.escaped {
                        self.escaped = false;
                    } else if byte == b'\\' {
                        self.escaped = true;
                    } else if byte == b'/' && !in_class {
                        // The flags that follow scan as a word.
                        self.end_literal();
                    } else if byte == b'[' {
                        self.mode = Mode::Regex {
                            in_class: true,
                            slash,
                        };
                    } else if byte == b']' {
                        self.mode = Mode::Regex {
                            in_class: false,
                            slash,
                        };
                    }
                }
            }
            // Only the byte at `i` can be a line break: what scanning consumes after it never is.
            if byte == b'\n' {
                self.line += 1;
            }
            i += 1;
        }
    }

    /// The word of code that starts at `start`; returns where it ends.
    fn scan_word(&mut self, bytes: &[u8], start: usize, on_token: &mut impl FnMut(Token)) -> usize {
        let end = bytes[start..]
            .iter()
            .position(|&b| !is_word_byte(b))
            .map_or(bytes.len(), |n| start + n);
        let word = &bytes[start..end];
        let property = self.after == After::Dot;
        on_token(Token::Word {
            start,
            end,
            top_level: self.open.is_empty() && !property,
        });
        self.after = if property {
            After::Value
        } else {
            After::word(word)
        };
        end
    }

    /// One byte of code that does not start a word; `i` moves past what it consumes after it.
    fn scan_code(&mut self, byte: u8, next: Option<u8>, i: &mut usize) {
        if byte.is_ascii_whitespace() {
            return;
        }
        // A comment is not a token: what the token before it decided holds after it.
        if byte == b'/' && next == Some(b'/') {
            self.mode = Mode::LineComment;
            *i += 1;
            return;
        }
        if byte == b'/' && next == Some(b'*') {
            self.start_mode(Mode::BlockComment, self.line);
            *i += 1;
            return;
        }
        // After most punctuation an expression starts.
        let after = mem::replace(&mut self.after, After::Operator);
        match byte {
            b'(' | b'[' | b'{' => {
                let statement_after = match byte {
                    b'(' => after == After::Condition,
                    b'[' => false,
                    // Where an expression starts, a `{` opens an object literal; elsewhere, a
                    // block, or a body such as the one after a function's parameters.
                    _ => after != After::Operator,
                };
                if byte == b'{' && statement_after {
                    self.after = After::Statement;
                }
                self.open.push(Open::Bracket {
                    byte,
                    line: self.line,
                    statement_after,
                });
            }
            b')' | b']' | b'}' => self.close(byte),
            b'.' => self.after = After::Dot,
            b';' => self.after = After::Statement,
            // An arrow function's body.
            b'=' if next == Some(b'>') => {
                self.after = After::Statement;
                *i += 1;
            }
            b'\'' | b'"' => self.start_mode(Mode::String(byte), self.line),
            b'`' => self.start_mode(Mode::Template, self.line),
            // `x++ / 2`: an operand an increment or decrement ends.
            b'+' | b'-' if next == Some(byte) => {
                self.after = After::Value;
                *i += 1;
            }
            b'/' if matches!(after, After::Statement | After::Block | After::Operator) => {
                self.mode = Mode::Regex {
                    in_class: false,
                    slash: (after == After::Block).then_some(*i),
                };
            }
            _ => {}
        }
    }

    /// Closes the innermost bracket or substitution with `byte`, a `)`, `]` or `}`, or finds the
    /// input mismatched if `byte` doesn't close it.
    fn close(&mut self, byte: u8) {
        // A value ends with it, unless it closes a condition or a block.
        self.after = After::Value;
        match self.open.last() {
            Some(&Open::Bracket {
                byte: open,
                statement_after,
                ..
            }) if matches!((open, byte), (b'(', b')') | (b'[', b']') | (b'{', b'}')) => {
                self.open.pop();
                if statement_after {
                    self.after = if byte == b'}' {
                        After::Block
                    } else {
                        After::Statement
                    };
                }
            }
            Some(&Open::Substitution { template_line, .. }) if byte == b'}' => {
                self.open.pop();
                self.start_mode(Mode::Template, template_line);
            }
            Some(_) => self.mismatched = true,
            None => {}
        }
    }

    /// For a regular expression literal that meets a line break, which it can't contain, or the
    /// end of the piece: if its `/` may divide instead ([`Mode::Regex`]), goes back to code to scan
    /// that `/` again as a division, and returns its offset.
    fn reconsider_regex(&mut self) -> Option<usize> {
        let Mode::Regex {
            slash: Some(slash), ..
        } = self.mode
        else {
            return None;
        };
        self.mode = Mode::Code;
        self.escaped = false;
        self.after = After::Value;
        Some(slash)
    }

    /// Enters a string, a template literal or a block comment that starts on `line`.
    fn start_mode(&mut self, mode: Mode, line: usize) {
        self.mode = mode;
        self.mode_line = line;
    }

    /// A string, template or regular expression literal ended: a value, which a `/` divides.
    fn end_literal(&mut self) {
        self.mode = Mode::Code;
        self.escaped = false;
        self.after = After::Value;
    }
}

/// Identifier characters, and every byte of a non-ASCII character (identifiers may contain them).
fn is_word_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$' || byte >= 0x80
}

/// The ways the REPL may compile `input`, in the order to try them: as it is, then, if it may
/// await at top level, wrapped in an async function, first returning the value of its last
/// statement ([`returning_last_statement`]) and then as statements whose result is `undefined`.
/// See [`rewrite_declarations`] for what the wrappings do to declarations; an input whose last
/// statement is one of those declarations has only the second wrapping, so that it prints
/// `undefined`, as a declaration does. The session passes over the input as it is when it uses
/// `await` as a name, as `await (x)` does in a script (`ReplEnvironment::compile`).
///
/// An input that starts with `{` and doesn't end with `;` is first tried in parentheses, so that
/// `{ a: 1 }` is an object literal rather than a block, as in Node's REPL. Without them it is a
/// block, as one that the parentheses don't compile, such as `{ let x = 1; x }`, still is.
pub fn candidates(input: &str) -> Vec<ReplCandidate> {
    let awaits = input.contains("await");
    let mut candidates = Vec::new();
    if input.trim_start().starts_with('{') && !input.trim_end().ends_with(';') {
        // The line break keeps a line comment that ends the input from swallowing the parenthesis.
        let parenthesized = format!("({input}\n)");
        let returning = awaits.then(|| returning_last_statement(&parenthesized));
        candidates.push(ReplCandidate {
            source: parenthesized,
            awaited: false,
            parenthesized: true,
        });
        if let Some(source) = returning {
            candidates.push(ReplCandidate {
                source,
                awaited: true,
                parenthesized: true,
            });
        }
    }
    candidates.push(ReplCandidate {
        source: input.to_owned(),
        awaited: false,
        parenthesized: false,
    });
    if awaits {
        let Rewritten {
            source: rewritten,
            declarations,
            ends_with_declaration,
        } = rewrite_declarations(input);
        if !ends_with_declaration {
            candidates.push(ReplCandidate {
                source: format!("{declarations}{}", returning_last_statement(&rewritten)),
                awaited: true,
                parenthesized: false,
            });
        }
        // The line break keeps a line comment that ends the input from swallowing the wrapping.
        candidates.push(ReplCandidate {
            source: format!("{declarations}(async () => {{{rewritten}\n}})()"),
            awaited: true,
            parenthesized: false,
        });
    }
    candidates
}

/// Where the last statement of `source` is: the code after its last top-level `;`, not counting
/// those that end it, up to them.
fn last_statement(source: &str) -> Range<usize> {
    let mut semicolons = Vec::new();
    Scanner::default().scan_tokens(source.as_bytes(), |token| {
        if let Token::Semicolon(at) = token {
            semicolons.push(at);
        }
    });
    let mut end = source.len();
    while let Some(&last) = semicolons.last()
        && source[last + 1..end].trim().is_empty()
    {
        end = last;
        semicolons.pop();
    }
    semicolons.last().map_or(0, |&split| split + 1)..end
}

/// `source` as an async function that returns the value of its last statement, the code after its
/// last top-level `;` (not counting those that end it): `a = await f(); a.b` becomes
/// `(async () => {a = await f();return ( a.b\n)})()`. Lines and the columns past the first line
/// stay where they were. If the last statement is not an expression, this does not compile.
fn returning_last_statement(source: &str) -> String {
    let last = last_statement(source);
    // The line break keeps a line comment that ends the input from swallowing the parenthesis.
    if last.start == 0 {
        format!("(async () => {{return ({}\n)}})()", &source[..last.end])
    } else {
        format!(
            "(async () => {{{};return ({}\n)}})()",
            &source[..last.start - 1],
            &source[last]
        )
    }
}

/// What [`rewrite_declarations`] makes of an input.
#[derive(Debug, PartialEq, Eq)]
pub struct Rewritten {
    /// The input, with the keywords of the declarations it turns into assignments blanked.
    pub source: String,
    /// The declarations of their names, to make ahead of the async function, on the first line:
    /// `let a, b;var c;`, or nothing.
    pub declarations: String,
    /// Whether the input's last statement is one of those declarations.
    pub ends_with_declaration: bool,
}

/// Turns the top-level `const`, `let` and `var` declarations in `source` that give each of their
/// names an initializer into assignments, by blanking the keyword: `const x = await f()` becomes
/// `      x = await f()`. A declaration in the async function that lets an input await would end
/// with the function. So the names are declared ahead of it instead, at the top level of the
/// script, `const` ones with `let`, as Node's REPL does: later inputs see them, and redeclaring
/// one is the error it would be without `await`. Declarations with a destructuring pattern or a
/// name without an initializer (in `let a = 1, b`, the assignment would leave `b` an undefined
/// name) are kept, as are declarations inside brackets. A declaration is taken to end at the next
/// top-level `;`, so one that a line break ends is kept if a top-level `,` after it is not
/// followed by a name and `=`.
pub fn rewrite_declarations(source: &str) -> Rewritten {
    let bytes = source.as_bytes();
    let mut tokens = Vec::new();
    Scanner::default().scan_tokens(bytes, |token| tokens.push(token));
    let mut blanked = Vec::new();
    let mut lexical = Vec::new();
    let mut vars = Vec::new();
    for (i, token) in tokens.iter().enumerate() {
        let Token::Word {
            start,
            end,
            top_level: true,
        } = *token
        else {
            continue;
        };
        let keyword = &source[start..end];
        if !matches!(keyword, "const" | "let" | "var") {
            continue;
        }
        let names: Option<Vec<&str>> = std::iter::once(end)
            .chain(
                tokens[i + 1..]
                    .iter()
                    .take_while(|token| !matches!(token, Token::Semicolon(_)))
                    .filter_map(|token| match *token {
                        Token::Comma(at) => Some(at + 1),
                        _ => None,
                    }),
            )
            .map(|at| initialized_name(source, at))
            .collect();
        if let Some(names) = names {
            blanked.push(start..end);
            if keyword == "var" {
                &mut vars
            } else {
                &mut lexical
            }
            .extend(names);
        }
    }
    let ends_with_declaration = blanked
        .last()
        .is_some_and(|keyword| keyword.start >= last_statement(source).start);
    let mut declarations = String::new();
    for (keyword, names) in [("let", &lexical), ("var", &vars)] {
        if !names.is_empty() {
            declarations += &format!("{keyword} {};", names.join(", "));
        }
    }
    let mut rewritten = source.to_owned();
    for range in blanked {
        // The keywords are ASCII, so blanking them keeps the string valid and every column put.
        rewritten.replace_range(range.clone(), &" ".repeat(range.len()));
    }
    Rewritten {
        source: rewritten,
        declarations,
        ends_with_declaration,
    }
}

/// The name that `source` starts with at `at`, what follows a declaration keyword or the `,`
/// before a declaration's next name, if `=` (and not `==` or `=>`) follows it.
fn initialized_name(source: &str, at: usize) -> Option<&str> {
    let start = at + source[at..].len() - source[at..].trim_start().len();
    let name_length = source.as_bytes()[start..]
        .iter()
        .take_while(|&&b| is_word_byte(b))
        .count();
    let name = &source[start..start + name_length];
    if name.is_empty() || name.as_bytes()[0].is_ascii_digit() {
        return None;
    }
    let rest = source[start + name_length..].trim_start().as_bytes();
    (rest.first() == Some(&b'=') && !matches!(rest.get(1), Some(b'=' | b'>'))).then_some(name)
}

/// What tab completion completes in a line: the name being typed before the cursor, and the
/// identifier chain whose properties it names, if any.
#[derive(Debug, PartialEq, Eq)]
pub struct CompletionTarget<'a> {
    /// Where the name being typed starts, a byte offset into the line.
    pub start: usize,
    /// The name typed so far.
    pub partial: &'a str,
    /// The expression before the last `.`, such as `a.b` in `a.b.c`; empty to complete a global.
    pub base: &'a str,
}

/// The completion target of `line` with the cursor at byte offset `cursor`: the run of identifier
/// characters and dots that ends at the cursor, split at its last dot. None if what is before the
/// dot is not an identifier chain, as in `f().x` or `1.5`.
pub fn completion_target(line: &str, cursor: usize) -> Option<CompletionTarget<'_>> {
    let before = &line[..cursor];
    let tail_start = before
        .bytes()
        .rposition(|b| !(is_word_byte(b) || b == b'.'))
        .map_or(0, |i| i + 1);
    let tail = &before[tail_start..];
    let Some(dot) = tail.rfind('.') else {
        return Some(CompletionTarget {
            start: tail_start,
            partial: tail,
            base: "",
        });
    };
    let base = &tail[..dot];
    base.split('.')
        .all(is_identifier)
        .then_some(CompletionTarget {
            start: tail_start + dot + 1,
            partial: &tail[dot + 1..],
            base,
        })
}

/// Whether `name` can be typed as it is after a `.` or as a global: an identifier.
pub fn is_identifier(name: &str) -> bool {
    let mut bytes = name.bytes();
    bytes
        .next()
        .is_some_and(|first| is_word_byte(first) && !first.is_ascii_digit())
        && bytes.all(is_word_byte)
}

#[cfg(test)]
#[path = "input-test.rs"]
mod tests;
