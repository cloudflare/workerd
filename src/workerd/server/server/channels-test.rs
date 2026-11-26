// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::config::InvalidConfigActorClass;
use crate::config::InvalidConfigChannel;

const SPECIAL: usize = SPECIAL_SUBREQUEST_CHANNEL_COUNT as usize;

fn new_channel() -> Rc<dyn Channel> {
    Rc::new(InvalidConfigChannel)
}

/// Whether `channel` is the server's own channel `expected`.
fn is(channel: &SubrequestChannel, expected: &Rc<dyn Channel>) -> bool {
    match channel {
        SubrequestChannel::Rust(channel) => Rc::ptr_eq(channel, expected),
        SubrequestChannel::Cxx(_) => false,
    }
}

#[test]
fn channels_are_numbered_as_they_are_added() {
    let global = new_channel();
    let first = new_channel();
    let second = new_channel();
    let mut table = ChannelTable::default();
    table.set_global_outbound(SubrequestChannel::new(Rc::clone(&global)));
    assert_eq!(
        table.add_subrequest(SubrequestChannel::new(Rc::clone(&first))),
        SPECIAL_SUBREQUEST_CHANNEL_COUNT
    );
    assert_eq!(
        table.add_subrequest(SubrequestChannel::new(Rc::clone(&second))),
        SPECIAL_SUBREQUEST_CHANNEL_COUNT + 1
    );
    assert_eq!(table.subrequest_len(), 2);
    let class: Rc<dyn ActorClass> = Rc::new(InvalidConfigActorClass);
    assert_eq!(
        table.add_actor_class(ActorClassChannel::new(Rc::clone(&class))),
        0
    );
    assert_eq!(table.actor_class_len(), 1);

    let tables = table.finish().unwrap();
    assert_eq!(tables.subrequest.len(), SPECIAL + 2);
    for channel in &tables.subrequest[..SPECIAL] {
        assert!(is(channel, &global));
    }
    assert!(is(&tables.subrequest[SPECIAL], &first));
    assert!(is(&tables.subrequest[SPECIAL + 1], &second));
    assert!(
        matches!(&tables.actor_class[..], [ActorClassChannel::Rust(c)] if Rc::ptr_eq(c, &class))
    );
    assert!(tables.rpc.is_empty());
}

#[test]
fn a_later_global_outbound_replaces_the_first() {
    let first = new_channel();
    let second = new_channel();
    let mut table = ChannelTable::default();
    table.set_global_outbound(SubrequestChannel::new(Rc::clone(&first)));
    table.set_global_outbound(SubrequestChannel::new(Rc::clone(&second)));
    let tables = table.finish().unwrap();
    assert!(tables.subrequest.iter().all(|channel| is(channel, &second)));
    assert_eq!(Rc::strong_count(&first), 1);
}

#[test]
fn a_table_needs_a_global_outbound() {
    let error = ChannelTable::default().finish().err().unwrap();
    assert_eq!(
        error.description(),
        "the channel table has no global outbound"
    );
}
