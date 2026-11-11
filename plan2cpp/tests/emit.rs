//! plan2cpp on real plans: the level app emits; constructs outside the C++
//! subset are refused by name.
#![allow(clippy::panic, reason = "a test fails by panicking")]

fn plan(source: &str) -> exact_plan::Plan {
    contract::compile(source).unwrap_or_else(|e| panic!("{e}"))
}

#[test]
fn the_level_app_emits_every_table_and_function() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");
    let plan = contract::compile_path(std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    let cpp = plan2cpp::emit(&plan, std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    for name in [
        "kSlots",
        "kDerives",
        "kActions",
        "kBindings",
        "kNodes",
        "kHandlers",
        "kRegions",
        "kTimers",
        "kResources",
        "kApp",
    ] {
        assert!(cpp.contains(name), "missing {name}");
    }
    for i in 0..plan.actions.len() {
        assert!(
            cpp.contains(&format!("static V action_{i}(Ctx& c)")),
            "missing action_{i}"
        );
    }
    assert!(
        cpp.contains("c.command(\"haptic\""),
        "the haptic command is emitted"
    );
}

#[test]
fn a_string_with_non_ascii_text_is_escaped_as_octal() {
    let cpp = plan2cpp::emit(
        &plan("component A\n  view\n    text \"20°\"\n"),
        std::path::Path::new("a.contract"),
    )
    .unwrap_or_else(|e| panic!("{e}"));
    assert!(cpp.contains("\"20\\302\\260\""), "{cpp}");
}

#[test]
fn a_match_region_is_refused_by_name() {
    let source = "component A\n  state o = some(\"x\")\n  view\n    column\n      match o\n        case some(v)\n          text v\n        case none\n          text \"none\"\n";
    let err = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("Match"), "{err}");
}

#[test]
fn the_badge_app_emits_routes_rows_and_owned_slots() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/badge/app.contract");
    let plan = contract::compile_path(std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    let cpp = plan2cpp::emit(&plan, std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    for needle in [
        "{\"setup\", \"/badge/setup\", 3, false, false}",
        "rt::R::Each",
        "region_key_0",
        "st[sp++] = c.item(0);",
        "c.call(13, &st[sp - 1], 2);",
        "c.call(14, &st[sp - 1], 1);",
        "{rt::E::Navigate, 1, 0, 0}",
        "{rt::E::Pan, 13, 0, 0}",
        "{rt::E::PanRelease, 14, 0, 0}",
        "c.send(0, \"setBrightness\", st + sp, 1);",
        "st[sp++] = c.now();",
        "const int16_t kSlotOwners[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 9, 9, 9, 9, 73, 73, 73, -1};",
        "{\"notfound\", \"\", -1, false, true}",
        "kRoutes, 9, 0, kGlyphs",
    ] {
        assert!(cpp.contains(needle), "missing {needle}");
    }
}

#[test]
fn every_route_call_is_a_roster_entry() {
    for name in plan2cpp::ROUTE_CALLS {
        let found = (0..=u8::MAX)
            .filter_map(exact_plan::Stdlib::from_wire)
            .any(|call| call.name() == name);
        assert!(found, "{name} is not in the stdlib roster");
    }
}

#[test]
fn an_unsupported_style_is_refused_by_name() {
    let source = "component A\n  view\n    text \"x\" letter-spacing=0.3\n";
    let err = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("LetterSpacing"), "{err}");
}

#[test]
fn an_unsupported_event_is_refused_by_name() {
    let source = "component A\n  state n = 0\n  action moved\n    n = n + 1\n  view\n    column pointermove=moved\n";
    let err = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default();
    assert!(
        err.contains("Pointermove") || err.contains("event"),
        "{err}"
    );
}

