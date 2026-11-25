//! The compatibility flags as `compatibility-date.capnp` annotates them, read once from the
//! schema compiled into the `compatibility_date_capnp` crate.

use std::collections::HashMap;
use std::sync::LazyLock;

use capnp::introspect::Introspect;
use capnp::introspect::TypeVariant;
use capnp::schema::Field;
use capnp::schema::StructSchema;
use capnp::schema_capnp::value;
use compatibility_date_capnp::compatibility_flags;
use compatibility_date_capnp::implied_by_after_date;

use crate::date::CompatDate;

/// Why the schema's annotations could not be read. Each one is a mistake in
/// `compatibility-date.capnp` that its tests catch.
#[derive(Debug, Clone, thiserror::Error)]
pub enum SchemaError {
    #[error("reading the CompatibilityFlags schema: {0}")]
    Capnp(String),
    #[error("field {field} has an annotation of the wrong type")]
    AnnotationType { field: &'static str },
    #[error("field {field} has an invalid date {date:?}")]
    Date {
        field: &'static str,
        date: &'static str,
    },
    #[error("field {field} is implied by {other}, which is not a field")]
    NoSuchField {
        field: &'static str,
        other: &'static str,
    },
    #[error("field {field} is $neededByFl but has no $compatEnableFlag")]
    NeededByFlWithoutFlag { field: &'static str },
    #[error("CompatibilityFlags has no field {0}")]
    MissingField(&'static str),
    #[error("field {field} has more than one ${annotation}")]
    RepeatedFlag {
        field: &'static str,
        annotation: &'static str,
    },
}

impl From<capnp::Error> for SchemaError {
    fn from(error: capnp::Error) -> Self {
        Self::Capnp(error.to_string())
    }
}

impl From<capnp::NotInSchema> for SchemaError {
    fn from(error: capnp::NotInSchema) -> Self {
        Self::Capnp(error.to_string())
    }
}

impl From<std::str::Utf8Error> for SchemaError {
    fn from(error: std::str::Utf8Error) -> Self {
        Self::Capnp(error.to_string())
    }
}

/// How a field's compatibility date turns it on, one per `$compatEnableDate` or
/// `$compatEnableAllDates`, in the order the schema gives them.
#[derive(Debug, Clone, Copy)]
pub enum DateRule {
    OnOrAfter(CompatDate),
    AllDates,
}

/// One `$impliedByAfterDate`: the field is on if any of `others` is, for compatibility dates on
/// or after `date`.
#[derive(Debug, Clone)]
pub struct ImpliedBy {
    pub date: CompatDate,
    /// Field indices, in the order the annotation names them.
    pub others: Vec<u16>,
}

/// A field of `CompatibilityFlags` and what its annotations say.
#[derive(Debug, Clone)]
pub struct FlagInfo {
    /// The field's index (code order).
    pub index: u16,
    pub name: &'static str,
    pub enable_flag: Option<&'static str>,
    pub disable_flag: Option<&'static str>,
    /// The field's flags in the order the schema gives them, which is the order the compiler
    /// matches them to the configuration's flags.
    pub flags_in_match_order: Vec<(&'static str, FlagKind)>,
    pub date_rules: Vec<DateRule>,
    pub experimental: bool,
    pub needed_by_fl: bool,
    pub implied_by: Vec<ImpliedBy>,
}

impl FlagInfo {
    /// The last `$compatEnableDate` date, which a warning about a redundant flag names.
    pub fn enable_date(&self) -> Option<CompatDate> {
        self.date_rules.iter().rev().find_map(|rule| match rule {
            DateRule::OnOrAfter(date) => Some(*date),
            DateRule::AllDates => None,
        })
    }

    /// Whether the compatibility date alone turns the field on. The last rule decides, as each
    /// annotation the C++ compiler read overwrote the previous one's answer.
    pub fn enabled_by_date(&self, date: CompatDate) -> bool {
        self.date_rules.last().is_some_and(|rule| match rule {
            DateRule::OnOrAfter(on) => date >= *on,
            DateRule::AllDates => true,
        })
    }
}

/// Every field of `CompatibilityFlags`, in code order, so `fields[i]` is the field of index `i`.
///
/// It holds plain data, since the schema's readers are neither `Send` nor `Sync`; `schema()`
/// returns the schema for reflection.
#[derive(Debug)]
pub struct FlagTable {
    pub fields: Vec<FlagInfo>,
    /// The slot (see `slot`) of each flag name. A name the schema gives more than once maps to
    /// the slot the compiler matches first, as only that one can ever match it.
    pub slots: HashMap<&'static [u8], usize>,
    pub auto_inject_python_workers: u16,
    pub python_workers: u16,
}

/// Which of a field's two flags a slot is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlagKind {
    Enable,
    Disable,
}

/// Numbers each field's enable and disable flags, from 0 to `2 * fields.len()`.
pub fn slot(field: usize, kind: FlagKind) -> usize {
    2 * field + usize::from(kind == FlagKind::Disable)
}

static TABLE: LazyLock<Result<FlagTable, SchemaError>> = LazyLock::new(FlagTable::read);

impl FlagTable {
    /// Returns the table, which is read from the schema the first time.
    ///
    /// # Errors
    ///
    /// Returns the `SchemaError` that reading the schema met.
    pub fn get() -> Result<&'static Self, SchemaError> {
        TABLE.as_ref().map_err(Clone::clone)
    }

    pub fn schema() -> StructSchema {
        match compatibility_flags::Owned::introspect().which() {
            TypeVariant::Struct(raw) => StructSchema::new(raw),
            _ => unreachable!("CompatibilityFlags is a struct"),
        }
    }

    fn read() -> Result<Self, SchemaError> {
        let schema = Self::schema();
        let mut fields = Vec::new();
        for field in schema.get_fields()? {
            fields.push(read_field(schema, field)?);
        }
        let index_of = |name: &'static str| {
            schema
                .find_field_by_name(name)?
                .map(|f| f.get_index())
                .ok_or(SchemaError::MissingField(name))
        };
        let mut slots = HashMap::new();
        for (index, info) in fields.iter().enumerate() {
            for &(name, kind) in &info.flags_in_match_order {
                slots
                    .entry(name.as_bytes())
                    .or_insert_with(|| slot(index, kind));
            }
        }
        Ok(Self {
            slots,
            auto_inject_python_workers: index_of("autoInjectPythonWorkers")?,
            python_workers: index_of("pythonWorkers")?,
            fields,
        })
    }
}

fn text(value: value::Reader<'static>, field: &'static str) -> Result<&'static str, SchemaError> {
    match value.which()? {
        value::Text(t) => Ok(t?.to_str()?),
        _ => Err(SchemaError::AnnotationType { field }),
    }
}

