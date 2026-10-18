//! Compiles a Contract plan to app-specific C++ over the `contract_rt` runtime
//! (Plan B). `emit` refuses, by name, everything outside the C++ subset.

mod assets;
mod code;
pub mod dump;
pub mod emit;
pub mod ext;
mod sound;
mod theme;

pub use assets::{faces_needed, pick_weight};
pub use code::ROUTE_CALLS;
pub use emit::{emit, needs, Needs};
