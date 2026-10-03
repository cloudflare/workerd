#[cxx::bridge(namespace = "workerd::rust::perfetto")]
mod bridge {
    extern "Rust" {
        /// Registers the Rust track event categories with Perfetto. Called by
        /// `workerd::PerfettoSession::registerWorkerdTracks()` after `perfetto::Tracing` has been
        /// initialized.
        fn register_track_events();
    }
}

use crate::register_track_events;
