use super::*;

fn normalized(text: &str) -> Option<String> {
    CompatDate::parse(text).map(|d| d.to_string())
}

#[test]
fn parses_valid_dates() {
    for date in [
        "2021-05-17",
        "2021-05-01",
        "2000-01-01",
        "2999-12-31",
        "2024-02-29",
        "2112-04-01",
    ] {
        assert_eq!(normalized(date).as_deref(), Some(date), "{date}");
    }
}

#[test]
fn rejects_invalid_dates() {
    for date in [
        "2024-2-32",
        "3000-01-01",
        "1999-12-31",
        "123-01-01",
        "2021-13-01",
        "2021-12-32",
        "2021-00-01",
        "2021-01-00",
        " 2021-05-17",
        "2021 -05-17",
        "2021- 05-17",
        "2021-05 -17",
        "2021-05- 17",
        "2021-05-17 ",
        "2021/05/17",
        "2021_05_17",
        "2021-5-07",
        "2021-05-7",
        "202-05-07",
        "",
        "abcd",
        "2021--1-01",
        "2021-05--1",
        "2021-1--05",
    ] {
        assert_eq!(normalized(date), None, "{date:?}");
    }
}

// The fields are read as `sscanf("%d-%d-%d")` reads them, so a dash may end the day early.
#[test]
fn reads_fields_as_sscanf_does() {
    assert_eq!(normalized("2021-05-1-").as_deref(), Some("2021-05-01"));
}

#[test]
fn accepts_dates_that_do_not_exist() {
    assert_eq!(normalized("2022-02-30").as_deref(), Some("2022-02-30"));
}

#[test]
fn orders_by_year_month_day() {
    let parse = |s| CompatDate::parse(s).unwrap();
    assert!(parse("2021-05-17") < parse("2021-05-18"));
    assert!(parse("2021-05-31") < parse("2021-06-01"));
    assert!(parse("2021-12-31") < parse("2022-01-01"));
    assert_eq!(parse("2021-05-17"), parse("2021-05-17"));
}

#[test]
fn converts_days_to_civil_dates() {
    assert_eq!(civil_from_days(0), (1970, 1, 1));
    assert_eq!(civil_from_days(10_957), (2000, 1, 1));
    assert_eq!(civil_from_days(19_782), (2024, 2, 29));
    assert_eq!(civil_from_days(19_783), (2024, 3, 1));
    assert_eq!(civil_from_days(20_775), (2026, 11, 18));
}

#[test]
fn today_parses() {
    let today = CompatDate::today().to_string();
    assert_eq!(normalized(&today), Some(today));
}
