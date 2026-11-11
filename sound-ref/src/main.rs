//! Runs a script on Exact's runner and prints what the C++ target must play: the app's slots
//! after boot and after each step, and every voice the runner issued (LLP 1096 D5, LLP 1125 D3).
//!
//! Usage: `sound-ref APP.contract STEPS.json ROOT_TEST_ID > RUN.json`
//!
//! A step is `["key", K]` (a key at the node with `ROOT_TEST_ID`, where the board's buttons
//! arrive), `["tap", TEST_ID]` (a press on that node), or `["clock", MS]` (the runner's clock
//! moves on by `MS` milliseconds; it starts at 0). The app's data source answers nothing: its
//! sounds come from its `sequence`, which the runner expands.

use std::path::Path;
use std::process::ExitCode;

use exact_kernel::{Kernel, ViewId};
use exact_plan::Value as PlanValue;
use exact_runner::{DataError, DataSource, Event, Runner, Viewport};
use serde_json::{json, Value};

fn main() -> ExitCode {
    match run() {
        Ok(out) => {
            println!("{out}");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("sound-ref: {e}");
            ExitCode::FAILURE
        }
    }
}

/// A data source with no sources of its own.
struct NoData;

impl DataSource for NoData {
    fn query(&mut self, source: &str, _: &[PlanValue]) -> Result<PlanValue, DataError> {
        Err(DataError::UnknownSource(source.into()))
    }
}

fn run() -> Result<String, String> {
    let args: Vec<String> = std::env::args().collect();
    let [_, app, steps, root] = args.as_slice() else {
        return Err("usage: sound-ref APP.contract STEPS.json ROOT_TEST_ID".into());
    };
    let plan = contract::compile_path(Path::new(app)).map_err(|e| format!("{app}: {e}"))?;
    let text = std::fs::read_to_string(steps).map_err(|e| format!("{steps}: {e}"))?;
    let steps: Vec<(String, String)> =
        serde_json::from_str(&text).map_err(|e| format!("{steps}: {e}"))?;
    let viewport = Viewport::sized(468.0, 466.0);
    let mut runner = Runner::boot(plan, NoData, Kernel::with_monospace(), viewport, "/")
        .map_err(|e| format!("boot: {e:?}"))?;
    let mut now = 0.0;
    runner.advance(now).map_err(|e| format!("advance: {e:?}"))?;
    let mut out = vec![snapshot(&runner, &json!(["boot"]))?];
    for (op, arg) in steps {
        if op == "clock" {
            now += arg.parse::<f64>().map_err(|e| format!("clock {arg}: {e}"))?;
            runner.advance(now).map_err(|e| format!("clock {arg}: {e:?}"))?;
        } else {
            apply(&mut runner, root, &op, &arg)?;
        }
        out.push(snapshot(&runner, &json!([op, arg]))?);
    }
    let voices: Vec<Value> = runner
        .sounds()
        .voices()
        .map(|v| {
            json!({ "id": v.id, "sound": v.sound, "src": v.src, "at": v.at, "gain": v.gain,
                    "ends": v.ends, "by": v.by.word(), "seq": v.seq })
        })
        .collect();
    serde_json::to_string_pretty(&json!({ "steps": out, "voices": voices }))
        .map_err(|e| e.to_string())
}

/// The live node with `test_id`, an active one first.
fn find(runner: &Runner<NoData>, test_id: &str) -> Option<ViewId> {
    let kernel = runner.kernel();
    let ids: Vec<ViewId> = kernel
        .find_by_test_id(test_id)
        .iter()
        .filter_map(|key| kernel.node_by_key(*key).map(|n| n.id))
        .collect();
    ids.iter()
        .copied()
        .find(|id| !runner.inactive(*id))
        .or_else(|| ids.first().copied())
}

fn apply(runner: &mut Runner<NoData>, root: &str, op: &str, arg: &str) -> Result<(), String> {
    let (at, event) = match op {
        "key" => {
            let at = find(runner, root).ok_or_else(|| format!("no node with testId {root}"))?;
            (at, Event::key(arg))
        }
        "tap" => {
            let at = find(runner, arg).ok_or_else(|| format!("no node with testId {arg}"))?;
            (at, Event::Press)
        }
        other => return Err(format!("unknown step {other}")),
    };
    runner
        .dispatch(at, event)
        .map(|_| ())
        .map_err(|e| format!("{op} {arg}: {e:?}"))
}

fn snapshot(runner: &Runner<NoData>, step: &Value) -> Result<Value, String> {
    let state: Value =
        serde_json::from_str(&exact_runner::agent::handle(runner, r#"{"op":"state"}"#))
            .map_err(|e| format!("state: {e}"))?;
    Ok(json!({ "step": step, "slots": state.get("slots").cloned().unwrap_or_else(|| json!({})) }))
}
