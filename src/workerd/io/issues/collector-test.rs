// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn crumb_text(crumb: &Breadcrumb) -> String {
    let state = match crumb.state {
        CrumbState::Started => " started",
        CrumbState::Done => " done",
        CrumbState::Failed => " failed",
        CrumbState::None => "",
    };
    match crumb.kind {
        CrumbKind::Call => format!("{}{state}", crumb.label.as_str()),
        CrumbKind::Log(level) => format!("log {} {}", level.as_str(), crumb.label.as_str()),
    }
}

fn kept(end: &End) -> Vec<(&str, &str)> {
    end.issues
        .iter()
        .map(|issue| (issue.code.as_str(), issue.message.as_str()))
        .collect()
}

#[test]
fn a_5xx_after_an_exception_is_dropped_and_the_exception_carries_the_final_status() {
    let mut collector = IssueCollector::default();
    collector.report("uncaught_exception", "TypeError: item is undefined");
    assert!(
        collector
            .report(HTTP_SERVER_ERROR, "Worker returned HTTP 500")
            .is_none()
    );
    collector.set_status(500);

    let end = collector.end();

    assert_eq!(
        kept(&end),
        vec![("uncaught_exception", "TypeError: item is undefined")]
    );
    assert_eq!(end.status, Some(500));
}

#[test]
fn a_5xx_with_no_fact_is_kept_with_its_status() {
    let mut collector = IssueCollector::default();
    collector.report(HTTP_SERVER_ERROR, "Worker returned HTTP 502");
    collector.set_status(502);

    let end = collector.end();

    assert_eq!(
        kept(&end),
        vec![(HTTP_SERVER_ERROR, "Worker returned HTTP 502")]
    );
    assert_eq!(end.status, Some(502));
}

#[test]
fn an_error_log_after_the_5xx_does_not_drop_it() {
    let mut collector = IssueCollector::default();
    collector.report(HTTP_SERVER_ERROR, "Worker returned HTTP 500");
    collector.set_status(500);
    collector.report(ERROR_LOG, "avatar upload failed");

    let end = collector.end();

    assert_eq!(
        kept(&end),
        vec![
            (HTTP_SERVER_ERROR, "Worker returned HTTP 500"),
            (ERROR_LOG, "avatar upload failed"),
        ]
    );
}

#[test]
fn an_error_log_before_the_5xx_drops_it() {
    let mut collector = IssueCollector::default();
    collector.report(ERROR_LOG, "avatar upload failed");
    collector.report(HTTP_SERVER_ERROR, "Worker returned HTTP 500");

    assert_eq!(
        kept(&collector.end()),
        vec![(ERROR_LOG, "avatar upload failed")]
    );
}

#[test]
fn the_error_outcome_fallback_follows_the_same_rule() {
    let mut collector = IssueCollector::default();
    collector.report("cpu_limit_exceeded", "too slow");
    assert!(collector.report(ERROR_OUTCOME, "exceededCpu").is_none());
    assert_eq!(
        kept(&collector.end()),
        vec![("cpu_limit_exceeded", "too slow")]
    );

    let mut collector = IssueCollector::default();
    collector.report(ERROR_OUTCOME, "exception");
    collector.report("uncaught_exception", "boom");
    assert_eq!(
        kept(&collector.end()),
        vec![(ERROR_OUTCOME, "exception"), ("uncaught_exception", "boom")]
    );
}

#[test]
fn six_error_logs_and_an_exception_keep_five_logs_and_the_exception() {
    let mut collector = IssueCollector::default();
    for i in 1..=6 {
        collector.report(ERROR_LOG, &format!("item {i} missing price"));
    }
    collector.report("uncaught_exception", "Error: no prices");
    collector.report(HTTP_SERVER_ERROR, "Worker returned HTTP 500");

    assert_eq!(
        kept(&collector.end()),
        vec![
            (ERROR_LOG, "item 1 missing price"),
            (ERROR_LOG, "item 2 missing price"),
            (ERROR_LOG, "item 3 missing price"),
            (ERROR_LOG, "item 4 missing price"),
            (ERROR_LOG, "item 5 missing price"),
            ("uncaught_exception", "Error: no prices"),
        ]
    );
}

