//! Host extensions: the repo's extension files read, apps' sources checked
//! against them, and the refusals for files and apps that break the rules.
#![allow(clippy::panic, reason = "a test fails by panicking")]

use std::path::{Path, PathBuf};

use plan2cpp::ext;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}

/// A fresh directory with `files` (relative path, contents) written in it.
fn scratch(name: &str, files: &[(&str, &str)]) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("plan2cpp-ext-{name}-{}", std::process::id()));
    if dir.exists() {
        std::fs::remove_dir_all(&dir).unwrap_or_else(|e| panic!("{e}"));
    }
    for (path, text) in files {
        let path = dir.join(path);
        let parent = path
            .parent()
            .unwrap_or_else(|| panic!("{} has no parent", path.display()));
        std::fs::create_dir_all(parent).unwrap_or_else(|e| panic!("{e}"));
        std::fs::write(&path, text).unwrap_or_else(|e| panic!("{e}"));
    }
    dir
}

fn compile(path: &Path) -> exact_plan::Plan {
    contract::compile_path(path).unwrap_or_else(|e| panic!("{e}"))
}

/// The error of emitting the app in `dir`, or a panic if it emits.
fn refusal(dir: &Path) -> String {
    let source = dir.join("app.contract");
    plan2cpp::emit(&compile(&source), &source)
        .err()
        .unwrap_or_else(|| panic!("{} emitted", source.display()))
}

const BATTERY_APP: &str = "shape Battery\n  percent: number\ncomponent A\n  resource battery = boardBattery() as shape Battery\n  view\n    text `${battery.percent}`\n";

fn battery_ext(field_type: &str) -> String {
    format!(
        r#"{{"extension": "board", "version": 1, "sources": [{{"name": "boardBattery", "live": true, "result": {{"percent": "{field_type}"}}}}]}}"#
    )
}

#[test]
fn the_repo_extensions_read_and_the_apps_need_them() {
    for (app, name, sources, ops) in [
        ("level", "attitude", vec!["attitude"], vec!["tilt"]),
        (
            "badge",
            "init-badge",
            vec![
                "badgeProfile",
                "badgeSettings",
                "badgeBattery",
                "setBrightness",
            ],
            vec!["panel"],
        ),
    ] {
        let source = repo().join("apps").join(app).join("app.contract");
        let exts = ext::roster(&source).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(exts.len(), 1, "{app}");
        assert_eq!(exts[0].name, name);
        let names: Vec<&str> = exts[0].sources.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, sources, "{app}");
        let op_names: Vec<&str> = exts[0].ops.iter().map(|o| o.name.as_str()).collect();
        assert_eq!(op_names, ops, "{app}");
        let uses = ext::check(&compile(&source), &exts).unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(
            uses.len(),
            sources.len(),
            "{app}: every declared source is used"
        );
    }
}

#[test]
fn the_level_apps_needs_list_its_board_parts_and_extension() {
    let source = repo().join("apps/level/app.contract");
    let exts = ext::roster(&source).unwrap_or_else(|e| panic!("{e}"));
    let props = ext::props(&source, &exts).unwrap_or_else(|e| panic!("{e}"));
    let json = ext::needs_json(&compile(&source), &exts, &props).unwrap_or_else(|e| panic!("{e}"));
    let v: serde_json::Value =
        serde_json::from_str(&json).unwrap_or_else(|e| panic!("{e}: {json}"));
    assert_eq!(v["view"], true);
    assert_eq!(v["keys"], true);
    assert_eq!(v["touch"], true);
    assert_eq!(v["commands"], serde_json::json!(["haptic"]));
    let e = &v["extensions"][0];
    assert_eq!(e["name"], "attitude");
    assert_eq!(e["version"], 1);
    assert_eq!(e["sha256"].as_str().map(str::len), Some(64));
    assert_eq!(
        e["sources"],
        serde_json::json!([{"name": "attitude", "params": {}, "result": {"pitch": "number", "roll": "number"}, "live": true}])
    );
    assert_eq!(
        e["ops"],
        serde_json::json!([{"name": "tilt", "params": {"pitch": "number", "roll": "number"}}])
    );
    assert_eq!(e["props"], serde_json::json!([]));
}

