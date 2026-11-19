//! Compiling a compatibility date and a list of flags into `CompatibilityFlags`, and back.

use capnp::dynamic_struct;
use capnp::dynamic_value;
use compatibility_date_capnp::compatibility_flags;

use crate::date::CompatDate;
use crate::table::FlagInfo;
use crate::table::FlagTable;
use crate::table::SchemaError;

/// The dates a compatibility date may name.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DateValidation {
    /// Dates up through the earlier of today and the newest date this binary supports,
    /// `MAXIMUM_COMPATIBILITY_DATE`.
    CodeVersion,
    /// Dates up through today. Only Cloudflare uses this: if a flag's default-on date passes
    /// before Cloudflare deploys the flag, the date moves back.
    CurrentDateForCloudflare,
    /// Any date. Only for testing the compiler itself and for test variants that use the
    /// newest date.
    FutureForTest,
}

/// What `compile` is allowed to accept.
#[derive(Debug, Clone, Copy)]
pub struct CompileOptions<'a> {
    pub date_validation: DateValidation,
    /// Whether every `$experimental` flag may be named.
    pub allow_experimental: bool,
    /// The `$experimental` flags that may be named even when `allow_experimental` is false.
    pub allowed_experimental_flags: &'a [&'a [u8]],
    /// Whether the main module is a Python module, which turns on `pythonWorkers` when
    /// `autoInjectPythonWorkers` is on.
    pub main_module_is_python: bool,
}

/// A problem with the configuration, in the order the compiler found it.
///
/// A message quotes the configuration's text byte for byte, so it is UTF-8 only if that is.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Diagnostic {
    /// The configuration is invalid.
    Error(Vec<u8>),
    /// The configuration is valid, but the developer ought to clean it up.
    Warning(Vec<u8>),
}

/// The flags a configuration compiles to.
#[derive(Debug, Clone)]
pub struct Compiled {
    /// Indexed by field index (code order).
    pub enabled: Vec<bool>,
    pub diagnostics: Vec<Diagnostic>,
}

impl Compiled {
    /// Writes every flag into `output`.
    ///
    /// # Errors
    ///
    /// Fails if `output` has fewer fields than this binary's schema, which only a builder for
    /// another schema could.
    pub fn write_to(&self, output: compatibility_flags::Builder<'_>) -> capnp::Result<()> {
        let table = FlagTable::get().map_err(|e| capnp::Error::failed(e.to_string()))?;
        let fields = FlagTable::schema().get_fields()?;
        let mut output: dynamic_struct::Builder<'_> =
            dynamic_value::Builder::from(output).downcast();
        for (info, &on) in table.fields.iter().zip(&self.enabled) {
            output.set(fields.get(info.index), dynamic_value::Reader::Bool(on))?;
        }
        Ok(())
    }

    pub fn errors(&self) -> impl Iterator<Item = &[u8]> {
        self.diagnostics.iter().filter_map(|d| match d {
            Diagnostic::Error(e) => Some(e.as_slice()),
            Diagnostic::Warning(_) => None,
        })
    }

    pub fn warnings(&self) -> impl Iterator<Item = &[u8]> {
        self.diagnostics.iter().filter_map(|d| match d {
            Diagnostic::Warning(w) => Some(w.as_slice()),
            Diagnostic::Error(_) => None,
        })
    }
}

/// The flags a configuration names that the compiler has not yet matched to a field.
///
/// Diagnostics name the flags left at the end in the order they are left, which is the order
/// C++'s `kj::HashSet` left them in: matching a flag moves the last one into its place.
struct PendingFlags<'a> {
    flags: Vec<&'a [u8]>,
}

impl<'a> PendingFlags<'a> {
    fn new(flags: impl IntoIterator<Item = &'a [u8]>, diagnostics: &mut Vec<Diagnostic>) -> Self {
        let mut unique: Vec<&[u8]> = Vec::new();
        for flag in flags {
            if unique.contains(&flag) {
                diagnostics.push(Diagnostic::Error(
                    [
                        b"Compatibility flag specified multiple times: ".as_slice(),
                        flag,
                    ]
                    .concat(),
                ));
            } else {
                unique.push(flag);
            }
        }
        Self { flags: unique }
    }

    fn take(&mut self, flag: &str) -> bool {
        if let Some(pos) = self.flags.iter().position(|f| *f == flag.as_bytes()) {
            self.flags.swap_remove(pos);
            true
        } else {
            false
        }
    }
}