#[test]
fn a_capped_fact_still_drops_a_later_fallback() {
    let mut collector = IssueCollector::default();
    for i in 1..=MAX_HELD {
        assert!(
            collector
                .report("uncaught_exception", &format!("e{i}"))
                .is_some()
        );
    }
    assert!(
        collector
            .report("uncaught_exception", "over the cap")
            .is_none()
    );
    assert!(
        collector
            .report(HTTP_SERVER_ERROR, "Worker returned HTTP 500")
            .is_none()
    );

    let end = collector.end();

    assert_eq!(end.issues.len(), MAX_HELD);
    assert!(
        end.issues
            .iter()
            .all(|issue| issue.code == "uncaught_exception")
    );
}

#[test]
fn a_capped_error_log_still_drops_a_later_fallback() {
    let mut collector = IssueCollector::default();
    for i in 1..=MAX_ERROR_LOGS {
        assert!(collector.report(ERROR_LOG, &format!("log {i}")).is_some());
    }
    assert!(collector.report(ERROR_LOG, "over the cap").is_none());
    assert!(
        collector
            .report(HTTP_SERVER_ERROR, "Worker returned HTTP 500")
            .is_none()
    );
    assert_eq!(collector.end().issues.len(), MAX_ERROR_LOGS);
}

#[test]
fn a_repeated_fact_still_drops_a_later_fallback() {
    let mut collector = IssueCollector::default();
    assert!(collector.report("uncaught_exception", "boom").is_some());
    assert!(collector.report("uncaught_exception", "boom").is_none());
    assert!(collector.report(ERROR_OUTCOME, "exception").is_none());
}

#[test]
fn a_fallback_that_is_itself_capped_does_not_count_as_a_fact() {
    let mut collector = IssueCollector::default();
    for i in 1..=MAX_HELD {
        collector.report(
            HTTP_SERVER_ERROR,
            &format!("Worker returned HTTP {}", 500 + i),
        );
    }
    assert!(collector.report(HTTP_SERVER_ERROR, "one more").is_none());
    assert!(collector.report(ERROR_OUTCOME, "exception").is_none());
    assert_eq!(collector.end().issues.len(), MAX_HELD);
}

#[test]
fn exact_repeats_are_held_once() {
    let mut collector = IssueCollector::default();
    assert!(collector.report(ERROR_LOG, "boom").is_some());
    assert!(collector.report(ERROR_LOG, "boom").is_none());
    assert!(collector.report(ERROR_LOG, "boom again").is_some());
    assert!(collector.report("uncaught_exception", "boom").is_some());

    assert_eq!(
        kept(&collector.end()),
        vec![
            (ERROR_LOG, "boom"),
            (ERROR_LOG, "boom again"),
            ("uncaught_exception", "boom"),
        ]
    );
}

#[test]
fn any_code_is_accepted() {
    let mut collector = IssueCollector::default();
    assert!(collector.report("cpu_limit_exceeded", "too slow").is_some());
    assert!(collector.report("", "empty").is_some());
    assert_eq!(collector.end().issues.len(), 2);
}

#[test]
fn ending_twice_keeps_nothing_the_second_time() {
    let mut collector = IssueCollector::default();
    collector.report("uncaught_exception", "boom");

    assert_eq!(collector.end().issues.len(), 1);
    assert_eq!(collector.end().issues.len(), 0);
}

#[test]
fn the_end_carries_the_invocation() {
    let mut collector = IssueCollector::default();
    collector.set_event("fetch");
    collector.set_request("GET", &format!("/{}", "p".repeat(300)));

    let end = collector.end();

    assert_eq!(end.invocation.event, "fetch");
    assert_eq!(end.invocation.method, "GET");
    assert_eq!(end.invocation.path.len(), MAX_PATH);
}

