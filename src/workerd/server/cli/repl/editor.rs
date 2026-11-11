// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The REPL's line editor: editing within the line, Emacs-style control keys, history, and tab
//! completion.
//!
//! The editor reads from a [`Terminal`] whatever input is available at once, handles all of it,
//! and then redraws the line once, so a paste costs one redraw per read rather than one per
//! character. What it reads past the end of a line stays queued for the next line. It edits
//! characters, not bytes, and takes each to be one column wide, so the cursor is drawn in the
//! wrong place on a line with wide characters (such as CJK) or one longer than the terminal.

use std::collections::VecDeque;
use std::io;
use std::time::Duration;

/// How long to wait for the rest of an escape sequence before taking an Escape byte to be the
/// Escape key.
const ESCAPE_TIMEOUT: Duration = Duration::from_millis(50);

/// How wide a candidate listing may be.
const LISTING_WIDTH: usize = 80;

/// The most entries the history keeps; adding another drops the oldest.
pub const HISTORY_LIMIT: usize = 1000;

/// Input and output, as raw bytes and text.
pub trait Terminal {
    /// What input is available, after waiting at most `timeout` (with `None`, as long as it
    /// takes) for some: empty if the wait timed out, `None` once the input has ended.
    fn read(&mut self, timeout: Option<Duration>) -> io::Result<Option<Vec<u8>>>;

    fn write(&mut self, output: &str) -> io::Result<()>;
}

/// Names that can replace the line from byte offset `start` to the cursor.
pub struct Completion {
    pub start: usize,
    pub candidates: Vec<String>,
}

/// How [`Editor::read_line`] ended.
#[derive(Debug, PartialEq, Eq)]
pub enum Line {
    /// Enter, or the end of the input after some text.
    Submitted(String),
    /// Ctrl+C: abandon the input.
    Canceled,
    /// Ctrl+D on an empty line, or the end of the input.
    Ended,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Key {
    Char(char),
    Enter,
    Tab,
    Backspace,
    Delete,
    Left,
    Right,
    Home,
    End,
    Up,
    Down,
    /// Ctrl+C.
    Interrupt,
    /// Ctrl+D.
    EndOfInput,
    /// Ctrl+K.
    KillToEnd,
    /// Ctrl+U.
    KillToStart,
    /// Ctrl+W.
    KillWord,
    /// Ctrl+L.
    ClearScreen,
    /// A control character or escape sequence the editor does not bind.
    Ignored,
}

enum Decoded {
    Key(Key, usize),
    /// The queued bytes start a key whose remaining bytes have not arrived.
    Partial,
}

/// The line being edited.
#[derive(Default)]
struct Edit {
    text: Vec<char>,
    cursor: usize,
    /// The history entry shown, if browsing history, and the line being edited before.
    history_index: Option<usize>,
    stashed: Vec<char>,
}

impl Edit {
    fn line(&self) -> String {
        self.text.iter().collect()
    }

    fn set(&mut self, text: &str) {
        self.text = text.chars().collect();
        self.cursor = self.text.len();
    }

    fn insert(&mut self, c: char) {
        self.text.insert(self.cursor, c);
        self.cursor += 1;
    }

    fn replace(&mut self, start: usize, replacement: &str) {
        let end = self.cursor;
        let replacement: Vec<char> = replacement.chars().collect();
        self.cursor = start + replacement.len();
        self.text.splice(start..end, replacement);
    }
}

pub struct Editor {
    history: Vec<String>,
    queued: VecDeque<u8>,
}

impl Editor {
    pub fn new() -> Self {
        Self {
            history: Vec::new(),
            queued: VecDeque::new(),
        }
    }

    /// Adds `line` to the history, unless it is empty or repeats the latest entry; whether it
    /// did.
    pub fn add_history(&mut self, line: &str) -> bool {
        if line.is_empty() || self.history.last().is_some_and(|last| last == line) {
            return false;
        }
        if self.history.len() == HISTORY_LIMIT {
            self.history.remove(0);
        }
        self.history.push(line.to_owned());
        true
    }

