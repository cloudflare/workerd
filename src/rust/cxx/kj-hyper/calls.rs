//! A connection's handler calls, each in a slot that a later call reuses once it is done.
//!
//! `FuturesUnordered` would allocate a task for each call and wake the connection through queues
//! of its own. Here a slot's future and its waker are made once, and a slot's waker names it by
//! a bit of the connection's [`Wakes`], so the connection polls the calls that woke it as it
//! does its other futures. Slots past the bits there are share the last one.

use std::future::Future;
use std::mem;
use std::pin::Pin;
use std::task::Context;
use std::task::Waker;

use crate::wakes::Wakes;

/// The bit of the first slot: those below are the connection's other futures.
const FIRST: u32 = 3;
/// The bit the slots past the others share, the highest [`Wakes`] has.
const SHARED: u32 = 62;

/// Every slot's bit.
pub const CALLS: u64 = ((1 << (SHARED + 1)) - 1) & !((1 << FIRST) - 1);

pub struct Calls<F> {
    wakes: Wakes,
    slots: Vec<Slot<F>>,
    /// The empty slots.
    free: Vec<usize>,
    running: usize,
    /// The bits the task woke for in this turn, not yet polled.
    pending: u64,
    /// The bits of the calls polled in this turn and still running. One that wakes again waits
    /// for the task's next turn, so that a call that keeps waking itself cannot hold the thread.
    polled: u64,
}

struct Slot<F> {
    /// Boxed so that the future stays put as slots are added.
    future: Pin<Box<Option<F>>>,
    waker: Waker,
    bit: u64,
}

fn bit(index: usize) -> u64 {
    let n = u32::try_from(index).map_or(SHARED, |index| (FIRST + index).min(SHARED));
    1 << n
}

impl<F: Future<Output = ()>> Calls<F> {
    pub fn new(wakes: &Wakes) -> Self {
        Self {
            wakes: wakes.clone(),
            slots: Vec::new(),
            free: Vec::new(),
            running: 0,
            pending: 0,
            polled: 0,
        }
    }

    /// Starts a turn of the task, which `woken` woke ([`Wakes::begin`]).
    pub fn begin(&mut self, woken: u64) {
        self.pending = woken & CALLS;
        self.polled = 0;
    }

    /// Polls the calls woken since they were last polled, until none is, but each at most once
    /// in a turn.
    pub fn poll(&mut self) {
        loop {
            let woken = mem::take(&mut self.pending) | self.wakes.take_any(CALLS & !self.polled);
            if woken == 0 {
                return;
            }
            self.polled |= self.poll_woken(woken);
        }
    }

    /// Adds a call, to be polled with the calls woken next.
    pub fn push(&mut self, future: F) {
        let index = if let Some(index) = self.free.pop() {
            self.slots[index].future.set(Some(future));
            index
        } else {
            let bit = bit(self.slots.len());
            self.slots.push(Slot {
                future: Box::pin(Some(future)),
                waker: self.wakes.waker(bit),
                bit,
            });
            self.slots.len() - 1
        };
        self.running += 1;
        self.wakes.wake(self.slots[index].bit);
    }

    /// Polls the calls whose bits are in `woken`; returns the bits of the slots polled whose
    /// calls are still running.
    fn poll_woken(&mut self, woken: u64) -> u64 {
        let mut polled = 0;
        for (index, slot) in self.slots.iter_mut().enumerate() {
            if woken & slot.bit == 0 {
                continue;
            }
            let Some(future) = slot.future.as_mut().as_pin_mut() else {
                continue;
            };
            if future
                .poll(&mut Context::from_waker(&slot.waker))
                .is_ready()
            {
                slot.future.set(None);
                self.free.push(index);
                self.running -= 1;
            } else {
                polled |= slot.bit;
            }
        }
        polled
    }

    /// Cancels every call.
    pub fn clear(&mut self) {
        for (index, slot) in self.slots.iter_mut().enumerate() {
            if slot.future.is_some() {
                slot.future.set(None);
                self.free.push(index);
            }
        }
        self.running = 0;
    }

    pub fn is_empty(&self) -> bool {
        self.running == 0
    }
}

#[cfg(test)]
#[path = "calls-test.rs"]
mod tests;
