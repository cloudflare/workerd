// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;

use super::*;

fn hub(count: usize) -> (Arc<Hub>, Vec<Seat>) {
    Hub::new(Placement {
        count,
        spread: true,
    })
}

/// The threads whose links thread `seat` was told to take, so far.
fn told(seat: &mut Seat) -> Vec<usize> {
    let mut peers = Vec::new();
    while let Ok(Mail::Link(peer)) = seat.inbox.try_recv() {
        peers.push(peer);
    }
    peers
}

#[test]
fn every_thread_has_a_seat_in_order() {
    let (hub, seats) = hub(5);
    assert_eq!(hub.threads(), 5);
    let indexes: Vec<usize> = seats.iter().map(|seat| seat.index).collect();
    assert_eq!(indexes, [0, 1, 2, 3, 4]);
}

#[test]
fn the_hub_places_by_its_policy() {
    let (spread, _seats) = hub(4);
    let homes: std::collections::HashSet<usize> = (0..200)
        .map(|n| spread.home(&Scope::Durable("ns"), &format!("{n}")))
        .collect();
    assert_eq!(homes.len(), 4);

    let (first, _seats) = Hub::new(Placement {
        count: 4,
        spread: false,
    });
    for n in 0..200 {
        assert_eq!(first.home(&Scope::Durable("ns"), &format!("{n}")), 0);
    }
}

#[test]
fn a_pair_of_threads_gets_one_pipe_whichever_asks_first() {
    let (hub, mut seats) = hub(4);
    let runtime = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();

    // Thread 3 asks; thread 1 is told, and takes the other end of the same pipe.
    let SessionEnd(mut asked) = hub.link(3, 1).expect("the first to ask gets its end");
    assert_eq!(told(&mut seats[1]), [3]);
    assert!(told(&mut seats[3]).is_empty());
    let SessionEnd(mut taken) = hub.link(1, 3).expect("the other thread gets its end");
    // Neither is told again, and neither gets a second end.
    assert!(told(&mut seats[1]).is_empty());
    assert!(told(&mut seats[3]).is_empty());
    assert!(hub.link(3, 1).is_none());
    assert!(hub.link(1, 3).is_none());

    runtime.block_on(async {
        asked.write_all(b"ping").await.unwrap();
        let mut received = [0; 4];
        taken.read_exact(&mut received).await.unwrap();
        assert_eq!(&received, b"ping");
        taken.write_all(b"pong").await.unwrap();
        asked.read_exact(&mut received).await.unwrap();
        assert_eq!(&received, b"pong");
    });

    // Another pair has a pipe of its own, and the threads not in it hear nothing.
    assert!(hub.link(1, 2).is_some());
    assert_eq!(told(&mut seats[2]), [1]);
    assert!(told(&mut seats[0]).is_empty());
    assert!(told(&mut seats[3]).is_empty());
}

#[test]
fn both_threads_asking_at_once_share_the_pipe() {
    for _ in 0..200 {
        let (hub, mut seats) = hub(2);
        let other = Arc::clone(&hub);
        let asking = std::thread::spawn(move || other.link(1, 0));
        let ours = hub.link(0, 1);
        let theirs = asking.join().unwrap();
        let (Some(SessionEnd(mut ours)), Some(SessionEnd(mut theirs))) = (ours, theirs) else {
            panic!("each thread gets its end");
        };
        // Exactly one of them made the pipe, and the other was told.
        let told = told(&mut seats[0]).len() + told(&mut seats[1]).len();
        assert_eq!(told, 1);
        let runtime = tokio::runtime::Builder::new_current_thread()
            .build()
            .unwrap();
        runtime.block_on(async {
            ours.write_all(b"x").await.unwrap();
            let mut received = [0; 1];
            theirs.read_exact(&mut received).await.unwrap();
        });
    }
}

#[test]
fn no_thread_links_to_itself_or_to_a_thread_the_process_lacks() {
    let (hub, mut seats) = hub(2);
    assert!(hub.link(1, 1).is_none());
    assert!(hub.link(0, 2).is_none());
    assert!(hub.link(2, 0).is_none());
    assert!(told(&mut seats[0]).is_empty());
    assert!(!hub.post(2, Mail::Link(0)));
}

#[test]
fn a_thread_that_is_gone_takes_no_mail_and_no_end_of_a_session() {
    let (hub, mut seats) = hub(2);
    drop(seats.pop());
    assert!(!hub.post(1, Mail::Link(0)));
    assert!(hub.post(0, Mail::Link(1)));
    // The end is handed out all the same, with nothing at the other end.
    let SessionEnd(mut end) = hub.link(0, 1).expect("the thread that asks gets its end");
    let runtime = tokio::runtime::Builder::new_current_thread()
        .build()
        .unwrap();
    let read = runtime.block_on(async { end.read(&mut [0; 1]).await.unwrap() });
    assert_eq!(read, 0);
}

#[test]
fn the_hub_and_a_seat_cross_threads() {
    fn send<T: Send>() {}
    fn sync<T: Sync>() {}
    send::<Arc<Hub>>();
    sync::<Hub>();
    send::<Seat>();
}
