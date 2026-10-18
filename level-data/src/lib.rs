//! The level app's data source.
//!
//! The device's IMU loop turns acceleration into a filtered attitude
//! ([`AttitudeFilter`]), stores it in a [`Shared`] cell, and calls
//! [`Shared::publish`], which announces the `attitude` topic when the value
//! changes. The runner then asks [`LevelData`] again, and the app's
//! `resource tilt = attitude()` gets the new value. Values are whole tenths of
//! a degree, so the app compares integers.

use std::sync::{Arc, Mutex, PoisonError};

use exact_plan::Value;
use exact_runner::{Answer, DataError, DataSource, Native, Store};

/// The topic the `attitude` answer watches.
pub const TOPIC: &str = "attitude";

/// Pitch and roll in degrees.
///
/// IMU axes with the lanyard at the bottom and the screen facing up: +x points
/// toward the lanyard, +y toward screen-left, +z out of the screen. Pitch is
/// positive when the top edge is raised; roll when the right edge is raised.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct Attitude {
    /// Degrees, top edge up is positive.
    pub pitch: f32,
    /// Degrees, right edge up is positive.
    pub roll: f32,
}

const RAD_TO_DEG: f32 = 57.295_78;
const MIN_GRAVITY: f32 = 0.5;
const MAX_GRAVITY: f32 = 2.0;

/// The attitude for one acceleration sample in g, or `None` for a sample that
/// is not finite or is far from 1 g (a shock).
#[must_use]
pub fn attitude_from_accel(ax: f32, ay: f32, az: f32) -> Option<Attitude> {
    if !(ax.is_finite() && ay.is_finite() && az.is_finite()) {
        return None;
    }
    let magnitude = (ax * ax + ay * ay + az * az).sqrt();
    if !(MIN_GRAVITY..=MAX_GRAVITY).contains(&magnitude) {
        return None;
    }
    Some(Attitude {
        pitch: (-ax).atan2((ay * ay + az * az).sqrt()) * RAD_TO_DEG,
        roll: (-ay).atan2((ax * ax + az * az).sqrt()) * RAD_TO_DEG,
    })
}

/// Adaptive one-pole filter: a long time constant hides sensor noise while
/// the board is still, and a short one follows real motion without lag.
#[derive(Debug, Clone, Copy, Default)]
pub struct AttitudeFilter {
    value: Option<Attitude>,
}

impl AttitudeFilter {
    /// Time constant while still.
    pub const STILL_TAU_MS: f32 = 100.0;
    /// Time constant while moving.
    pub const MOVING_TAU_MS: f32 = 25.0;
    /// A change larger than this on either axis counts as motion.
    pub const MOTION_DEGREES: f32 = 0.6;

    /// Feeds one sample taken `dt_ms` after the previous one.
    pub fn update(&mut self, raw: Attitude, dt_ms: f32) -> Attitude {
        let Some(value) = self.value else {
            self.value = Some(raw);
            return raw;
        };
        let change = (raw.pitch - value.pitch)
            .abs()
            .max((raw.roll - value.roll).abs());
        // Arithmetic, not `if moving { MOVING } else { STILL }`: LLVM turns that
        // select into a two-float constant table, which the Xtensa back end
        // (esp Rust 1.97.0.0) cannot lower ("Cannot select: PCREL_WRAPPER").
        let moving = f32::from(u8::from(change > Self::MOTION_DEGREES));
        let tau = Self::STILL_TAU_MS - moving * (Self::STILL_TAU_MS - Self::MOVING_TAU_MS);
        let alpha = 1.0 - (-dt_ms.max(0.0) / tau).exp();
        let next = Attitude {
            pitch: value.pitch + (raw.pitch - value.pitch) * alpha,
            roll: value.roll + (raw.roll - value.roll) * alpha,
        };
        self.value = Some(next);
        next
    }
}

/// Degrees to whole tenths, limited to ±90 degrees; not-finite becomes 0.
#[must_use]
pub fn tenths(degrees: f32) -> i32 {
    if !degrees.is_finite() {
        return 0;
    }
    // Truncation is safe: the value is clamped to ±900 first.
    #[allow(clippy::cast_possible_truncation)] // reason: clamped to ±900 above
    let t = (degrees * 10.0).clamp(-900.0, 900.0).round() as i32;
    t
}

/// The attitude cell shared by the IMU loop and the data source.
#[derive(Clone, Default)]
pub struct Shared {
    cell: Arc<Mutex<(i32, i32)>>,
    native: Native,
}

impl Shared {
    /// Stores a filtered attitude; announces the topic only when the stored
    /// tenths change. Returns whether it announced.
    pub fn publish(&self, attitude: Attitude) -> bool {
        let next = (tenths(attitude.pitch), tenths(attitude.roll));
        let mut cell = self.cell.lock().unwrap_or_else(PoisonError::into_inner);
        if *cell == next {
            return false;
        }
        *cell = next;
        drop(cell);
        self.native.changed(TOPIC);
        true
    }

    /// The stored attitude in tenths.
    #[must_use]
    pub fn get(&self) -> (i32, i32) {
        *self.cell.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

/// The app's data source: answers `attitude()` from the shared cell.
#[derive(Clone, Default)]
pub struct LevelData {
    shared: Shared,
}

impl LevelData {
    /// A source reading `shared`.
    #[must_use]
    pub fn new(shared: Shared) -> LevelData {
        LevelData { shared }
    }

    fn attitude(&self) -> Value {
        let (pitch, roll) = self.shared.get();
        Value::record(vec![
            Value::Number(f64::from(pitch)),
            Value::Number(f64::from(roll)),
        ])
    }
}

impl DataSource for LevelData {
    fn query(&mut self, source: &str, _: &[Value]) -> Result<Value, DataError> {
        match source {
            "attitude" => Ok(self.attitude()),
            _ => Err(DataError::UnknownSource(source.into())),
        }
    }

    fn answer(
        &mut self,
        store: &mut Store,
        source: &str,
        args: &[Value],
    ) -> Result<Answer, DataError> {
        if source == "attitude" {
            store.observe_topic(TOPIC);
        }
        self.query(source, args).map(Answer::Now)
    }

    fn native(&self) -> Option<Native> {
        Some(self.shared.native.clone())
    }
}

#[cfg(test)]
mod tests;
