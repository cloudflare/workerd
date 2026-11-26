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

#[test]
fn maps_every_flag_to_its_slot() {
    let table = FlagTable::get().unwrap();
    let mut expected = HashMap::new();
    for (index, info) in table.fields.iter().enumerate() {
        for (flag, kind) in [
            (info.enable_flag, FlagKind::Enable),
            (info.disable_flag, FlagKind::Disable),
        ] {
            if let Some(flag) = flag {
                expected
                    .entry(flag.as_bytes())
                    .or_insert_with(|| slot(index, kind));
            }
        }
    }
    assert_eq!(table.slots, expected);
}
