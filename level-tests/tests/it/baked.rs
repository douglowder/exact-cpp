//! The device boots a baked plan (`contract::bake`, encoded, decoded), not a
//! freshly compiled one. Switching the screen must work on that plan too.
use exact_kernel::Kernel;
use exact_runner::{Event, Runner};
use level_data::{LevelData, Shared};

const SOURCE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");

#[test]
fn a_baked_plan_switches_screens() {
    let plan =
        contract::compile_path(std::path::Path::new(SOURCE)).unwrap_or_else(|e| panic!("{e}"));
    let baked =
        contract::bake(plan, LevelData::default()).unwrap_or_else(|e| panic!("bake: {e:?}"));
    let plan =
        exact_plan::Plan::decode(&baked.encode()).unwrap_or_else(|e| panic!("decode: {e:?}"));
    let mut runner = Runner::boot(
        plan,
        LevelData::new(Shared::default()),
        Kernel::with_monospace(),
        exact_runner::Viewport::sized(468.0, 466.0),
        "/",
    )
    .unwrap_or_else(|e| panic!("boot: {e:?}"));
    let root = {
        let k = runner.kernel();
        let key = k.find_by_test_id("level")[0];
        k.node_by_key(key)
            .map(|n| n.id)
            .unwrap_or_else(|| panic!("root"))
    };
    for key in ["ArrowRight", "Enter", "Enter"] {
        runner
            .dispatch(root, Event::key(key))
            .unwrap_or_else(|e| panic!("{key}: {e:?}"));
    }
}
