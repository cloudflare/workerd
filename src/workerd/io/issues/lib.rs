// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Runtime-native issues. A request's code reports problems it sees in a customer's Worker to an
//! [`IssueCollector`]. At the end of the request the collector hands back the issues it kept.

mod collector;
mod ffi;

pub use collector::Attachment;
pub use collector::Breadcrumb;
pub use collector::BreadcrumbRing;
pub use collector::CrumbId;
pub use collector::CrumbKind;
pub use collector::CrumbState;
pub use collector::ERROR_LOG;
pub use collector::ERROR_OUTCOME;
pub use collector::End;
pub use collector::ErrorInfo;
pub use collector::Field;
pub use collector::HTTP_SERVER_ERROR;
pub use collector::InlineStr;
pub use collector::Invocation;
pub use collector::Issue;
pub use collector::IssueCollector;
pub use collector::UserContext;
pub use collector::Value;