/// The committed C++ base classes are what the extension files make now.
#[test]
fn the_committed_extension_headers_match_their_files() {
    for (file, header) in [
        ("extensions/attitude.json", "apps/level/cpp/ext_attitude.h"),
        ("extensions/init-badge.json", "apps/badge/cpp/ext_init_badge.h"),
        (
            "rt-test/marks/extensions/marks.json",
            "rt-test/marks/ext_marks.h",
        ),
    ] {
        let e = ext::read(&repo().join(file)).unwrap_or_else(|e| panic!("{e}"));
        let committed =
            std::fs::read_to_string(repo().join(header)).unwrap_or_else(|e| panic!("{e}"));
        let made = ext::header(&e).replace(&repo().join(file).display().to_string(), file);
        assert_eq!(
            committed, made,
            "{header} is stale; run plan2cpp ext {file} {header}"
        );
    }
}

#[test]
fn a_header_has_a_typed_method_per_source_and_op() {
    let e = ext::read(&repo().join("extensions/init-badge.json")).unwrap_or_else(|e| panic!("{e}"));
    let h = ext::header(&e);
    for needle in [
        "class InitBadgeExtension : public rt::Extension {",
        "struct BadgeSettings { double brightness = 0; std::string orientation; bool unlocked = false; };",
        "virtual bool set_brightness(double level, SetBrightness& out) = 0;",
        "argc == 1 && args[0].k == rt::V::K::Num",
        "virtual std::string panel(rt::Runtime& runtime) = 0;",
        "void badge_battery_changed(rt::Runtime& runtime) { runtime.changed(\"badgeBattery\"); }",
    ] {
        assert!(h.contains(needle), "missing {needle}\n{h}");
    }
    assert!(
        !h.contains("badge_profile_changed"),
        "only a live source has a change call"
    );
}

#[test]
fn a_source_no_listed_extension_declares_is_refused() {
    let dir = scratch("undeclared", &[("app.contract", BATTERY_APP)]);
    let err = refusal(&dir);
    assert!(
        err.contains("`boardBattery`") && err.contains("listed: none"),
        "{err}"
    );
}