    /// Edits one line after `prompt`. `complete` gives the completions for a line and a cursor (a
    /// byte offset); it is called on Tab.
    pub fn read_line(
        &mut self,
        terminal: &mut dyn Terminal,
        prompt: &str,
        complete: &mut dyn FnMut(&str, usize) -> Completion,
    ) -> io::Result<Line> {
        let mut edit = Edit::default();
        // Whether the line needs drawing: at first, and after keys.
        let mut changed = true;
        loop {
            let mut partial = false;
            while !self.queued.is_empty() {
                let Decoded::Key(key, length) = decode(self.queued.make_contiguous()) else {
                    partial = true;
                    break;
                };
                self.queued.drain(..length);
                if let Some(line) = self.handle(key, &mut edit, terminal, prompt, complete)? {
                    return Ok(line);
                }
                changed = true;
            }
            if changed {
                terminal.write(&render(prompt, &edit))?;
                changed = false;
            }

            let timeout =
                (partial && self.queued.front() == Some(&ESCAPE)).then_some(ESCAPE_TIMEOUT);
            match terminal.read(timeout)? {
                Some(bytes) if bytes.is_empty() => {
                    // The Escape key on its own, or a sequence cut short: nothing.
                    self.queued.clear();
                }
                Some(bytes) => self.queued.extend(bytes),
                None => {
                    // What remains of a character cut off by the end is not one.
                    self.queued.clear();
                    terminal.write("\r\n")?;
                    return Ok(if edit.text.is_empty() {
                        Line::Ended
                    } else {
                        Line::Submitted(edit.line())
                    });
                }
            }
        }
    }

    /// Applies `key`; the line, if it ends the editing.
    fn handle(
        &self,
        key: Key,
        edit: &mut Edit,
        terminal: &mut dyn Terminal,
        prompt: &str,
        complete: &mut dyn FnMut(&str, usize) -> Completion,
    ) -> io::Result<Option<Line>> {
        match key {
            Key::Char(c) => edit.insert(c),
            Key::Enter => {
                terminal.write("\r\n")?;
                return Ok(Some(Line::Submitted(edit.line())));
            }
            Key::Interrupt => {
                terminal.write("^C\r\n")?;
                return Ok(Some(Line::Canceled));
            }
            Key::EndOfInput if edit.text.is_empty() => {
                terminal.write("\r\n")?;
                return Ok(Some(Line::Ended));
            }
            Key::EndOfInput | Key::Delete => {
                if edit.cursor < edit.text.len() {
                    edit.text.remove(edit.cursor);
                }
            }
            Key::Backspace => {
                if edit.cursor > 0 {
                    edit.cursor -= 1;
                    edit.text.remove(edit.cursor);
                }
            }
            Key::Left => edit.cursor = edit.cursor.saturating_sub(1),
            Key::Right => edit.cursor = (edit.cursor + 1).min(edit.text.len()),
            Key::Home => edit.cursor = 0,
            Key::End => edit.cursor = edit.text.len(),
            Key::Up => self.older(edit),
            Key::Down => self.newer(edit),
            Key::KillToEnd => edit.text.truncate(edit.cursor),
            Key::KillToStart => {
                edit.text.drain(..edit.cursor);
                edit.cursor = 0;
            }
            Key::KillWord => {
                let before = &edit.text[..edit.cursor];
                let word_end = before
                    .iter()
                    .rposition(|c| !c.is_whitespace())
                    .map_or(0, |i| i + 1);
                let word_start = before[..word_end]
                    .iter()
                    .rposition(|c| c.is_whitespace())
                    .map_or(0, |i| i + 1);
                edit.text.drain(word_start..edit.cursor);
                edit.cursor = word_start;
            }
            Key::ClearScreen => terminal.write("\x1b[2J\x1b[3J\x1b[H")?,
            Key::Tab => Self::complete(edit, terminal, prompt, complete)?,
            Key::Ignored => {}
        }
        Ok(None)
    }

    fn older(&self, edit: &mut Edit) {
        let index = match edit.history_index {
            Some(0) => return,
            Some(index) => index - 1,
            None if self.history.is_empty() => return,
            None => {
                edit.stashed = std::mem::take(&mut edit.text);
                self.history.len() - 1
            }
        };
        edit.history_index = Some(index);
        edit.set(&self.history[index]);
    }

    fn newer(&self, edit: &mut Edit) {
        let Some(index) = edit.history_index else {
            return;
        };
        if index + 1 < self.history.len() {
            edit.history_index = Some(index + 1);
            edit.set(&self.history[index + 1]);
        } else {
            edit.history_index = None;
            edit.text = std::mem::take(&mut edit.stashed);
            edit.cursor = edit.text.len();
        }
    }