fn date(text: &'static str, field: &'static str) -> Result<CompatDate, SchemaError> {
    CompatDate::parse(text).ok_or(SchemaError::Date { field, date: text })
}

/// Sets a field's enable or disable flag, which the schema may give only once: the compiler
/// matches one name of each to the configuration's flags.
fn set_flag(
    flag: &mut Option<&'static str>,
    value: &'static str,
    field: &'static str,
    annotation: &'static str,
) -> Result<(), SchemaError> {
    if flag.replace(value).is_some() {
        return Err(SchemaError::RepeatedFlag { field, annotation });
    }
    Ok(())
}

fn read_field(schema: StructSchema, field: Field) -> Result<FlagInfo, SchemaError> {
    let proto = field.get_proto();
    let name = proto.get_name()?.to_str()?;
    let mut info = FlagInfo {
        index: field.get_index(),
        name,
        enable_flag: None,
        disable_flag: None,
        flags_in_match_order: Vec::new(),
        date_rules: Vec::new(),
        experimental: false,
        needed_by_fl: false,
        implied_by: Vec::new(),
    };

    for annotation in proto.get_annotations()? {
        let value = annotation.get_value()?;
        match annotation.get_id() {
            compatibility_flags::compat_enable_flag::ID => {
                let flag = text(value, name)?;
                set_flag(&mut info.enable_flag, flag, name, "compatEnableFlag")?;
                info.flags_in_match_order.push((flag, FlagKind::Enable));
            }
            compatibility_flags::compat_disable_flag::ID => {
                let flag = text(value, name)?;
                set_flag(&mut info.disable_flag, flag, name, "compatDisableFlag")?;
                info.flags_in_match_order.push((flag, FlagKind::Disable));
            }
            compatibility_flags::compat_enable_date::ID => {
                let on = date(text(value, name)?, name)?;
                info.date_rules.push(DateRule::OnOrAfter(on));
            }
            compatibility_flags::compat_enable_all_dates::ID => {
                info.date_rules.push(DateRule::AllDates);
            }
            compatibility_flags::experimental::ID => info.experimental = true,
            compatibility_flags::needed_by_fl::ID => info.needed_by_fl = true,
            compatibility_flags::implied_by_after_date::ID => {
                let value::Struct(any) = value.which()? else {
                    return Err(SchemaError::AnnotationType { field: name });
                };
                let implied: implied_by_after_date::Reader<'static> = any.get_as()?;
                let on = date(implied.get_date()?.to_str()?, name)?;
                let mut others = Vec::new();
                let mut add = |other: &'static str| -> Result<(), SchemaError> {
                    let other_field = schema
                        .find_field_by_name(other)?
                        .ok_or(SchemaError::NoSuchField { field: name, other })?;
                    others.push(other_field.get_index());
                    Ok(())
                };
                match implied.which()? {
                    implied_by_after_date::Name(other) => add(other?.to_str()?)?,
                    implied_by_after_date::Names(list) => {
                        for other in list? {
                            add(other?.to_str()?)?;
                        }
                    }
                }
                info.implied_by.push(ImpliedBy { date: on, others });
            }
            _ => {}
        }
    }

    if info.needed_by_fl && info.enable_flag.is_none() {
        return Err(SchemaError::NeededByFlWithoutFlag { field: name });
    }
    Ok(info)
}

#[cfg(test)]
#[path = "table-test.rs"]
mod tests;
