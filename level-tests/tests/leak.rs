//! Live heap must stay flat while the app runs (IMU announcements every 10 ms,
//! the 50 ms task, layout after each commit, as on the device) once the
//! runner's bounded journal is full. The journal itself holds ~170 KB of small
//! strings, which is why the device puts small allocations in PSRAM.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicIsize, Ordering};

use exact_kernel::{Kernel, Offer};
use exact_runner::{Runner, Viewport};
use level_data::{Attitude, LevelData, Shared};

struct Counting;
static LIVE: AtomicIsize = AtomicIsize::new(0);

// SAFETY: forwards to the system allocator and only counts bytes.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        LIVE.fetch_add(layout.size() as isize, Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        LIVE.fetch_sub(layout.size() as isize, Ordering::Relaxed);
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

const SOURCE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");

#[derive(Clone, Copy)]
struct Inputs {
    announce: bool,
    timers: bool,
}

const BOTH: Inputs = Inputs {
    announce: true,
    timers: true,
};

fn round(runner: &mut Runner<LevelData>, shared: &Shared, i: u32, now: &mut f64, inputs: Inputs) {
    if inputs.announce {
        let wobble = if i.is_multiple_of(2) { 0.3 } else { -0.4 };
        shared.publish(Attitude {
            pitch: wobble,
            roll: -wobble,
        });
        let _ = runner.apply_announced();
    }
    if inputs.timers {
        *now += 10.0;
        let _ = runner.advance_timed(*now);
    }
    for root in runner.roots() {
        let _ = runner
            .kernel_mut()
            .compute_layout(root, Offer::definite(468.0, 466.0));
    }
    let _ = runner.take_commands();
}

fn growth(inputs: Inputs) -> isize {
    let plan =
        contract::compile_path(std::path::Path::new(SOURCE)).unwrap_or_else(|e| panic!("{e}"));
    let shared = Shared::default();
    let mut runner = Runner::boot(
        plan,
        LevelData::new(shared.clone()),
        Kernel::with_monospace(),
        Viewport::sized(468.0, 466.0),
        "/",
    )
    .unwrap_or_else(|e| panic!("{e:?}"));
    runner.listen(std::sync::Arc::new(|| {}));
    let mut now = 0.0;
    // Fill the runner's journal ring first; it is bounded and full after ~2000 lines.
    for i in 0..4000 {
        round(&mut runner, &shared, i, &mut now, inputs);
    }
    let before = LIVE.load(Ordering::Relaxed);
    for i in 4000..5000 {
        round(&mut runner, &shared, i, &mut now, inputs);
    }
    let after = LIVE.load(Ordering::Relaxed);
    after - before
}

// One test function: the counting allocator is process-wide, so the cases run in order.
#[test]
fn live_heap_is_flat_under_announcements_and_timers() {
    let announce = growth(Inputs {
        announce: true,
        timers: false,
    });
    let timers = growth(Inputs {
        announce: false,
        timers: true,
    });
    let both = growth(BOTH);
    println!("LEAK per 1000 rounds: announcements {announce} B, timers {timers} B, both {both} B");
    assert!(both < 4096, "live heap grew {both} bytes over 1000 rounds");
}