/// Reports a date that `validation` does not allow.
fn validate_date(
    date: CompatDate,
    validation: DateValidation,
    today: CompatDate,
    maximum: CompatDate,
    diagnostics: &mut Vec<Diagnostic>,
) {
    match validation {
        DateValidation::CodeVersion => {
            if maximum < date {
                diagnostics.push(Diagnostic::Error(
                    format!(
                        "This Worker requires compatibility date \"{date}\", but the newest date \
                     supported by this server binary is \"{maximum}\"."
                    )
                    .into_bytes(),
                ));
            }
            // workerd is built with MAXIMUM_COMPATIBILITY_DATE a little in the future, so that
            // a build can support today's date until the next release, but a Worker should
            // still not set a date in the future.
            if today < date {
                diagnostics.push(Diagnostic::Error(format!(
                    "Can't set compatibility date in the future: \"{date}\". Today's date (UTC) \
                     is \"{today}\"."
                ).into_bytes()));
            }
        }
        DateValidation::CurrentDateForCloudflare => {
            if today < date {
                diagnostics.push(Diagnostic::Error(
                    format!("Can't set compatibility date in the future: {date}").into_bytes(),
                ));
            }
        }
        DateValidation::FutureForTest => {}
    }
}

/// How a field's flags and the date decided it.
#[derive(Clone, Copy)]
struct FieldState {
    enable_by_flag: bool,
    disable_by_flag: bool,
    enable_by_date: bool,
}

/// Reports the problems with how the configuration names one field's flags.
fn report_field(
    info: &FlagInfo,
    state: FieldState,
    options: &CompileOptions<'_>,
    diagnostics: &mut Vec<Diagnostic>,
) {
    let FieldState {
        enable_by_flag,
        disable_by_flag,
        enable_by_date,
    } = state;
    let enable_flag = info.enable_flag.unwrap_or_default();
    if enable_by_flag && disable_by_flag {
        diagnostics.push(Diagnostic::Error(
            format!(
                "Compatibility flags are mutually contradictory: {enable_flag} vs {}",
                info.disable_flag.unwrap_or_default()
            )
            .into_bytes(),
        ));
    }
    // Naming a flag the date already enables is redundant but harmless. Tests stay quiet
    // about it: they name flags for the default variant, which uses an old date, while the
    // all-compat-flags variant enables every flag by date.
    if enable_by_flag && enable_by_date && options.date_validation != DateValidation::FutureForTest
    {
        diagnostics.push(Diagnostic::Warning(
            match info.enable_date() {
                Some(on) => format!(
                    "The compatibility flag {enable_flag} became the default as of {on} so does \
                 not need to be specified anymore."
                ),
                None => format!(
                    "The compatibility flag {enable_flag} is the default, so does not need to be \
                 specified anymore."
                ),
            }
            .into_bytes(),
        ));
    }
    // A disable flag the date makes redundant is fine: it won't be redundant at a later
    // date, and someone may set it early so as not to forget.
    if enable_by_flag
        && info.experimental
        && !options.allow_experimental
        && !options
            .allowed_experimental_flags
            .contains(&enable_flag.as_bytes())
    {
        diagnostics.push(Diagnostic::Error(
            if options.date_validation == DateValidation::CurrentDateForCloudflare {
                format!(
                    "The compatibility flag {enable_flag} is experimental and cannot yet be \
                     used in Workers deployed to Cloudflare."
                )
            } else {
                format!(
                    "The compatibility flag {enable_flag} is experimental and may break or be \
                     removed in a future version of workerd. To use this flag, you must pass \
                     --experimental on the command line."
                )
            }
            .into_bytes(),
        ));
    }
}

/// Compiles a compatibility date and a list of flags.
///
/// # Errors
///
/// Fails only if the schema's annotations could not be read; problems with the configuration
/// are diagnostics.
///
/// Text is bytes, which need not be UTF-8, as C++ hands it over; diagnostics quote it as is.
pub fn compile<'a>(
    compat_date: &[u8],
    flags: impl IntoIterator<Item = &'a [u8]>,
    options: &CompileOptions<'_>,
) -> Result<Compiled, SchemaError> {
    compile_with_today(
        compat_date,
        flags,
        options,
        CompatDate::today(),
        maximum_compat_date(),
    )
}

