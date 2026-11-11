//! Runs a script on Exact's runner and prints what the C++ target must match:
//! the app state and every node with a testId (its text, and whether it lies
//! under an unselected route) after boot and after each step.
//!
//! Usage: `badge-ref APP.contract STEPS.json FIXTURE.json > RUN.json`
//!
//! The app's data module answers from FIXTURE.json (a made-up badge): its
//! profile, saved settings, and battery, and `setBrightness` sends. The
//! runner's clock is the fixture's (`epoch_ms`, `utc_offset`).
//!
//! A step is `["key", K]` (a key at the navigation root, where the badge's
//! buttons arrive), `["tap", TEST_ID]` (a press on that node, an active one
//! first; an inactive node takes no press, as every host makes it inert), or
//! `["navigate", LOCATION]` (a location typed into the navigation root),
//! `["pan", "TEST_ID DX DY"]` or `["panrelease", "TEST_ID VX VY"]` (a pan
//! event at that node, as a host's recognizer sends it).
//! The host behaviors (focus, Enter, the Back rule) are not in a step: they
//! are the host's, not the language's, and `tools/badge_check.py` checks them
//! on the C++ boards.

use std::path::Path;
use std::process::ExitCode;

use exact_kernel::{Kernel, NodeType, PropId, ViewId};
use exact_plan::Value as PlanValue;
use exact_runner::{Answer, DataError, DataSource, Event, Runner, Store, Viewport};
use serde_json::{json, Value};

const ROOT: &str = "navigation";

fn main() -> ExitCode {
    match run() {
        Ok(out) => {
            println!("{out}");
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("badge-ref: {e}");
            ExitCode::FAILURE
        }
    }
}

/// The badge's data module, answering from the fixture.
struct BadgeData {
    fixture: serde_json::Value,
}

impl BadgeData {
    fn text(&self, path: &[&str]) -> PlanValue {
        let mut v = &self.fixture;
        for key in path {
            v = &v[*key];
        }
        PlanValue::str(v.as_str().unwrap_or(""))
    }

    fn number(&self, path: &[&str]) -> PlanValue {
        let mut v = &self.fixture;
        for key in path {
            v = &v[*key];
        }
        PlanValue::Number(v.as_f64().unwrap_or(-1.0))
    }
}

impl DataSource for BadgeData {
    fn query(&mut self, source: &str, _: &[PlanValue]) -> Result<PlanValue, DataError> {
        Ok(match source {
            "badgeProfile" => PlanValue::record(
                ["name", "company", "url", "label", "photo"]
                    .iter()
                    .map(|f| self.text(&["profile", f]))
                    .collect(),
            ),
            "badgeSettings" => PlanValue::record(vec![
                self.number(&["settings", "brightness"]),
                self.text(&["settings", "orientation"]),
                PlanValue::Bool(self.fixture["settings"]["unlocked"].as_bool().unwrap_or(false)),
            ]),
            "badgeBattery" => PlanValue::record(vec![self.number(&["battery"])]),
            "setBrightness" => PlanValue::record(vec![PlanValue::Bool(true)]),
            _ => return Err(DataError::UnknownSource(source.into())),
        })
    }

    fn answer(&mut self, _: &mut Store, source: &str, args: &[PlanValue]) -> Result<Answer, DataError> {
        self.query(source, args).map(Answer::Now)
    }
}

