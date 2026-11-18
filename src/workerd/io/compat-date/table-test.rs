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
