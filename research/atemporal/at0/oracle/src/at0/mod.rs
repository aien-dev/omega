//! AT-0 reference oracle, compute crate. Standard library only; no clock, file,
//! process, thread, network, environment or random facility anywhere below
//! this module (isolation.sh enforces it). All I/O lives in src/main.rs.
#![allow(dead_code)] // several helpers exist for the test suite and calibration, not the binary

pub mod case;
pub mod complex;
pub mod exact;
pub mod fixtures;
pub mod matrix_path;
pub mod rational;
pub mod reference;
pub mod result;
pub mod sha256;

/// Draft mathematical contract the hand-table tests target (aien-architecture
/// pull request 176, not merged): re-check when it merges.
pub const SPEC_DRAFT_COMMIT: &str = "0efd1a14cbdf117bc694b556bb61d031cf80c8f9";
