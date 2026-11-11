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
//! token before it, which the scanner judges by that token alone. Two judgments then differ from
//! the language's: after a `}` the scanner takes `/` to start a regular expression, which it does
//! after a block but not after an object literal; after a `)` it takes `/` to divide, which it
//! does after an expression but not after the condition of an `if` or a loop.

use super::bridge::ffi::ReplCandidate;

/// The keywords after which an expression starts, so that a `/` starts a regular expression.
const KEYWORDS_BEFORE_EXPRESSION: &[&[u8]] = &[
    b"await",
    b"case",
    b"delete",
    b"do",
    b"else",
    b"in",
    b"instanceof",
    b"new",
    b"of",
    b"return",
    b"throw",
    b"typeof",
    b"void",
    b"yield",
];

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
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Open {
    Bracket,
    /// A template literal's `${`, whose `}` returns to the template.
    Substitution,
}

/// Scans input in pieces as it is typed, keeping the lexical state between them.
#[derive(Clone, Debug)]
pub struct Scanner {
    mode: Mode,
    open: Vec<Open>,
    /// The next byte is escaped by a backslash.
    escaped: bool,
    /// A `/` in code here starts a regular expression.
    regex_allowed: bool,
    /// The last significant byte of code was a `.`, so a word is a property name.
    after_dot: bool,
}

impl Default for Scanner {
    fn default() -> Self {
        Self {
            mode: Mode::Code,
            open: Vec::new(),
            escaped: false,
            regex_allowed: true,
            after_dot: false,
        }
    }
}

/// A token of code the scanner reports.
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
}

impl Scanner {
    /// Whether the input scanned so far continues on another line: a bracket, a template
    /// literal or a block comment is open, or a string literal whose line ends in a backslash. A
    /// line break ends any other string literal, as a syntax error the evaluation reports.
    pub fn is_unfinished(&self) -> bool {
        !self.open.is_empty()
            || matches!(
                self.mode,
                Mode::String(_) | Mode::Template | Mode::BlockComment
            )
    }

    /// Scans the next line of input, which ends with its line break. A token does not continue
    /// from one piece into the next, and the line break ends a string literal unless escaped.
    pub fn scan(&mut self, piece: &str) {
        self.scan_tokens(piece.as_bytes(), |_| {});
    }