pub(crate) fn compile_with_today<'a>(
    compat_date: &[u8],
    flags: impl IntoIterator<Item = &'a [u8]>,
    options: &CompileOptions<'_>,
    today: CompatDate,
    maximum: CompatDate,
) -> Result<Compiled, SchemaError> {
    let table = FlagTable::get()?;
    let mut diagnostics = Vec::new();
    let mut pending = PendingFlags::new(flags, &mut diagnostics);

    let date = CompatDate::parse_bytes(compat_date).unwrap_or_else(|| {
        diagnostics.push(Diagnostic::Error(
            [b"Invalid compatibility date: ".as_slice(), compat_date].concat(),
        ));
        // The C++ compiler went on with this date, so the flags compile the same.
        CompatDate::new(2021, 5, 1).unwrap_or_else(|| unreachable!())
    });

    validate_date(
        date,
        options.date_validation,
        today,
        maximum,
        &mut diagnostics,
    );

    let mut enabled = vec![false; table.fields.len()];
    // (field, other): `field` is on if `other` is.
    let mut implied: Vec<(usize, u16)> = Vec::new();

    for (index, info) in table.fields.iter().enumerate() {
        let mut take = |flag: Option<&str>| flag.is_some_and(|f| pending.take(f));
        let (enable_by_flag, disable_by_flag) = if info.disable_flag_first {
            let disable = take(info.disable_flag);
            (take(info.enable_flag), disable)
        } else {
            let enable = take(info.enable_flag);
            (enable, take(info.disable_flag))
        };
        let enable_by_date = info.enabled_by_date(date);

        if !disable_by_flag {
            for rule in &info.implied_by {
                if date >= rule.date {
                    implied.extend(rule.others.iter().map(|&other| (index, other)));
                }
            }
        }

        report_field(
            info,
            FieldState {
                enable_by_flag,
                disable_by_flag,
                enable_by_date,
            },
            options,
            &mut diagnostics,
        );

        enabled[index] = enable_by_flag || (enable_by_date && !disable_by_flag);
    }

    // Flags implied by pythonWorkers need it set first.
    if options.main_module_is_python && enabled[usize::from(table.auto_inject_python_workers)] {
        enabled[usize::from(table.python_workers)] = true;
    }

    // In order, so that a flag implied by an implied flag is on when its implier comes first.
    for (field, other) in implied {
        if enabled[usize::from(other)] {
            enabled[field] = true;
        }
    }

    for flag in pending.flags {
        diagnostics.push(Diagnostic::Error(
            [b"No such compatibility flag: ".as_slice(), flag].concat(),
        ));
    }

    Ok(Compiled {
        enabled,
        diagnostics,
    })
}

/// Which set flags `decompile` names.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decompile {
    /// Every set flag that has an enable flag.
    All,
    /// Only the set flags annotated `$neededByFl`.
    NeededByFl,
}

/// Returns the enable flags of the set fields, in code order. The names live in the schema,
/// which is static.
///
/// # Errors
///
/// Fails if the schema could not be read or `input` could not be.
pub fn decompile(
    input: compatibility_flags::Reader<'_>,
    which: Decompile,
) -> Result<Vec<&'static str>, SchemaError> {
    let table = FlagTable::get()?;
    Ok(decompile_fields(input, which)?
        .into_iter()
        .filter_map(|index| table.fields[usize::from(index)].enable_flag)
        .collect())
}

/// Like `decompile`, but returns the indices of the fields.
///
/// # Errors
///
/// Fails if the schema could not be read or `input` could not be.
pub fn decompile_fields(
    input: compatibility_flags::Reader<'_>,
    which: Decompile,
) -> Result<Vec<u16>, SchemaError> {
    let table = FlagTable::get()?;
    let schema_fields = FlagTable::schema().get_fields()?;
    let input: dynamic_struct::Reader<'_> = dynamic_value::Reader::from(input).downcast();
    let mut fields = Vec::new();
    for info in &table.fields {
        let wanted = match which {
            Decompile::All => info.enable_flag.is_some(),
            Decompile::NeededByFl => info.needed_by_fl,
        };
        if wanted
            && matches!(
                input.get(schema_fields.get(info.index))?,
                dynamic_value::Reader::Bool(true)
            )
        {
            fields.push(info.index);
        }
    }
    Ok(fields)
}

/// The contents of `maximum-compatibility-date.txt`, which `BUILD.bazel` generates as a literal.
const MAXIMUM_COMPATIBILITY_DATE: &str = include!("maximum_date.rs");

/// The newest compatibility date this binary supports.
pub fn maximum_compat_date() -> CompatDate {
    CompatDate::parse(MAXIMUM_COMPATIBILITY_DATE).unwrap_or_else(|| {
        unreachable!("maximum-compatibility-date.txt holds a valid date, as its test checks")
    })
}

/// The newest date a Worker may set under `DateValidation::CodeVersion`: the earlier of today
/// and `maximum_compat_date()`.
pub fn newest_compat_date() -> CompatDate {
    CompatDate::today().min(maximum_compat_date())
}

#[cfg(test)]
#[path = "compile-test.rs"]
mod tests;
