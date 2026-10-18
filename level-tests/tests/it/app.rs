use crate::support::App;

#[test]
fn starts_on_the_bubble_screen_with_zero_focused() {
    let app = App::boot();
    assert_eq!(app.slot("screen"), "\"bubble\"");
    assert_eq!(app.slot("focus"), "\"zero\"");
    assert_eq!(app.slot("zeroed"), "false");
    assert!(app.has("vial") && app.has("bubble"));
    assert_eq!(app.text("zero"), "ZERO: OFF");
}

#[test]
fn arrows_move_the_focus_wrap_and_are_claimed() {
    let mut app = App::boot();
    app.key("ArrowRight");
    assert_eq!(app.slot("focus"), "\"mode\"");
    app.key("ArrowRight");
    assert_eq!(app.slot("focus"), "\"zero\"");
    app.key("ArrowLeft");
    assert_eq!(app.slot("focus"), "\"mode\"");
    let claimed = app
        .commands
        .iter()
        .filter(|c| c.as_str() == "preventDefault")
        .count();
    assert_eq!(claimed, 3, "{:?}", app.commands);
}

#[test]
fn enter_on_mode_switches_screens_and_back() {
    let mut app = App::boot();
    app.key("ArrowRight");
    app.key("Enter");
    assert_eq!(app.slot("screen"), "\"numeric\"");
    assert!(!app.has("vial") && app.has("pitch") && app.has("roll"));
    assert_eq!(app.text("mode"), "MODE: NUMERIC");
    app.key("Enter");
    assert_eq!(app.slot("screen"), "\"bubble\"");
    assert!(app.has("vial"));
}

#[test]
fn enter_on_zero_stores_the_current_tilt_and_escape_clears_it() {
    let mut app = App::boot();
    app.tilt(2.0, -1.0);
    assert_eq!(app.slot("pitch"), "20");
    app.key("Enter");
    assert_eq!(app.slot("zeroed"), "true");
    assert_eq!(app.slot("zeroPitch"), "20");
    assert_eq!(app.slot("zeroRoll"), "-10");
    assert_eq!(app.slot("pitch"), "0");
    assert_eq!(app.text("zero"), "ZERO: SET");
    app.tilt(3.0, -1.0);
    assert_eq!(app.slot("pitch"), "10", "readings are relative to the zero");
    app.key("Escape");
    assert_eq!(app.slot("zeroed"), "false");
    assert_eq!(app.slot("pitch"), "30");
    assert_eq!(app.text("zero"), "ZERO: OFF");
}

#[test]
fn level_has_hysteresis_and_cues_once_per_entry() {
    let mut app = App::boot();
    let steps: [(f32, f32, &str, &str); 6] = [
        (5.0, 0.0, "false", "0"),
        (0.0, 0.0, "true", "1"),
        (2.0, 0.0, "true", "1"),
        (4.0, 0.0, "false", "1"),
        (0.4, -0.3, "true", "2"),
        (0.2, 0.1, "true", "2"),
    ];
    for (pitch, roll, level, cues) in steps {
        app.tilt(pitch, roll);
        app.advance(50.0);
        assert_eq!(app.slot("level"), level, "at {pitch},{roll}");
        assert_eq!(app.slot("cues"), cues, "at {pitch},{roll}");
    }
    let haptics = app
        .commands
        .iter()
        .filter(|c| c.as_str() == "haptic")
        .count();
    assert_eq!(haptics, 2, "{:?}", app.commands);
}

#[test]
fn the_enter_threshold_is_one_degree_and_the_exit_threshold_two_and_a_half() {
    let mut app = App::boot();
    app.tilt(1.1, 0.0);
    app.advance(50.0);
    assert_eq!(app.slot("level"), "false");
    app.tilt(0.0, 1.0);
    app.advance(50.0);
    assert_eq!(app.slot("level"), "true");
    app.tilt(0.0, -2.5);
    app.advance(50.0);
    assert_eq!(app.slot("level"), "true");
    app.tilt(0.0, -2.6);
    app.advance(50.0);
    assert_eq!(app.slot("level"), "false");
}

#[test]
fn touch_runs_the_same_actions_as_keys() {
    let mut app = App::boot();
    app.press("mode");
    assert_eq!(app.slot("focus"), "\"mode\"");
    assert_eq!(app.slot("screen"), "\"numeric\"");
    app.press("zero");
    assert_eq!(app.slot("focus"), "\"zero\"");
    assert_eq!(app.slot("zeroed"), "true");
}

#[test]
fn the_bubble_moves_to_the_raised_side_and_stays_in_the_vial() {
    let mut app = App::boot();
    let (cx, cy, _, _) = app.frame("bubble");
    app.tilt(2.0, 3.0);
    let (x, y, _, _) = app.frame("bubble");
    assert!(
        (x - cx - 21.0).abs() < 0.5,
        "3 degrees right edge up moves right 21 px: {x} vs {cx}"
    );
    assert!(
        (cy - y - 14.0).abs() < 0.5,
        "2 degrees top edge up moves up 14 px: {y} vs {cy}"
    );
    app.tilt(-45.0, 45.0);
    let (x, y, w, _) = app.frame("bubble");
    let (vx, vy, vw, _) = app.frame("vial");
    assert!(
        x >= vx && x + w <= vx + vw && y >= vy,
        "bubble {x},{y} outside vial {vx},{vy}"
    );
}

#[test]
fn screens_switch_while_the_timer_task_runs() {
    let mut app = App::boot();
    for key in [
        "ArrowRight",
        "Enter",
        "Enter",
        "Enter",
        "ArrowLeft",
        "Enter",
        "Escape",
    ] {
        app.advance(400.0);
        app.key(key);
    }
    app.advance(400.0);
    assert_eq!(app.slot("screen"), "\"numeric\"");
}

#[test]
fn screens_switch_while_the_clock_advances_in_small_steps_as_on_the_device() {
    let mut app = App::boot();
    for key in [
        "ArrowRight",
        "Enter",
        "Enter",
        "Enter",
        "ArrowLeft",
        "Enter",
        "Escape",
    ] {
        for _ in 0..200 {
            app.advance(2.0);
        }
        app.key(key);
    }
    assert_eq!(app.slot("screen"), "\"numeric\"");
}

#[test]
fn screens_switch_with_the_device_hosts_handler_walks() {
    let mut app = App::boot();
    for key in [
        "ArrowRight",
        "Enter",
        "Enter",
        "Enter",
        "ArrowLeft",
        "Enter",
        "Escape",
    ] {
        assert!(app.walk_handlers() > 10);
        app.advance(50.0);
        app.key(key);
        assert!(app.walk_handlers() > 10);
    }
    assert_eq!(app.slot("screen"), "\"numeric\"");
}