#[test]
fn the_ring_keeps_the_last_three_oldest_first_and_issues_snapshot_it() {
    let mut collector = IssueCollector::default();
    let sessions = collector.push_breadcrumb(Breadcrumb::call(2, "kv get SESSIONS"));
    collector.finish_breadcrumb(sessions, true);
    let carts = collector.push_breadcrumb(Breadcrumb::call(10, "kv get CARTS"));
    collector.finish_breadcrumb(carts, true);
    collector.push_breadcrumb(Breadcrumb::call(15, "d1 query PRICES_DB"));
    collector.report(ERROR_LOG, "first");
    collector.push_breadcrumb(Breadcrumb::log(43, "info", "pricing sku-991"));
    collector.report("uncaught_exception", "TypeError");

    let end = collector.end();

    assert_eq!(
        end.issues
            .iter()
            .map(|issue| issue.breadcrumbs.iter().map(crumb_text).collect::<Vec<_>>())
            .collect::<Vec<_>>(),
        vec![
            vec![
                "kv get SESSIONS done".to_owned(),
                "kv get CARTS done".to_owned(),
                "d1 query PRICES_DB started".to_owned(),
            ],
            vec![
                "kv get CARTS done".to_owned(),
                "d1 query PRICES_DB started".to_owned(),
                "log info pricing sku-991".to_owned(),
            ],
        ]
    );
}

#[test]
fn a_late_finish_leaves_a_reused_slot_alone() {
    let mut ring = BreadcrumbRing::default();
    let slow = ring.push(Breadcrumb::call(1, "fetch api.example"));
    ring.push(Breadcrumb::log(3, "info", "a"));
    ring.push(Breadcrumb::log(4, "info", "b"));
    ring.push(Breadcrumb::log(7, "warn", "payment slow"));
    ring.finish(slow, false);

    let texts: Vec<_> = ring.snapshot().iter().map(crumb_text).collect();
    assert_eq!(
        texts,
        vec!["log info a", "log info b", "log warn payment slow"]
    );
}

#[test]
fn user_context_keeps_the_last_value_and_snapshots_per_issue() {
    let mut collector = IssueCollector::default();
    collector.note_user_context("user.id", "u_1");
    collector.note_user_context("other.key", "ignored");
    collector.report(ERROR_LOG, "first");
    collector.note_user_context("user.id", "u_2");
    collector.note_user_context("session.id", &"s".repeat(200));
    let issue = collector.report("uncaught_exception", "second").unwrap();

    assert_eq!(issue.user.user_id.as_deref(), Some("u_2"));
    assert_eq!(issue.user.session_id.as_deref().map(str::len), Some(128));
    assert_eq!(issue.user.account_id, None);
}

#[test]
fn fields_errors_and_attachments_are_capped() {
    let mut collector = IssueCollector::default();
    let issue = collector.report("error_log", "boom").unwrap();

    for i in 0..9 {
        assert_eq!(issue.add_field(&format!("f{i}"), Value::Int(i)), i < 8);
    }
    issue.add_field("long", Value::Str("x".repeat(300)));
    assert_eq!(issue.fields.len(), 8);
    assert_eq!(
        issue.fields.first().map(|field| field.name.as_str()),
        Some("f0")
    );

    for i in 0..5 {
        assert_eq!(
            issue.add_error("Error", "m", Some(&"s".repeat(20_000))),
            i < 4
        );
    }
    assert_eq!(
        issue
            .errors
            .first()
            .and_then(|e| e.stack.as_ref())
            .map(String::len),
        Some(16_384)
    );

    assert!(issue.add_attachment("args", &"a".repeat(10_000)));
    assert!(issue.add_attachment("more", &"b".repeat(10_000)));
    assert!(!issue.add_attachment("none", "full"));
    let lens: Vec<_> = issue
        .attachments
        .iter()
        .map(|a| (a.body.len(), a.truncated))
        .collect();
    assert_eq!(lens, vec![(10_000, false), (6_384, true)]);
}

#[test]
fn truncation_never_splits_a_character() {
    assert_eq!(truncate("aé", 2), "a");
    assert_eq!(InlineStr::<4>::new("abcdef").as_str(), "abcd");
    assert_eq!(InlineStr::<128>::new(&"é".repeat(100)).as_str().len(), 128);
}
