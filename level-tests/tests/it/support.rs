//! Boots apps/level/app.contract on the runner with a test attitude cell, and
//! drives it as the device host does: keys at the focused root, presses at
//! buttons, attitude through the announced topic, time through the clock.
use exact_kernel::{Kernel, Offer};
use exact_runner::{Event, Runner};
use level_data::{Attitude, LevelData, Shared};

pub struct App {
    runner: Runner<LevelData>,
    shared: Shared,
    now: f64,
    /// Host commands the app emitted, by name, oldest first.
    pub commands: Vec<String>,
}

const SOURCE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");

impl App {
    pub fn boot() -> App {
        let path = std::path::Path::new(SOURCE);
        let plan = contract::compile_path(path).unwrap_or_else(|e| panic!("{e}"));
        let shared = Shared::default();
        let data = LevelData::new(shared.clone());
        let kernel = Kernel::with_monospace();
        let viewport = exact_runner::Viewport::sized(468.0, 466.0);
        let mut runner = Runner::boot(plan, data, kernel, viewport, "/")
            .unwrap_or_else(|e| panic!("boot: {e:?}"));
        // The device host's loop wakes on this; the test applies announcements itself.
        runner.listen(std::sync::Arc::new(|| {}));
        let mut app = App {
            runner,
            shared,
            now: 0.0,
            commands: Vec::new(),
        };
        app.settle();
        app
    }

    fn view(&self, test_id: &str) -> exact_kernel::ViewId {
        let kernel = self.runner.kernel();
        let key = *kernel
            .find_by_test_id(test_id)
            .first()
            .unwrap_or_else(|| panic!("no node with testId {test_id}"));
        kernel
            .node_by_key(key)
            .map(|n| n.id)
            .unwrap_or_else(|| panic!("{test_id} is not live"))
    }

    pub fn has(&self, test_id: &str) -> bool {
        !self.runner.kernel().find_by_test_id(test_id).is_empty()
    }

    fn settle(&mut self) {
        for root in self.runner.roots() {
            let _ = self
                .runner
                .kernel_mut()
                .compute_layout(root, Offer::definite(468.0, 466.0));
        }
        for command in self.runner.take_commands() {
            self.commands.push(command.name);
        }
    }

    /// Asks `handlers_of` for every live node, as the device host's focus walk does.
    pub fn walk_handlers(&self) -> usize {
        let kernel = self.runner.kernel();
        let mut stack = self.runner.roots();
        let mut seen = 0;
        while let Some(id) = stack.pop() {
            let _ = self.runner.handlers_of(id);
            seen += 1;
            if let Some(n) = kernel.node(id) {
                stack.extend(n.children());
            }
        }
        seen
    }

    /// A key at the root (the focused element on the device), as BtnA/BtnB send.
    pub fn key(&mut self, key: &str) {
        let root = self.view("level");
        self.runner
            .dispatch(root, Event::key(key))
            .unwrap_or_else(|e| panic!("{key}: {e:?}"));
        self.settle();
    }

    /// A touch press on the element with `test_id`.
    pub fn press(&mut self, test_id: &str) {
        let id = self.view(test_id);
        self.runner
            .dispatch(id, Event::Press)
            .unwrap_or_else(|e| panic!("press: {e:?}"));
        self.settle();
    }

    /// The IMU reports `pitch`/`roll` degrees; the host applies the announcement.
    pub fn tilt(&mut self, pitch: f32, roll: f32) {
        self.shared.publish(Attitude { pitch, roll });
        let (_, error) = self.runner.apply_announced();
        assert!(error.is_none(), "{error:?}");
        self.settle();
    }

    /// Advances the clock by `ms`, firing due timers.
    pub fn advance(&mut self, ms: f64) {
        self.now += ms;
        let advanced = self.runner.advance_timed(self.now);
        assert!(advanced.error.is_none(), "{:?}", advanced.error);
        self.settle();
    }

    /// One field of the agent's `state` answer (slots, then derives), as JSON text.
    pub fn slot(&self, name: &str) -> String {
        let state = exact_runner::agent::handle(&self.runner, r#"{"op":"state"}"#);
        let needle = format!("\"{name}\":");
        let start = state
            .find(&needle)
            .unwrap_or_else(|| panic!("no {name} in {state}"))
            + needle.len();
        let rest = &state[start..];
        let end = rest.find([',', '}']).unwrap_or(rest.len());
        rest[..end].to_string()
    }

    /// Any agent request, answered by the runner (LLP 1012 shapes).
    pub fn agent(&self, request: &str) -> String {
        exact_runner::agent::handle(&self.runner, request)
    }

    /// An element's absolute frame after layout: (x, y, width, height).
    pub fn frame(&self, test_id: &str) -> (f32, f32, f32, f32) {
        let node = self
            .runner
            .kernel()
            .node(self.view(test_id))
            .unwrap_or_else(|| panic!("{test_id}"));
        (
            node.frame.x,
            node.frame.y,
            node.frame.width,
            node.frame.height,
        )
    }

    /// The text inside the element with `test_id` (its first text descendant).
    pub fn text(&self, test_id: &str) -> String {
        let kernel = self.runner.kernel();
        let mut stack = vec![self.view(test_id)];
        while let Some(id) = stack.pop() {
            let Some(node) = kernel.node(id) else {
                continue;
            };
            if let Some(text) = node.props.str(exact_kernel::PropId::Text) {
                return text.to_string();
            }
            stack.extend(node.children().into_iter().rev());
        }
        String::new()
    }
}