    /// Tab: extends the name before the cursor as far as every candidate agrees, or else lists
    /// the candidates.
    fn complete(
        edit: &mut Edit,
        terminal: &mut dyn Terminal,
        prompt: &str,
        complete: &mut dyn FnMut(&str, usize) -> Completion,
    ) -> io::Result<()> {
        let line = edit.line();
        let cursor: usize = edit.text[..edit.cursor].iter().map(|c| c.len_utf8()).sum();
        let completion = complete(&line, cursor);
        let Some(first) = completion.candidates.first() else {
            return terminal.write("\x07");
        };
        let start = line[..completion.start.min(cursor)].chars().count();
        let common = completion
            .candidates
            .iter()
            .fold(first.as_str(), |common, candidate| {
                common_prefix(common, candidate)
            });
        if common.chars().count() > edit.cursor - start {
            edit.replace(start, common);
        } else if completion.candidates.len() > 1 {
            terminal.write(&listing(&completion.candidates))?;
            terminal.write(&render(prompt, edit))?;
        }
        Ok(())
    }
}

const ESCAPE: u8 = 0x1b;

/// The first key in `bytes`, which is not empty, and how many bytes it takes.
fn decode(bytes: &[u8]) -> Decoded {
    let key = match bytes[0] {
        b'\r' | b'\n' => Key::Enter,
        b'\t' => Key::Tab,
        0x01 => Key::Home,
        0x02 => Key::Left,
        0x03 => Key::Interrupt,
        0x04 => Key::EndOfInput,
        0x05 => Key::End,
        0x06 => Key::Right,
        0x08 | 0x7f => Key::Backspace,
        0x0b => Key::KillToEnd,
        0x0c => Key::ClearScreen,
        0x0e => Key::Down,
        0x10 => Key::Up,
        0x15 => Key::KillToStart,
        0x17 => Key::KillWord,
        ESCAPE => return decode_escape(bytes),
        byte if byte < 0x20 => Key::Ignored,
        _ => return decode_char(bytes),
    };
    Decoded::Key(key, 1)
}

/// An escape sequence: `ESC [ <parameters> <final>` or `ESC O <final>`, or Alt with a key.
fn decode_escape(bytes: &[u8]) -> Decoded {
    match bytes.get(1) {
        None => Decoded::Partial,
        Some(b'[' | b'O') => {
            let Some(length) = bytes[2..].iter().position(|b| (0x40..=0x7e).contains(b)) else {
                return Decoded::Partial;
            };
            let parameters = &bytes[2..2 + length];
            let key = match (parameters, bytes[2 + length]) {
                (_, b'A') => Key::Up,
                (_, b'B') => Key::Down,
                (_, b'C') => Key::Right,
                (_, b'D') => Key::Left,
                (_, b'H') | (b"1" | b"7", b'~') => Key::Home,
                (_, b'F') | (b"4" | b"8", b'~') => Key::End,
                (b"3", b'~') => Key::Delete,
                _ => Key::Ignored,
            };
            Decoded::Key(key, 3 + length)
        }
        // Alt with a key, which nothing binds.
        Some(_) => Decoded::Key(Key::Ignored, 2),
    }
}

/// A UTF-8 character; U+FFFD for each byte of an invalid sequence.
fn decode_char(bytes: &[u8]) -> Decoded {
    let length = match bytes[0] {
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf4 => 4,
        0x00..=0x7f => 1,
        _ => return Decoded::Key(Key::Char(char::REPLACEMENT_CHARACTER), 1),
    };
    let Some(sequence) = bytes.get(..length) else {
        // Wait for the rest, unless what has arrived is already not a sequence's start.
        return match std::str::from_utf8(bytes) {
            Err(e) if e.error_len().is_some() => {
                Decoded::Key(Key::Char(char::REPLACEMENT_CHARACTER), 1)
            }
            _ => Decoded::Partial,
        };
    };
    match std::str::from_utf8(sequence)
        .ok()
        .and_then(|s| s.chars().next())
    {
        Some(c) => Decoded::Key(Key::Char(c), length),
        None => Decoded::Key(Key::Char(char::REPLACEMENT_CHARACTER), 1),
    }
}

/// The line drawn over the terminal's current line, with the cursor in place.
fn render(prompt: &str, edit: &Edit) -> String {
    let text = edit.line();
    let back = edit.text.len() - edit.cursor;
    if back == 0 {
        format!("\r{prompt}{text}\x1b[K")
    } else {
        format!("\r{prompt}{text}\x1b[K\x1b[{back}D")
    }
}

/// The candidates in columns, on the lines below the edited line.
fn listing(candidates: &[String]) -> String {
    let width = candidates
        .iter()
        .map(|c| c.chars().count())
        .max()
        .unwrap_or(0)
        + 2;
    let per_row = (LISTING_WIDTH / width).max(1);
    let mut listing = String::from("\r\n");
    for row in candidates.chunks(per_row) {
        let cells: Vec<String> = row.iter().map(|c| format!("{c:width$}")).collect();
        listing.push_str(cells.concat().trim_end());
        listing.push_str("\r\n");
    }
    listing
}

fn common_prefix<'a>(a: &'a str, b: &str) -> &'a str {
    let length = a
        .char_indices()
        .zip(b.chars())
        .find(|((_, x), y)| x != y)
        .map_or_else(|| a.len().min(b.len()), |((i, _), _)| i);
    &a[..length]
}

#[cfg(test)]
#[path = "editor-test.rs"]
mod tests;
