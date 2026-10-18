//! Diagnostic: which runner-visible lists grow with announcements. Ignored by default.
use crate::support::App;

#[test]
#[ignore = "diagnostic"]
fn agent_answer_sizes_by_round() {
    let mut app = App::boot();
    for round in 0..3 {
        for i in 0..1000 {
            app.tilt(if i % 2 == 0 { 0.3 } else { -0.4 }, 0.0);
        }
        let sizes: Vec<String> = ["logs", "frames", "holds", "perf", "state", "tree"]
            .iter()
            .map(|op| format!("{op}={}", app.agent(&format!("{{\"op\":\"{op}\"}}")).len()))
            .collect();
        println!(
            "GROWTH after {} announcements: {}",
            (round + 1) * 1000,
            sizes.join(" ")
        );
    }
}
