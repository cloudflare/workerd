// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// Input in the reads it arrives in, then the end of the input; what the editor wrote.
struct FakeTerminal {
    reads: VecDeque<Vec<u8>>,
    output: String,
    /// The timeout of each read.
    timeouts: Vec<Option<Duration>>,
}

impl FakeTerminal {
    fn new(reads: &[&[u8]]) -> Self {
        Self {
            reads: reads.iter().map(|read| read.to_vec()).collect(),
            output: String::new(),
            timeouts: Vec::new(),
        }
    }
}

impl Terminal for FakeTerminal {
    fn read(&mut self, timeout: Option<Duration>) -> io::Result<Option<Vec<u8>>> {
        self.timeouts.push(timeout);
        Ok(self.reads.pop_front())
    }

    fn write(&mut self, output: &str) -> io::Result<()> {
        self.output.push_str(output);
        Ok(())
    }
}

fn no_completion(_: &str, _: usize) -> Completion {
    Completion {
        start: 0,
        candidates: Vec::new(),
    }
}

/// The lines `editor` reads from `reads` until the input ends.
fn lines(editor: &mut Editor, reads: &[&[u8]]) -> Vec<Line> {
    let mut terminal = FakeTerminal::new(reads);
    let mut lines = Vec::new();
    loop {
        let line = editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap();
        if line == Line::Ended {
            return lines;
        }
        lines.push(line);
    }
}

fn submitted(text: &str) -> Line {
    Line::Submitted(text.to_owned())
}

#[test]
fn reads_lines() {
    let mut editor = Editor::new();
    assert_eq!(
        lines(&mut editor, &[b"1 + 2\r", b"foo", b"()\r"]),
        vec![submitted("1 + 2"), submitted("foo()")]
    );
}

#[test]
fn keeps_what_follows_a_line_for_the_next() {
    let mut editor = Editor::new();
    assert_eq!(
        lines(&mut editor, &[b"a\rb\rc"]),
        vec![submitted("a"), submitted("b"), submitted("c")]
    );
}

#[test]
fn edits_within_the_line() {
    let mut editor = Editor::new();
    // Left twice, insert; Home, insert; End, Backspace; Delete at the start.
    assert_eq!(
        lines(
            &mut editor,
            &[b"abc\x1b[D\x1b[DX\x01Y\x05\x7f\x01\x1b[3~\r"]
        ),
        vec![submitted("aXb")]
    );
    // Ctrl+B and Ctrl+F, and the other spellings of Home and End.
    assert_eq!(
        lines(
            &mut editor,
            &[b"bc\x02\x02a\x06\x06d\x1b[1~0\x1b[4~e\x1bOH_\x1bOF.\r"]
        ),
        vec![submitted("_0abcde.")]
    );
}

#[test]
fn kills_text() {
    let mut editor = Editor::new();
    assert_eq!(
        lines(&mut editor, &[b"one two  three\x17\r"]),
        vec![submitted("one two  ")]
    );
    assert_eq!(
        lines(&mut editor, &[b"one two\x1b[D\x1b[D\x1b[D\x0b\r"]),
        vec![submitted("one ")]
    );
    assert_eq!(
        lines(&mut editor, &[b"one two\x1b[D\x1b[D\x1b[D\x15\r"]),
        vec![submitted("two")]
    );
}

#[test]
fn decodes_utf8_split_across_reads() {
    let mut editor = Editor::new();
    let pi = "π".as_bytes();
    assert_eq!(
        lines(&mut editor, &[&pi[..1], &pi[1..], b"\x1b[Dx\r"]),
        vec![submitted("xπ")]
    );
    // Invalid bytes are replaced, without losing what follows them.
    assert_eq!(
        lines(&mut editor, &[b"\xffa\xc3(\r"]),
        vec![submitted("\u{fffd}a\u{fffd}(")]
    );
}