fn run() -> Result<String, String> {
    let args: Vec<String> = std::env::args().collect();
    let [_, app, steps, fixture] = args.as_slice() else {
        return Err("usage: badge-ref APP.contract STEPS.json FIXTURE.json".into());
    };
    let fixture: serde_json::Value = serde_json::from_str(
        &std::fs::read_to_string(fixture).map_err(|e| format!("{fixture}: {e}"))?,
    )
    .map_err(|e| format!("{fixture}: {e}"))?;
    let epoch = fixture["epoch_ms"].as_f64().ok_or("the fixture has no epoch_ms")?;
    let offset = fixture["utc_offset"].as_f64().ok_or("the fixture has no utc_offset")?;
    let plan = contract::compile_path(Path::new(app)).map_err(|e| format!("{app}: {e}"))?;
    let text = std::fs::read_to_string(steps).map_err(|e| format!("{steps}: {e}"))?;
    let steps: Vec<(String, String)> =
        serde_json::from_str(&text).map_err(|e| format!("{steps}: {e}"))?;
    let viewport = Viewport::sized(468.0, 466.0);
    let data = BadgeData { fixture };
    let mut runner = Runner::boot(plan, data, Kernel::with_monospace(), viewport, "/")
        .map_err(|e| format!("boot: {e:?}"))?;
    runner
        .set_time(epoch, offset)
        .map_err(|e| format!("set_time: {e:?}"))?;
    let mut out = vec![snapshot(&runner, &json!(["boot"]))?];
    for (op, arg) in steps {
        apply(&mut runner, &op, &arg)?;
        out.push(snapshot(&runner, &json!([op, arg]))?);
    }
    serde_json::to_string_pretty(&json!({ "steps": out })).map_err(|e| e.to_string())
}

/// The live node with `test_id`, an active one first; `None` if none is live.
fn find(runner: &Runner<BadgeData>, test_id: &str) -> Option<ViewId> {
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

fn apply(runner: &mut Runner<BadgeData>, op: &str, arg: &str) -> Result<(), String> {
    let root = find(runner, ROOT).ok_or("no navigation root")?;
    let (at, event) = match op {
        "key" => (root, Event::key(arg)),
        "navigate" => (root, Event::Navigate(arg.to_owned())),
        "pan" | "panrelease" => {
            let parts: Vec<&str> = arg.split(' ').collect();
            let [id, a, b] = parts.as_slice() else {
                return Err(format!("{op} {arg}: expected TEST_ID A B"));
            };
            let number = |s: &str| s.parse::<f64>().map_err(|e| format!("{op} {arg}: {e}"));
            let at = find(runner, id).ok_or_else(|| format!("no node with testId {id}"))?;
            if runner.inactive(at) {
                return Ok(());
            }
            let event = if op == "pan" {
                Event::Pan(number(a)?, number(b)?)
            } else {
                Event::PanRelease(number(a)?, number(b)?)
            };
            (at, event)
        }
        "tap" => {
            let id = find(runner, arg).ok_or_else(|| format!("no node with testId {arg}"))?;
            if runner.inactive(id) {
                return Ok(());
            }
            (id, Event::Press)
        }
        other => return Err(format!("unknown step {other}")),
    };
    runner
        .dispatch(at, event)
        .map(|_| ())
        .map_err(|e| format!("{op} {arg}: {e:?}"))
}

fn snapshot(runner: &Runner<BadgeData>, step: &Value) -> Result<Value, String> {
    let state: Value =
        serde_json::from_str(&exact_runner::agent::handle(runner, r#"{"op":"state"}"#))
            .map_err(|e| format!("state: {e}"))?;
    let pick = |name: &str| state.get(name).cloned().unwrap_or_else(|| json!({}));
    let kernel = runner.kernel();
    let mut nodes = Vec::new();
    let mut stack: Vec<ViewId> = runner.roots().into_iter().rev().collect();
    while let Some(id) = stack.pop() {
        let Some(node) = kernel.node(id) else {
            continue;
        };
        if let Some(test_id) = node.props.str(PropId::TestId) {
            let text = if node.node_type == NodeType::Text {
                node.props.str(PropId::Text).unwrap_or("")
            } else {
                ""
            };
            nodes.push(json!({ "testId": test_id, "text": text, "inactive": runner.inactive(id) }));
        }
        stack.extend(node.children().into_iter().rev());
    }
    Ok(json!({
        "step": step,
        "state": { "slots": pick("slots"), "derives": pick("derives"), "resources": pick("resources") },
        "nodes": nodes,
    }))
}
