//! AT-1 reference oracle, compute crate. Standard library only; no clock, file, process,
//! thread, network, environment or random facility anywhere below this module
//! (isolation.sh enforces it). All I/O lives in src/main.rs.
#![allow(dead_code)] // several helpers exist for the test suite and the fixtures only

pub mod big;
pub mod case;
pub mod closed;
pub mod complex;
pub mod fixtures;
pub mod matrix;
pub mod model;
pub mod result;
pub mod sha256;

/// Mathematical specification the hand-table tests and fixtures target: AT1_SPEC.md,
/// aien-architecture pull request 185, merged as this squash commit (section 13 tables).
pub const SPEC_COMMIT: &str = "81047f52850f4bd14c2fc5772ec2ac833ac694b9";