#[test]
fn a_declared_source_binds_and_is_recorded() {
    let dir = scratch(
        "declared",
        &[
            ("app.contract", BATTERY_APP),
            ("extensions.json", r#"{"extensions": {"board": 1}}"#),
            ("extensions/board.json", &battery_ext("number")),
        ],
    );
    let source = dir.join("app.contract");
    let cpp = plan2cpp::emit(&compile(&source), &source).unwrap_or_else(|e| panic!("{e}"));
    assert!(
        cpp.contains("const rt::ExtensionUse kExtensions[] = {{\"board\", 1, \""),
        "{cpp}"
    );
    assert!(
        cpp.contains("const rt::ExtensionSource kExtensionSources[] = {{\"boardBattery\", 0, "),
        "{cpp}"
    );
}

#[test]
fn a_field_of_another_type_is_refused() {
    let dir = scratch(
        "mistyped",
        &[
            ("app.contract", BATTERY_APP),
            ("extensions.json", r#"{"extensions": {"board": 1}}"#),
            ("extensions/board.json", &battery_ext("string")),
        ],
    );
    let err = refusal(&dir);
    assert!(
        err.contains("`percent` as Number") && err.contains("declares string"),
        "{err}"
    );
}

#[test]
fn a_field_the_extension_lacks_is_refused() {
    let app = BATTERY_APP.replace("percent: number", "percent: number\n  charging: bool");
    let dir = scratch(
        "extra-field",
        &[
            ("app.contract", &app),
            ("extensions.json", r#"{"extensions": {"board": 1}}"#),
            ("extensions/board.json", &battery_ext("number")),
        ],
    );
    let err = refusal(&dir);
    assert!(err.contains("field `charging`"), "{err}");
}

#[test]
fn a_send_with_another_argument_count_is_refused() {
    let app = "shape Done\n  ok: bool\ncomponent A\n  mutation m as shape Done\n  action go\n    send m = setLevel(1, 2)\n  view\n    button \"Go\" press=go\n";
    let decl = r#"{"extension": "panel", "version": 1, "sources": [{"name": "setLevel", "params": [{"name": "level", "type": "number"}], "result": {"ok": "bool"}}]}"#;
    let dir = scratch(
        "argc",
        &[
            ("app.contract", app),
            ("extensions.json", r#"{"extensions": {"panel": 1}}"#),
            ("extensions/panel.json", decl),
        ],
    );
    let err = refusal(&dir);
    assert!(
        err.contains("gives 2 arguments") && err.contains("declares 1"),
        "{err}"
    );
}

#[test]
fn a_roster_asking_another_version_is_refused() {
    let dir = scratch(
        "version",
        &[
            ("app.contract", BATTERY_APP),
            ("extensions.json", r#"{"extensions": {"board": 2}}"#),
            ("extensions/board.json", &battery_ext("number")),
        ],
    );
    let err = ext::roster(&dir.join("app.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("asks for board version 2"), "{err}");
}

#[test]
fn two_extensions_declaring_one_source_are_refused() {
    let other = battery_ext("number").replace("\"board\"", "\"spare\"");
    let dir = scratch(
        "collision",
        &[
            ("app.contract", BATTERY_APP),
            (
                "extensions.json",
                r#"{"extensions": {"board": 1, "spare": 1}}"#,
            ),
            ("extensions/board.json", &battery_ext("number")),
            ("extensions/spare.json", &other),
        ],
    );
    let err = ext::roster(&dir.join("app.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("both declare source `boardBattery`"), "{err}");
}

#[test]
fn an_extension_file_that_breaks_a_rule_is_refused_by_name() {
    let cases = [
        (
            r#"{"extension": "x", "version": 1, "colour": 1}"#,
            "unknown key `colour`",
        ),
        (r#"{"extension": "Board", "version": 1}"#, "lowercase words"),
        (
            r#"{"extension": "x", "version": 0}"#,
            "`version` is a whole number",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "exactTime", "result": {}}]}"#,
            "does not start with `exact`",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "a", "result": {"n": "int"}}]}"#,
            "type `int`",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "a", "result": {"class": "bool"}}]}"#,
            "not a name C++ can use",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "a", "result": {}}, {"name": "a", "result": {}}]}"#,
            "declared twice",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "xExtension", "result": {}}]}"#,
            "share its C++ name",
        ),
        (
            r#"{"extension": "x", "version": 1, "ops": [{"name": "state"}]}"#,
            "not one of the runtime's",
        ),
        (
            r#"{"extension": "x", "version": 1, "ops": [{"name": "go", "params": [{"name": "on", "type": "bool"}]}]}"#,
            "numbers or strings",
        ),
        (
            r#"{"extension": "x", "version": 1, "props": [{"name": "Snap"}]}"#,
            "a prop is a `data-*` word",
        ),
        (
            r#"{"extension": "x", "version": 1, "props": [{"name": "snap"}, {"name": "snap"}]}"#,
            "prop `snap` is declared twice",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "dataSnap", "result": {}}], "props": [{"name": "snap"}]}"#,
            "its C++ method `data_snap` is source `dataSnap`'s",
        ),
        (
            r#"{"extension": "x", "version": 1, "sources": [{"name": "data", "live": true, "result": {}}], "props": [{"name": "changed"}]}"#,
            "its C++ method `data_changed` is source `data`'s",
        ),
    ];
    for (i, (text, want)) in cases.iter().enumerate() {
        let dir = scratch(&format!("rule-{i}"), &[("x.json", text)]);
        let err = ext::read(&dir.join("x.json")).err().unwrap_or_default();
        assert!(err.contains(want), "case {i}: {err}");
    }
}

const MARK_APP: &str = "component A\n  state n = 1\n  view\n    column testId=\"c\" data-snap=\"y\" data-count=n\n      text \"x\"\n";
const MARK_MANIFEST: &str =
    r#"{"app": {"id": "com.example.marks", "name": "marks"}, "data": ["snap", "count"]}"#;