    fn scan_tokens(&mut self, bytes: &[u8], mut on_token: impl FnMut(Token)) {
        let mut i = 0;
        while i < bytes.len() {
            let byte = bytes[i];
            let next = bytes.get(i + 1).copied();
            match self.mode {
                Mode::Code => {
                    if is_word_byte(byte) {
                        let end = bytes[i..]
                            .iter()
                            .position(|&b| !is_word_byte(b))
                            .map_or(bytes.len(), |n| i + n);
                        let word = &bytes[i..end];
                        let property = self.after_dot;
                        on_token(Token::Word {
                            start: i,
                            end,
                            top_level: self.open.is_empty() && !property,
                        });
                        self.regex_allowed = !property
                            && !byte.is_ascii_digit()
                            && KEYWORDS_BEFORE_EXPRESSION.contains(&word);
                        self.after_dot = false;
                        i = end;
                        continue;
                    }
                    if byte == b';' && self.open.is_empty() {
                        on_token(Token::Semicolon(i));
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
                        self.open.push(Open::Substitution);
                        self.mode = Mode::Code;
                        self.regex_allowed = true;
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
                Mode::Regex { in_class } => {
                    if self.escaped {
                        self.escaped = false;
                    } else if byte == b'\\' {
                        self.escaped = true;
                    } else if byte == b'\n' || (byte == b'/' && !in_class) {
                        // The flags that follow scan as a word.
                        self.end_literal();
                    } else if byte == b'[' {
                        self.mode = Mode::Regex { in_class: true };
                    } else if byte == b']' {
                        self.mode = Mode::Regex { in_class: false };
                    }
                }
            }
            i += 1;
        }
    }

    /// One byte of code that does not start a word; `i` moves past what it consumes after it.
    fn scan_code(&mut self, byte: u8, next: Option<u8>, i: &mut usize) {
        if byte.is_ascii_whitespace() {
            return;
        }
        let regex_allowed = self.regex_allowed;
        self.after_dot = byte == b'.';
        // After most punctuation an expression starts; after a closing bracket a value ended.
        self.regex_allowed = !matches!(byte, b')' | b']' | b'.');
        match byte {
            b'(' | b'[' | b'{' => self.open.push(Open::Bracket),
            b')' | b']' => {
                if self.open.last() == Some(&Open::Bracket) {
                    self.open.pop();
                }
            }
            b'}' => match self.open.last() {
                Some(Open::Bracket) => {
                    self.open.pop();
                }
                Some(Open::Substitution) => {
                    self.open.pop();
                    self.mode = Mode::Template;
                }
                None => {}
            },
            b'\'' | b'"' => self.mode = Mode::String(byte),
            b'`' => self.mode = Mode::Template,
            // `x++ / 2`: an operand an increment or decrement ends.
            b'+' | b'-' if next == Some(byte) => {
                self.regex_allowed = false;
                *i += 1;
            }
            b'/' if next == Some(b'/') => {
                self.mode = Mode::LineComment;
                self.regex_allowed = regex_allowed;
                *i += 1;
            }
            b'/' if next == Some(b'*') => {
                self.mode = Mode::BlockComment;
                self.regex_allowed = regex_allowed;
                *i += 1;
            }
            b'/' if regex_allowed => self.mode = Mode::Regex { in_class: false },
            _ => {}
        }
    }

    /// A string, template or regular expression literal ended: a value, which a `/` divides.
    fn end_literal(&mut self) {
        self.mode = Mode::Code;
        self.regex_allowed = false;
        self.after_dot = false;
    }
}

/// Identifier characters, and every byte of a non-ASCII character (identifiers may contain them).
fn is_word_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$' || byte >= 0x80
}

/// The ways the REPL may compile `input`, in the order to try them: as it is, then, if it may
/// await at top level, wrapped in an async function, first returning the value of its last
/// statement ([`returning_last_statement`]) and then as statements whose result is `undefined`.
/// See [`rewrite_declarations`] for what the wrappings do to declarations. The session passes over
/// the input as it is when it uses `await` as a name, as `await (x)` does in a script
/// (`ReplEnvironment::compile`).
pub fn candidates(input: &str) -> Vec<ReplCandidate> {
    let mut candidates = vec![ReplCandidate {
        source: input.to_owned(),
        awaited: false,
    }];
    if input.contains("await") {
        let rewritten = rewrite_declarations(input);
        candidates.push(ReplCandidate {
            source: returning_last_statement(&rewritten),
            awaited: true,
        });
        // The line break keeps a line comment that ends the input from swallowing the wrapping.
        candidates.push(ReplCandidate {
            source: format!("(async () => {{{rewritten}\n}})()"),
            awaited: true,
        });
    }
    candidates
}

/// `source` as an async function that returns the value of its last statement, the code after its
/// last top-level `;` (not counting those that end it): `a = await f(); a.b` becomes
/// `(async () => {a = await f();return ( a.b\n)})()`. Lines and the columns past the first line
/// stay where they were. If the last statement is not an expression, this does not compile.
fn returning_last_statement(source: &str) -> String {
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
    // The line break keeps a line comment that ends the input from swallowing the parenthesis.
    match semicolons.last() {
        Some(&split) => format!(
            "(async () => {{{};return ({}\n)}})()",
            &source[..split],
            &source[split + 1..end]
        ),
        None => format!("(async () => {{return ({}\n)}})()", &source[..end]),
    }
}

/// Turns the top-level `const`, `let` and `var` declarations of single names in `source` into
/// assignments, by blanking the keyword: `const x = await f()` becomes `      x = await f()`.
/// A declaration in the async function that lets an input await would end with the function;
/// the assignment makes a global that later inputs see, as Node's REPL does. Destructuring
/// declarations, declarations without an initializer and declarations inside brackets are kept.
pub fn rewrite_declarations(source: &str) -> String {
    let bytes = source.as_bytes();
    let mut blanked = Vec::new();
    Scanner::default().scan_tokens(bytes, |token| {
        if let Token::Word {
            start,
            end,
            top_level: true,
        } = token
            && matches!(&bytes[start..end], b"const" | b"let" | b"var")
            && declares_single_name(&bytes[end..])
        {
            blanked.push(start..end);
        }
    });
    let mut rewritten = source.to_owned();
    for range in blanked {
        // The keywords are ASCII, so blanking them keeps the string valid and every column put.
        rewritten.replace_range(range.clone(), &" ".repeat(range.len()));
    }
    rewritten
}

/// Whether `rest`, what follows a declaration keyword, starts with a name and then `=` (and not
/// `==` or `=>`).
fn declares_single_name(rest: &[u8]) -> bool {
    let rest = trim_start(rest);
    let name_length = rest.iter().take_while(|&&b| is_word_byte(b)).count();
    if name_length == 0 || rest[0].is_ascii_digit() {
        return false;
    }
    let rest = trim_start(&rest[name_length..]);
    rest.first() == Some(&b'=') && !matches!(rest.get(1), Some(b'=' | b'>'))
}

fn trim_start(bytes: &[u8]) -> &[u8] {
    let start = bytes
        .iter()
        .position(|b| !b.is_ascii_whitespace())
        .unwrap_or(bytes.len());
    &bytes[start..]
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