#[test]
fn the_level_app_needs_keys_touch_and_haptic() {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");
    let plan = contract::compile_path(std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    let needs = plan2cpp::needs(&plan).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(
        needs,
        plan2cpp::Needs {
            keys: true,
            touch: true,
            commands: vec!["haptic".to_string()],
        }
    );
}

#[test]
fn an_app_without_controls_or_commands_needs_none() {
    let needs = plan2cpp::needs(&plan("component A\n  view\n    text \"hi\"\n"))
        .unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(
        needs,
        plan2cpp::Needs {
            keys: false,
            touch: false,
            commands: Vec::new(),
        }
    );
    let cpp = plan2cpp::emit(
        &plan("component A\n  view\n    text \"hi\"\n"),
        std::path::Path::new("a.contract"),
    )
    .unwrap_or_else(|e| panic!("{e}"));
    assert!(cpp.contains("kCommands[] = {nullptr}"), "{cpp}");
    assert!(
        cpp.contains(", false, false, kCommands, 0, kTypes"),
        "{cpp}"
    );
}

#[test]
fn prevent_default_is_not_a_board_command() {
    let source = "component A\n  action k(key: string)\n    preventDefault()\n  view\n    column key=k autofocus=true\n";
    let needs = plan2cpp::needs(&plan(source)).unwrap_or_else(|e| panic!("{e}"));
    assert!(needs.keys, "a key handler needs keys");
    assert!(needs.commands.is_empty(), "{:?}", needs.commands);
}

fn badge() -> (exact_plan::Plan, String) {
    let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/badge/app.contract");
    let plan = contract::compile_path(std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    let cpp = plan2cpp::emit(&plan, std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
    (plan, cpp)
}

#[test]
fn the_badge_app_rasterizes_only_the_faces_and_sizes_its_text_uses() {
    let (plan, cpp) = badge();
    let faces = plan2cpp::faces_needed(&plan).unwrap_or_else(|e| panic!("{e}"));
    let mut used = Vec::new();
    for (face, sizes) in &faces {
        let weight = plan.faces[*face].weight;
        for px16 in sizes {
            used.push((weight, px16 / 16));
        }
    }
    // Inter Medium at 14, 16, 20, 24; Plex Mono Regular 20, 24, Medium 12,
    // 18, 24, 32, SemiBold 12.
    assert_eq!(
        used,
        [
            (500, 14),
            (500, 16),
            (500, 20),
            (500, 24),
            (400, 20),
            (400, 24),
            (500, 12),
            (500, 18),
            (500, 24),
            (500, 32),
            (600, 12)
        ]
    );
    assert!(
        cpp.contains("constexpr uint16_t kFaceCount = 11;"),
        "face count"
    );
    // Inter has kerning; Plex Mono, a monospace face, has none.
    assert!(
        cpp.contains("const rt::FontFile kFontFiles[] = {\n  {0, 1238, 2048},\n  {1238, 0, 1000},"),
        "kerning"
    );
}

#[test]
fn images_are_masks_when_one_color_and_resampled_to_their_one_size() {
    let (_, cpp) = badge();
    // A 57 x 13 white mark with alpha: a mask in its color.
    assert!(
        cpp.contains(
            "{\"assets/images/logo_small.png\", 57, 13, 2, 0xffffff, nullptr, kImageAlpha"
        ),
        "mask"
    );
    // The 2400 x 2400 artwork, drawn only at 300 x 300: opaque RGB565 at that size.
    assert!(cpp.contains("{\"assets/images/developers-after-dark.png\", 300, 300, 0, 0x000000, kImageRgb0, nullptr}"), "resampled");
}

#[test]
fn css_weight_matching_picks_the_nearest_face_in_its_direction() {
    assert_eq!(plan2cpp::pick_weight(&[400, 500, 600], 500), Some(500));
    assert_eq!(plan2cpp::pick_weight(&[400, 500, 600], 700), Some(600));
    assert_eq!(plan2cpp::pick_weight(&[400, 600], 450), Some(400));
    assert_eq!(plan2cpp::pick_weight(&[300, 500], 400), Some(500));
    assert_eq!(plan2cpp::pick_weight(&[400, 600], 300), Some(400));
    assert_eq!(plan2cpp::pick_weight(&[300, 400], 650), Some(400));
    assert_eq!(plan2cpp::pick_weight(&[], 400), None);
}

#[test]
fn text_in_a_generic_family_uses_the_boards_faces() {
    let (_, level) = {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../apps/level/app.contract");
        let plan =
            contract::compile_path(std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
        let cpp =
            plan2cpp::emit(&plan, std::path::Path::new(path)).unwrap_or_else(|e| panic!("{e}"));
        (plan, cpp)
    };
    assert!(
        level.contains("constexpr uint16_t kFaceCount = 0;"),
        "level faces"
    );
    let cpp = plan2cpp::emit(
        &plan("component A\n  view\n    text \"x\" font-family=\"monospace\"\n"),
        std::path::Path::new("a.contract"),
    )
    .unwrap_or_else(|e| panic!("{e}"));
    assert!(
        cpp.contains("constexpr uint16_t kFaceCount = 0;"),
        "generic family"
    );
}

#[test]
fn an_image_outside_the_apps_assets_is_refused_by_name() {
    let source =
        "component A\n  view\n    image \"https://example.com/a.png\" width=10 height=10\n";
    let err = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("not a file under the app's assets/"), "{err}");
}

#[test]
fn a_mutation_that_refreshes_is_refused_by_name() {
    let source = "shape Done\n  ok: bool\nshape Items\n  n: number\ncomponent A\n  resource items = listItems() as shape Items\n  mutation saved as shape Done refreshes items\n  action save\n    send saved = saveItem(1)\n  view\n    button press=save\n      text \"save\"\n";
    let err = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default();
    assert!(err.contains("mutation saved refreshes"), "{err}");
}

fn refusal(source: &str) -> String {
    plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .err()
        .unwrap_or_default()
}

const SHAPE_P: &str = "shape P\n  n: number\n";

#[test]
fn a_resource_asked_for_each_item_is_refused_by_name() {
    let source = format!(
        "{SHAPE_P}component A\n  state ids = [1, 2]\n  resource got = load(u) for u in ids as shape P\n  view\n    text \"a\"\n"
    );
    let err = refusal(&source);
    assert!(
        err.contains("resource got asks once for each item"),
        "{err}"
    );
}

fn tokens(case: &str) -> Result<String, String> {
    let path = format!(
        "{}/tests/tokens/{case}/app.contract",
        env!("CARGO_MANIFEST_DIR")
    );
    let path = std::path::Path::new(&path);
    let plan = contract::compile_path(path).unwrap_or_else(|e| panic!("{e}"));
    plan2cpp::emit(&plan, path)
}

#[test]
fn fixed_design_tokens_become_the_boards_token_table() {
    let cpp = tokens("fixed").unwrap_or_else(|e| panic!("{e}"));
    assert!(
        cpp.contains(r#"const rt::Token kTokens[] = {{"surface.page", 0xff101010}, {"ink.on", 0xff48ef8b}, {"ink.off", 0xffffffff}};"#),
        "{cpp}"
    );
    assert!(cpp.contains("kTokens, 3, kSounds"), "{cpp}");
    // A row and a value made at run time both name the token; the runtime looks it up.
    assert!(cpp.contains(r#"rt::Lit::str("surface.page")"#), "{cpp}");
    assert!(cpp.contains(r#"V::str("ink.off")"#), "{cpp}");
}

#[test]
fn an_app_without_a_design_has_an_empty_token_table() {
    let cpp = plan2cpp::emit(
        &plan("component A\n  view\n    text \"x\"\n"),
        std::path::Path::new("a.contract"),
    )
    .unwrap_or_else(|e| panic!("{e}"));
    assert!(
        cpp.contains("const rt::Token kTokens[] = {{nullptr, 0}};"),
        "{cpp}"
    );
    assert!(
        cpp.contains("kTokens, 0, kSounds, 0, kSequences, 0, kEaseStops, kEases, kKeyValues, kAnimations, kAnimationValues, 0};"),
        "{cpp}"
    );
}

#[test]
fn design_tokens_the_board_cannot_show_are_refused_by_name() {
    let refused = |case: &str| tokens(case).err().unwrap_or_default();
    assert!(
        refused("scheme").contains("design token surface.page is `light-dark("),
        "{}",
        refused("scheme")
    );
    assert!(
        refused("axis").contains("the app has app axes"),
        "{}",
        refused("axis")
    );
    assert!(
        refused("role").contains("AccentColor overrides the role"),
        "{}",
        refused("role")
    );
    assert!(
        refused("length").contains("space.card is not a colour"),
        "{}",
        refused("length")
    );
}

fn feature(case: &str) -> String {
    let path = format!(
        "{}/tests/features/{case}/app.contract",
        env!("CARGO_MANIFEST_DIR")
    );
    let path = std::path::Path::new(&path);
    let plan = contract::compile_path(path).unwrap_or_else(|e| panic!("{e}"));
    plan2cpp::emit(&plan, path).unwrap_or_else(|e| panic!("{e}"))
}

#[test]
fn sounds_become_samples_and_a_sequence_its_functions() {
    let sound = feature("sound");
    assert!(
        sound.contains("static const int16_t kSound0[] = {0, "),
        "{sound}"
    );
    assert!(
        sound.contains(
            r#"const rt::Sound kSounds[] = {{"assets/click.wav", kSound0, 662, 22050}};"#
        ),
        "{sound}"
    );
    let sequence = feature("sequence");
    assert!(
        sequence.contains(r#"const rt::Sequence kSequences[] = {{"pulse", sequence_gate_0, sequence_tracks_0, sequence_bpm_0}};"#),
        "{sequence}"
    );
    // The tracks are built in code: a Step of 9 fields, a Track of 5, and two lists.
    assert!(
        sequence.contains("sp -= 9; st[sp] = V::record("),
        "{sequence}"
    );
    assert!(
        sequence.contains("sp -= 5; st[sp] = V::record("),
        "{sequence}"
    );
    assert!(sequence.contains("st[sp] = V::list("), "{sequence}");
}

/// The sequence test app with its `sequence` lines replaced by `lines`.
fn sequence_with(lines: &str) -> Result<String, String> {
    let path = format!(
        "{}/tests/features/sequence/app.contract",
        env!("CARGO_MANIFEST_DIR")
    );
    let text = std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{e}"));
    let start = text
        .find("  sequence ")
        .unwrap_or_else(|| panic!("no sequence"));
    let end = text.find("  view").unwrap_or_else(|| panic!("no view"));
    let source = format!(
        "{}  state take = 0\n  action done\n    take = take + 1\n{lines}{}",
        &text[..start],
        &text[end..]
    );
    plan2cpp::emit(&plan(&source), std::path::Path::new("a.contract"))
}

#[test]
fn sequence_forms_the_board_does_not_play_are_refused_by_name() {
    let tracks = "[Track(beats=1, loops=0, offset=0, output=\"\", steps=[])]";
    let cases = [
        (format!("  sequence pulse when playing\n    play({tracks}, bpm=60, from=-4)\n"), "`from=` (a count-in)"),
        (format!("  sequence pulse when playing key=take\n    play({tracks}, bpm=60)\n"), "is keyed"),
        (format!("  sequence pulse when playing\n    play({tracks}, bpm=60, ended=done)\n"), "runs an `ended=` action"),
        (format!("  sequence pulse mount\n    play({tracks}, bpm=60)\n"), "has no `when` gate"),
        (
            format!("  sequence a when playing\n    play({tracks}, bpm=60)\n  sequence b when playing\n    play({tracks}, bpm=90)\n"),
            "declares 2 sequences",
        ),
    ];
    for (lines, why) in cases {
        let err = sequence_with(&lines).err().unwrap_or_default();
        assert!(err.contains(why), "{why}: {err}");
    }
}

#[test]
fn animation_longhands_play_as_their_shorthand() {
    // The compiler writes the longhands as one shorthand literal, which the
    // board finds in its table.
    let source = "keyframes rise\n  from opacity=0\n  to opacity=1\n\ncomponent A\n  view\n    text \"x\" animation-name=\"rise\" animation-duration=\"1s\"\n";
    let cpp = plan2cpp::emit(&plan(source), std::path::Path::new("a.contract"))
        .unwrap_or_else(|e| panic!("{e}"));
    assert!(
        cpp.contains(r#"{"1s ease 0s 1 normal none running rise", 1, 1}"#),
        "{cpp}"
    );
}