fn marks_ext(name: &str, props: &str) -> String {
    format!(r#"{{"extension": "{name}", "version": 1, "props": [{props}]}}"#)
}

#[test]
fn the_marks_apps_words_bind_to_its_extension_and_reach_the_runtime() {
    let source = repo().join("rt-test/marks/app.contract");
    let exts = ext::roster(&source).unwrap_or_else(|e| panic!("{e}"));
    let props = ext::props(&source, &exts).unwrap_or_else(|e| panic!("{e}"));
    let words: Vec<&str> = props.iter().map(|p| p.word.as_str()).collect();
    assert_eq!(words, ["snap", "snap-align", "mode", "count"]);
    let plan = compile(&source);
    let json = ext::needs_json(&plan, &exts, &props).unwrap_or_else(|e| panic!("{e}"));
    let v: serde_json::Value =
        serde_json::from_str(&json).unwrap_or_else(|e| panic!("{e}: {json}"));
    assert_eq!(
        v["extensions"][0]["props"],
        serde_json::json!(["snap", "snap-align", "mode", "count"])
    );
    let cpp = plan2cpp::emit(&plan, &source).unwrap_or_else(|e| panic!("{e}"));
    for needle in [
        "static_cast<uint8_t>(rt::P::Dataset)",
        "sp -= 4; st[sp] = rt::native_props(&st[sp], 2); ++sp;",
    ] {
        assert!(cpp.contains(needle), "missing {needle}");
    }
    let committed = std::fs::read_to_string(repo().join("rt-test/marks/app_gen.cpp"))
        .unwrap_or_else(|e| panic!("{e}"));
    let made = cpp.replace(&source.display().to_string(), "rt-test/marks/app.contract");
    assert_eq!(
        committed, made,
        "rt-test/marks/app_gen.cpp is stale; run plan2cpp emit"
    );
}

#[test]
fn a_header_has_a_reader_per_prop() {
    let e = ext::read(&repo().join("rt-test/marks/extensions/marks.json"))
        .unwrap_or_else(|e| panic!("{e}"));
    let h = ext::header(&e);
    for needle in [
        "static constexpr const char* kProps[] = {\"snap\", \"snap-align\", \"mode\", \"count\"};",
        "std::vector<rt::Mark> data_snap_align(const rt::Runtime& runtime) const { return runtime.marks(\"snap-align\"); }",
        "(void)argc;",
    ] {
        assert!(h.contains(needle), "missing {needle}\n{h}");
    }
}

#[test]
fn a_word_no_listed_extension_declares_is_refused() {
    let dir = scratch(
        "word-undeclared",
        &[
            ("app.contract", MARK_APP),
            ("app.json", MARK_MANIFEST),
            ("extensions.json", r#"{"extensions": {"marks": 1}}"#),
            (
                "extensions/marks.json",
                &marks_ext("marks", r#"{"name": "snap"}"#),
            ),
        ],
    );
    let err = refusal(&dir);
    assert!(
        err.contains("`data-count`") && err.contains("listed: marks"),
        "{err}"
    );
}

#[test]
fn a_word_with_no_extensions_listed_is_refused() {
    let dir = scratch(
        "word-no-roster",
        &[("app.contract", MARK_APP), ("app.json", MARK_MANIFEST)],
    );
    let err = refusal(&dir);
    assert!(
        err.contains("`data-snap`") && err.contains("listed: none"),
        "{err}"
    );
}

#[test]
fn two_extensions_declaring_one_prop_are_refused() {
    let dir = scratch(
        "word-collision",
        &[
            ("app.contract", MARK_APP),
            ("app.json", MARK_MANIFEST),
            (
                "extensions.json",
                r#"{"extensions": {"marks": 1, "spare": 1}}"#,
            ),
            (
                "extensions/marks.json",
                &marks_ext("marks", r#"{"name": "snap"}, {"name": "count"}"#),
            ),
            (
                "extensions/spare.json",
                &marks_ext("spare", r#"{"name": "snap"}"#),
            ),
        ],
    );
    let err = ext::roster(&dir.join("app.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("both declare prop `snap`"), "{err}");
}
