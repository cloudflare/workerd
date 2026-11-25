use super::*;

#[test]
fn reads_the_schema() {
    let table = FlagTable::get().unwrap();
    assert!(table.fields.len() > 100);
    for (i, info) in table.fields.iter().enumerate() {
        assert_eq!(usize::from(info.index), i, "{}", info.name);
    }
}

#[test]
fn reads_enable_and_disable_flags() {
    let table = FlagTable::get().unwrap();
    let info = table
        .fields
        .iter()
        .find(|f| f.name == "formDataParserSupportsFiles")
        .unwrap();
    assert_eq!(info.enable_flag, Some("formdata_parser_supports_files"));
    assert_eq!(
        info.disable_flag,
        Some("formdata_parser_converts_files_to_strings")
    );
    assert_eq!(info.enable_date(), CompatDate::new(2021, 11, 3));
}

#[test]
fn gives_a_flag_with_no_disable_flag_a_slot() {
    let table = FlagTable::get().unwrap();
    let info = table
        .fields
        .iter()
        .find(|f| f.name == "workerdExperimental")
        .unwrap();
    assert_eq!(info.enable_flag, Some("experimental"));
    assert_eq!(info.disable_flag, None);
    assert_eq!(
        table.slots.get(b"experimental".as_slice()),
        Some(&slot(usize::from(info.index), FlagKind::Enable))
    );
}

#[test]
fn gives_every_flag_a_slot() {
    let table = FlagTable::get().unwrap();
    for info in &table.fields {
        for flag in [info.enable_flag, info.disable_flag].into_iter().flatten() {
            assert!(table.slots.contains_key(flag.as_bytes()), "{flag}");
        }
    }
}

#[test]
fn reads_implied_by_after_date() {
    let table = FlagTable::get().unwrap();
    let info = table
        .fields
        .iter()
        .find(|f| f.name == "nodeJsCompatV2")
        .unwrap();
    let node_js_compat = table
        .fields
        .iter()
        .find(|f| f.name == "nodeJsCompat")
        .unwrap()
        .index;
    assert!(
        info.implied_by
            .iter()
            .any(|i| i.date == CompatDate::new(2024, 9, 23).unwrap()
                && i.others.contains(&node_js_compat))
    );
}

#[test]
fn reads_enable_all_dates() {
    let table = FlagTable::get().unwrap();
    let info = table
        .fields
        .iter()
        .find(|f| f.name == "r2PublicBetaApi")
        .unwrap();
    assert!(info.enabled_by_date(CompatDate::new(2000, 1, 1).unwrap()));
}

#[test]
fn rejects_a_repeated_flag() {
    let mut flag = None;
    assert!(set_flag(&mut flag, "first", "someField", "compatEnableFlag").is_ok());
    assert_eq!(flag, Some("first"));
    let error = set_flag(&mut flag, "second", "someField", "compatEnableFlag").unwrap_err();
    assert_eq!(
        error.to_string(),
        "field someField has more than one $compatEnableFlag"
    );
}