#[test]
fn waits_briefly_for_an_escape_sequence() {
    let mut editor = Editor::new();
    let mut terminal = FakeTerminal::new(&[b"ab\x1b", b"[D", b"x\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap(),
        submitted("axb")
    );
    assert_eq!(terminal.timeouts, vec![None, Some(ESCAPE_TIMEOUT), None]);

    // An Escape with nothing after it is the Escape key, which does nothing.
    let mut terminal = FakeTerminal::new(&[b"a\x1b", b"", b"b\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap(),
        submitted("ab")
    );
}

#[test]
fn cancels_and_ends() {
    let mut editor = Editor::new();
    assert_eq!(
        lines(&mut editor, &[b"abc\x03", b"\x04"]),
        vec![Line::Canceled]
    );
    // Ctrl+D deletes when the line is not empty; the end of the input submits the line.
    assert_eq!(lines(&mut editor, &[b"ab\x01\x04"]), vec![submitted("b")]);
}

#[test]
fn browses_history() {
    let mut editor = Editor::new();
    assert!(editor.add_history("first"));
    assert!(editor.add_history("second"));
    // A repeat of the latest entry, or an empty line, is not added.
    assert!(!editor.add_history("second"));
    assert!(!editor.add_history(""));
    // Up past the oldest entry stays there; Down past the newest restores the edited line.
    assert_eq!(
        lines(
            &mut editor,
            &[
                b"\x1b[A\x1b[A\x1b[A\r",
                b"draft\x1b[A\x1b[B\x1b[B!\r",
                b"\x10\x0e\x10\r"
            ]
        ),
        vec![submitted("first"), submitted("draft!"), submitted("second")]
    );
}

#[test]
fn history_keeps_the_latest_entries() {
    let mut editor = Editor::new();
    for i in 0..=HISTORY_LIMIT {
        editor.add_history(&i.to_string());
    }
    assert_eq!(editor.history.len(), HISTORY_LIMIT);
    assert_eq!(editor.history.first().map(String::as_str), Some("1"));
    assert_eq!(editor.history.last(), Some(&HISTORY_LIMIT.to_string()));
}

#[test]
fn completes_names() {
    let mut editor = Editor::new();
    let mut calls = Vec::new();
    let mut complete = |line: &str, cursor: usize| {
        calls.push((line.to_owned(), cursor));
        let candidates: &[&str] = match &line[..cursor] {
            "π.fe" => &["fetch"],
            "w.s" => &["service", "socket"],
            _ => &[],
        };
        Completion {
            start: line[..cursor].rfind('.').map_or(0, |dot| dot + 1),
            candidates: candidates.iter().map(|&c| c.to_owned()).collect(),
        }
    };

    // One candidate completes; the cursor is a byte offset into the line.
    let mut terminal = FakeTerminal::new(&[b"\xcf\x80.fe()\x1b[D\x1b[D\t\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut complete)
            .unwrap(),
        submitted("π.fetch()")
    );

    // Several complete as far as they agree, and then list.
    let mut terminal = FakeTerminal::new(&[b"w.s\t\t\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut complete)
            .unwrap(),
        submitted("w.s")
    );
    assert!(terminal.output.contains("\r\nservice  socket\r\n"));

    // None rings the bell.
    let mut terminal = FakeTerminal::new(&[b"x\t\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut complete)
            .unwrap(),
        submitted("x")
    );
    assert!(terminal.output.contains('\x07'));

    assert_eq!(
        calls,
        vec![
            ("π.fe()".to_owned(), 5),
            ("w.s".to_owned(), 3),
            ("w.s".to_owned(), 3),
            ("x".to_owned(), 1),
        ]
    );
}

#[test]
fn renders_the_cursor() {
    let edit = Edit {
        text: "abc".chars().collect(),
        cursor: 1,
        ..Edit::default()
    };
    assert_eq!(render("> ", &edit), "\r> abc\x1b[K\x1b[2D");
    let edit = Edit {
        text: "abc".chars().collect(),
        cursor: 3,
        ..Edit::default()
    };
    assert_eq!(render("> ", &edit), "\r> abc\x1b[K");
}

#[test]
fn lists_in_columns() {
    let names: Vec<String> = (0..12).map(|i| format!("name{i:02}")).collect();
    // Eight characters a column, ten columns a row.
    let listing = listing(&names);
    let rows: Vec<&str> = listing.split("\r\n").collect();
    assert_eq!(rows.len(), 4);
    assert_eq!(rows[0], "");
    assert!(rows[1].starts_with("name00  name01  "));
    assert_eq!(rows[2], "name10  name11");
    assert_eq!(rows[3], "");
}

#[test]
fn common_prefixes() {
    assert_eq!(common_prefix("service", "socket"), "s");
    assert_eq!(common_prefix("fetch", "fetch"), "fetch");
    assert_eq!(common_prefix("fe", "fetch"), "fe");
    assert_eq!(common_prefix("πa", "πb"), "π");
    assert_eq!(common_prefix("a", "b"), "");
}

#[test]
fn takes_pasted_text_as_text() {
    let mut editor = Editor::new();
    let mut terminal = FakeTerminal::new(&[b"\x1b[200~if (a)\t{ b\x03\x04\x1b[A\x7f }\x1b[201~\r"]);
    let mut complete = |_: &str, _: usize| -> Completion {
        panic!("a pasted tab completed");
    };
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut complete)
            .unwrap(),
        submitted("if (a)\t{ b }")
    );

    // Keys after the paste are keys again.
    let mut terminal = FakeTerminal::new(&[b"\x1b[200~ab\x1b[201~\x02x\r"]);
    assert_eq!(
        editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap(),
        submitted("axb")
    );
}

#[test]
fn ends_a_line_at_each_pasted_line_break() {
    let mut editor = Editor::new();
    // A paste can span lines, and its end can come in a line of its own.
    assert_eq!(
        lines(
            &mut editor,
            &[
                b"x\x1b[200~one\r\ntwo\rthree\nfour",
                b"\r",
                b"\nfive\x1b[201~\r"
            ]
        ),
        vec![
            submitted("xone"),
            submitted("two"),
            submitted("three"),
            submitted("four"),
            submitted("five"),
        ]
    );
    // A CR at the end of a read waits for what follows, which may be an LF.
    let mut editor = Editor::new();
    let mut terminal = FakeTerminal::new(&[b"\x1b[200~a\r", b"\x1b[201~\r"]);
    let mut read = || {
        editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap()
    };
    assert_eq!(read(), submitted("a"));
    assert_eq!(read(), submitted(""));
}

#[test]
fn draws_each_line_it_ends() {
    // Lines that arrive at once, as a paste's do, are drawn although nothing is drawn between
    // their keys.
    let mut editor = Editor::new();
    let mut terminal = FakeTerminal::new(&[b"ab\x02\rc\x03"]);
    for _ in 0..2 {
        editor
            .read_line(&mut terminal, "> ", &mut no_completion)
            .unwrap();
    }
    // The first prompt is drawn before any input arrives.
    assert_eq!(
        terminal.output,
        "\r> \x1b[K\r> ab\x1b[K\r\n\r> c\x1b[K^C\r\n"
    );
}
