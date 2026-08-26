use std::borrow::Cow;
use std::error::Error as StdError;
use std::fmt::Display;
use std::fmt::{self};
use std::io::Write;
use std::io::{self};
use std::ops::Range;
use std::path::Path;
use std::path::PathBuf;
use std::process;
use std::str::Utf8Error;

use codespan_reporting::diagnostic::Diagnostic;
use codespan_reporting::diagnostic::Label;
use codespan_reporting::files::SimpleFiles;
use codespan_reporting::term::Config;
use codespan_reporting::term::termcolor::ColorChoice;
use codespan_reporting::term::termcolor::StandardStream;
use codespan_reporting::term::termcolor::WriteColor;
use codespan_reporting::term::{self};

use crate::fs;
use crate::syntax;

pub type Result<T, E = Error> = std::result::Result<T, E>;

#[derive(Debug)]
pub enum Error {
    NoBridgeMod,
    Fs(fs::Error),
    Utf8(PathBuf, Utf8Error),
    Syn(syn::Error),
}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::NoBridgeMod => write!(f, "no #[cxx::bridge] module found"),
            Self::Fs(err) => err.fmt(f),
            Self::Utf8(path, _) => write!(f, "Failed to read file `{}`", path.display()),
            Self::Syn(err) => err.fmt(f),
        }
    }
}

impl StdError for Error {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            Self::Fs(err) => err.source(),
            Self::Utf8(_, err) => Some(err),
            Self::Syn(err) => err.source(),
            Self::NoBridgeMod => None,
        }
    }
}

impl From<fs::Error> for Error {
    fn from(err: fs::Error) -> Self {
        Self::Fs(err)
    }
}

impl From<syn::Error> for Error {
    fn from(err: syn::Error) -> Self {
        Self::Syn(err)
    }
}

pub fn format_err(path: &Path, source: &str, error: Error) -> ! {
    match error {
        Error::Syn(syn_error) => {
            let syn_error = sort_syn_errors(syn_error);
            let writer = StandardStream::stderr(ColorChoice::Auto);
            let ref mut stderr = writer.lock();
            for error in syn_error {
                let _ = writeln!(stderr);
                display_syn_error(stderr, path, source, error);
            }
        }
        Error::NoBridgeMod => {
            let _ = writeln!(
                io::stderr(),
                "cxxbridge: no #[cxx::bridge] module found in {}",
                path.display(),
            );
        }
        _ => {
            let _ = writeln!(io::stderr(), "cxxbridge: {}", report(error));
        }
    }
    process::exit(1);
}

pub fn report(error: impl StdError) -> impl Display {
    struct Report<E>(E);

    impl<E: StdError> Display for Report<E> {
        fn fmt(&self, formatter: &mut fmt::Formatter) -> fmt::Result {
            write!(formatter, "{}", self.0)?;
            let mut error: &dyn StdError = &self.0;

            while let Some(cause) = error.source() {
                write!(formatter, "\n\nCaused by:\n    {}", cause)?;
                error = cause;
            }

            Ok(())
        }
    }

    Report(error)
}

fn sort_syn_errors(error: syn::Error) -> Vec<syn::Error> {
    let mut errors: Vec<_> = error.into_iter().collect();
    errors.sort_by_key(|e| {
        let start = e.span().start();
        (start.line, start.column)
    });
    errors
}

fn display_syn_error(stderr: &mut dyn WriteColor, path: &Path, source: &str, error: syn::Error) {
    let span = error.span();
    let start = span.start();
    let end = span.end();

    let mut start_offset = 0;
    for _ in 1..start.line {
        let Some(newline) = source[start_offset..].find('\n') else {
            start_offset = source.len();
            break;
        };
        start_offset += newline + 1;
    }
    let start_column = source[start_offset..]
        .chars()
        .take(start.column)
        .map(char::len_utf8)
        .sum::<usize>();
    start_offset += start_column;

    let mut end_offset = start_offset;
    if start.line == end.line {
        end_offset -= start_column;
    } else {
        for _ in 0..end.line - start.line {
            let Some(newline) = source[end_offset..].find('\n') else {
                end_offset = source.len();
                break;
            };
            end_offset += newline + 1;
        }
    }
    end_offset += source[end_offset..]
        .chars()
        .take(end.column)
        .map(char::len_utf8)
        .sum::<usize>();

    let mut path = path.to_string_lossy();
    if path == "-" {
        path = Cow::Borrowed(if cfg!(unix) { "/dev/stdin" } else { "stdin" });
    }

    let mut files = SimpleFiles::new();
    let file = files.add(path, source);

    let diagnostic = diagnose(file, start_offset..end_offset, error);

    let config = Config::default();
    let _ = term::emit_to_write_style(stderr, &config, &files, &diagnostic);
}

fn diagnose(file: usize, range: Range<usize>, error: syn::Error) -> Diagnostic<usize> {
    let message = error.to_string();
    let info = syntax::error::ERRORS
        .iter()
        .find(|e| message.contains(e.msg));
    let mut diagnostic = Diagnostic::error().with_message(&message);
    let mut label = Label::primary(file, range);
    if let Some(info) = info {
        label.message = info.label.map_or(message, str::to_owned);
        diagnostic.labels.push(label);
        diagnostic.notes.extend(info.note.map(str::to_owned));
    } else {
        label.message = message;
        diagnostic.labels.push(label);
    }
    diagnostic.code = Some("cxxbridge".to_owned());
    diagnostic
}
