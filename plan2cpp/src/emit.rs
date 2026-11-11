//! The app's C++: one function per code body and constant tables for the
//! runtime (`exact_rt.h`). Everything outside the C++ target's subset is refused
//! here, by name, so the device never meets a construct it cannot run.

use std::fmt::Write;
use std::path::Path;

use exact_kernel::{NodeType, PropId, StyleId};
use exact_plan::{BindingKind, Code, EventKind, Plan, RegionKind, TypeKind};

use crate::anim;
use crate::assets;
use crate::code::{self, cpp_string, function};
use crate::ext;
use crate::resources;
use crate::sound;
use crate::theme;

/// Style rows the runtime lays out and paints, by Exact name.
const STYLES: &[&str] = &[
    "Width",
    "Height",
    "PaddingTop",
    "PaddingRight",
    "PaddingBottom",
    "PaddingLeft",
    "FlexDirection",
    "JustifyContent",
    "AlignItems",
    "PositionType",
    "Top",
    "Right",
    "Bottom",
    "Left",
    "Display",
    "BoxSizing",
    "BorderWidthTop",
    "BorderWidthRight",
    "BorderWidthBottom",
    "BorderWidthLeft",
    "BackgroundColor",
    "BorderRadiusTopLeft",
    "BorderRadiusTopRight",
    "BorderRadiusBottomRight",
    "BorderRadiusBottomLeft",
    "BorderColorTop",
    "BorderColorRight",
    "BorderColorBottom",
    "BorderColorLeft",
    "FontSize",
    "FontWeight",
    "TextAlign",
    "TextColor",
    "BorderStyleTop",
    "BorderStyleRight",
    "BorderStyleBottom",
    "BorderStyleLeft",
    "Appearance",
    "MarginTop",
    "MarginRight",
    "MarginBottom",
    "MarginLeft",
    "RowGap",
    "ColumnGap",
    "Opacity",
    "LineHeight",
    "FontFamily",
    "ObjectFit",
    "OverflowX",
    "OverflowY",
    "WhiteSpace",
    "TextOverflow",
    "Animation",
];

/// Props the runtime reads; accessibility props are kept by name and not painted.
fn prop(id: u16) -> Result<&'static str, String> {
    let p = PropId::from_wire(id).ok_or_else(|| format!("prop {id} is unknown"))?;
    Ok(match p {
        PropId::Text => "rt::P::Text",
        PropId::ImageSource => "rt::P::Source",
        PropId::TestId => "rt::P::TestId",
        PropId::Autofocus => "rt::P::Autofocus",
        PropId::Id => "rt::P::Id",
        PropId::NavigationKey => "rt::P::NavigationKey",
        PropId::NavigationBack => "rt::P::NavigationBack",
        PropId::NavigationPresentation => "rt::P::NavigationPresentation",
        PropId::Closedby => "rt::P::Closedby",
        PropId::Dataset => "rt::P::Dataset",
        PropId::AccessibilityRole => "rt::P::Role",
        PropId::AccessibilityControls => "rt::P::Controls",
        PropId::AccessibilityHint
        | PropId::AccessibilityLabel
        | PropId::AccessibilityHeadingLevel
        | PropId::SemanticTag => "rt::P::Ignored",
        other => return Err(format!("the prop {other:?} is not in the C++ target")),
    })
}

fn style(id: u16) -> Result<String, String> {
    let s = StyleId::from_bit(u32::from(id)).ok_or_else(|| format!("style bit {id} is unknown"))?;
    let name = format!("{s:?}");
    if STYLES.contains(&name.as_str()) {
        Ok(format!("rt::S::{name}"))
    } else {
        Err(format!("the style {name} is not in the C++ target"))
    }
}

/// The literal a one-push body yields, as a runtime `Lit`, or `None` if dynamic.
fn literal(plan: &Plan, body: Code) -> Option<String> {
    let code: Vec<_> = exact_runner::vm::instructions(plan.code(body))
        .collect::<Result<_, _>>()
        .ok()?;
    let [first, ret] = code.as_slice() else {
        return None;
    };
    if ret.op != exact_plan::Opcode::Return {
        return None;
    }
    let id = u32::try_from(first.args[0]).ok();
    Some(match first.op {
        exact_plan::Opcode::Number => format!("rt::Lit::num({:?})", first.number),
        exact_plan::Opcode::Bool => format!("rt::Lit::boolean({})", first.args[0] != 0),
        exact_plan::Opcode::Str => format!(
            "rt::Lit::str({})",
            cpp_string(plan.str(exact_plan::StrId(id?)))
        ),
        exact_plan::Opcode::None | exact_plan::Opcode::Unit => "rt::Lit::none()".into(),
        _ => return None,
    })
}

/// Writes the code functions and returns their names, in table order.
struct Out {
    functions: String,
}

impl Out {
    fn body(&mut self, plan: &Plan, body: Code, name: &str) -> Result<String, String> {
        self.functions.push_str(&function(plan, body, name)?);
        self.functions.push('\n');
        Ok(name.to_string())
    }
}

/// The items of a C++ array; `nullptr` for none, as C++ has no empty array.
fn row(items: &[String]) -> String {
    if items.is_empty() {
        "nullptr".into()
    } else {
        items.join(", ")
    }
}

fn names<I: Iterator<Item = String>>(items: I) -> String {
    row(&items.map(|s| cpp_string(&s)).collect::<Vec<_>>())
}

fn check_plan(plan: &Plan) -> Result<(), String> {
    for s in &plan.slots {
        if s.late {
            return Err(format!(
                "slot {} is a late child slot; not in the C++ target",
                plan.str(s.name)
            ));
        }
    }
    for r in &plan.resources {
        if r.each.len > 0 {
            return Err(format!(
                "resource {} asks once for each item of a list (`for`); not in the C++ target",
                plan.str(r.name)
            ));
        }
    }
    for t in &plan.timers {
        if t.gated || t.frame || t.keyed {
            return Err(format!(
                "task {} is gated, keyed, or per frame; not in the C++ target",
                plan.str(t.name)
            ));
        }
    }
    for r in &plan.regions {
        let admitted = match r.kind {
            RegionKind::When => r.arms.len <= 2,
            RegionKind::Each => r.arms.len == 1,
            RegionKind::Match => false,
        };
        if !admitted {
            return Err(format!("a {:?} region is not in the C++ target", r.kind));
        }
    }
    for m in &plan.mutations {
        if m.refreshes.len > 0 || m.then.is_some() || m.queue {
            return Err(format!(
                "mutation {} refreshes, runs `then`, or queues; not in the C++ target",
                plan.str(m.name)
            ));
        }
    }
    if let Some(router) = plan.router {
        if router.0 != 0 {
            return Err("a routes slot other than slot 0 is not in the C++ target".into());
        }
    }
    sound::check_sequences(plan)?;
    unsupported_features(plan)
}

/// Refuses an app that uses a feature whose plan table the C++ target does not read, so the
/// board never runs it without the feature. Hatches are left out: an app works without its
/// hatches, and a platform without one shows the node (Exact LLP 1075.003.000.001 D5).
fn unsupported_features(plan: &Plan) -> Result<(), String> {
    let features = [
        (plan.surfaces.len(), "draws canvas surfaces"),
        (
            plan.profiles.len(),
            "declares colour profiles (`@color-profile`)",
        ),
        (
            plan.axes.len(),
            "has app axes (`app.tamber` `axes`, `setMode`)",
        ),
        (plan.icons.len(), "names icon tokens (`symbol:`)"),
        (plan.texts.len(), "has localized texts (`t`)"),
    ];
    for (count, what) in features {
        if count > 0 {
            return Err(format!("the app {what} ({count}); not in the C++ target"));
        }
    }
    Ok(())
}

/// Each region's subject function and (for `each`) its key function.
fn region_bodies(plan: &Plan, out: &mut Out) -> Result<Vec<(String, String)>, String> {
    plan.regions
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let subject = out.body(plan, r.subject, &format!("region_{i}"))?;
            let key = if r.kind == RegionKind::Each {
                out.body(plan, r.key, &format!("region_key_{i}"))?
            } else {
                "nullptr".into()
            };
            Ok((subject, key))
        })
        .collect()
}

/// Each handler's bound argument, as a function.
fn arg_bodies(plan: &Plan, out: &mut Out) -> Result<Vec<String>, String> {
    plan.args
        .iter()
        .enumerate()
        .map(|(i, a)| out.body(plan, a.expr, &format!("arg_{i}")))
        .collect()
}

/// Each sequence's row: its name, and its gate, tracks and tempo as functions
/// (`sound::check_sequences` admitted the rest).
fn sequence_rows(plan: &Plan, out: &mut Out) -> Result<Vec<String>, String> {
    plan.sequences
        .iter()
        .enumerate()
        .map(|(i, q)| {
            let gate = out.body(plan, q.gate, &format!("sequence_gate_{i}"))?;
            let tracks = out.body(plan, q.tracks, &format!("sequence_tracks_{i}"))?;
            let bpm = out.body(plan, q.bpm, &format!("sequence_bpm_{i}"))?;
            Ok(format!(
                "{{{}, {gate}, {tracks}, {bpm}}}",
                cpp_string(plan.str(q.name))
            ))
        })
        .collect()
}

/// The counts of the tables `kApp` points at beside the plan's own.
struct Counts {
    extensions: usize,
    uses: usize,
    tokens: usize,
    sounds: usize,
    sequences: usize,
    animations: usize,
}

/// The whole C++ file for `plan`, compiled from `source`; its fonts and images
/// are read from `source`'s directory.
///
/// # Errors
/// The first construct outside the C++ target, by name, or an unreadable asset.
pub fn emit(plan: &Plan, source: &Path) -> Result<String, String> {
    check_plan(plan)?;
    let extensions = ext::roster(source)?;
    let uses = ext::check(plan, &extensions)?;
    ext::props(source, &extensions)?;
    let mut out = Out {
        functions: String::new(),
    };
    let slots = plan
        .slots
        .iter()
        .enumerate()
        .map(|(i, s)| out.body(plan, s.init, &format!("slot_{i}")))
        .collect::<Result<Vec<_>, _>>()?;
    let derives = plan
        .derives
        .iter()
        .enumerate()
        .map(|(i, d)| out.body(plan, d.body, &format!("derive_{i}")))
        .collect::<Result<Vec<_>, _>>()?;
    let actions = plan
        .actions
        .iter()
        .enumerate()
        .map(|(i, a)| out.body(plan, a.body, &format!("action_{i}")))
        .collect::<Result<Vec<_>, _>>()?;
    let regions = region_bodies(plan, &mut out)?;
    let args = arg_bodies(plan, &mut out)?;
    let fallbacks = resources::fallbacks(plan, &mut out.functions)?;
    let sequences = sequence_rows(plan, &mut out)?;
    let mut bindings = Vec::new();
    for (i, b) in plan.bindings.iter().enumerate() {
        let target = match b.kind {
            BindingKind::Prop => {
                format!("rt::Target{{true, static_cast<uint8_t>({})}}", prop(b.id)?)
            }
            BindingKind::Style => format!(
                "rt::Target{{false, static_cast<uint8_t>({})}}",
                style(b.id)?
            ),
        };
        bindings.push(match literal(plan, b.expr) {
            Some(lit) => format!("{{{target}, nullptr, {lit}}}"),
            None => format!(
                "{{{target}, {}, rt::Lit::none()}}",
                out.body(plan, b.expr, &format!("binding_{i}"))?
            ),
        });
    }
    let nodes = nodes(plan)?;
    let handlers = handlers(plan)?;
    let root = source.parent().unwrap_or_else(|| Path::new("."));
    let mut s = format!("// Generated by plan2cpp from {}. Do not edit.\n#include \"exact_rt.h\"\n\nnamespace app {{\nusing rt::Ctx;\nusing rt::V;\n\n", source.display());
    s.push_str(&out.functions);
    let _ = writeln!(s, "const rt::CodeFn kSlots[] = {{{}}};", row(&slots));
    let _ = writeln!(s, "const rt::CodeFn kDerives[] = {{{}}};", row(&derives));
    let _ = writeln!(s, "const rt::CodeFn kActions[] = {{{}}};", row(&actions));
    let _ = writeln!(
        s,
        "const rt::Binding kBindings[] = {{\n  {}\n}};",
        bindings.join(",\n  ")
    );
    let _ = writeln!(
        s,
        "const rt::Node kNodes[] = {{\n  {}\n}};",
        nodes.join(",\n  ")
    );
    let _ = writeln!(
        s,
        "const rt::Handler kHandlers[] = {{{}}};",
        handlers.join(", ")
    );
    let _ = writeln!(s, "const rt::CodeFn kArgs[] = {{{}}};", row(&args));
    resources::table(plan, &fallbacks, &mut s);
    types(plan, &mut s);
    assets::fonts(plan, root, &mut s)?;
    assets::images(plan, root, &mut s)?;
    ext::tables(&extensions, &uses, &mut s);
    let tokens = theme::tables(plan, &mut s)?;
    let sounds = sound::tables(plan, root, &mut s)?;
    let animations = anim::tables(plan, &mut s)?;
    sequence_table(&sequences, &mut s);
    let counts = Counts {
        extensions: extensions.len(),
        uses: uses.len(),
        tokens,
        sounds,
        sequences: sequences.len(),
        animations,
    };
    tables(plan, &regions, &needs(plan)?, &counts, &mut s);
    s.push_str("}  // namespace app\n");
    Ok(s)
}

fn sequence_table(sequences: &[String], s: &mut String) {
    let _ = writeln!(
        s,
        "const rt::Sequence kSequences[] = {{{}}};",
        if sequences.is_empty() {
            "{nullptr, nullptr, nullptr, nullptr}".to_string()
        } else {
            sequences.join(", ")
        }
    );
}

fn idx<T: Into<usize>>(v: Option<T>) -> String {
    v.map_or_else(|| "-1".to_string(), |x| x.into().to_string())
}

fn nodes(plan: &Plan) -> Result<Vec<String>, String> {
    plan.nodes
        .iter()
        .map(|n| {
            let ty = match NodeType::from_wire(n.node_type) {
                Some(NodeType::View) => "rt::T::View",
                Some(NodeType::Text) => "rt::T::Text",
                Some(NodeType::Pressable) => "rt::T::Pressable",
                Some(NodeType::Image) => "rt::T::Image",
                other => return Err(format!("the node type {other:?} is not in the C++ target")),
            };
            Ok(format!(
                "{{{ty}, {}, {}, {}, {}, {}, {}, {}}}",
                idx(n.parent.map(|p| p.0 as usize)),
                idx(n.arm.map(|a| a.0 as usize)),
                n.order,
                n.bindings.start,
                n.bindings.len,
                n.handlers.start,
                n.handlers.len
            ))
        })
        .collect()
}

fn handlers(plan: &Plan) -> Result<Vec<String>, String> {
    let mut out = Vec::new();
    for h in &plan.handlers {
        let event = match h.event {
            EventKind::Press => "rt::E::Press",
            EventKind::Key => "rt::E::Key",
            EventKind::Navigate => "rt::E::Navigate",
            EventKind::Pan => "rt::E::Pan",
            EventKind::Panrelease => "rt::E::PanRelease",
            other => return Err(format!("the {other:?} event is not in the C++ target")),
        };
        // Bound arguments first, then as much of the event's payload as the
        // action takes: none for a press, a key or a location, or a pan's
        // (dx, dy) and a release's (vx, vy).
        let params = plan.action(h.action).params.len;
        let bound = h.args.len;
        let payload = match h.event {
            EventKind::Press => 0,
            EventKind::Pan | EventKind::Panrelease => 2,
            _ => 1,
        };
        if params < bound || params > bound + payload {
            return Err(format!(
                "action {} takes event fields the C++ target does not pass",
                plan.str(plan.action(h.action).name)
            ));
        }
        out.push(format!(
            "{{{event}, {}, {}, {}}}",
            h.action.0, h.args.start, h.args.len
        ));
    }
    if out.is_empty() {
        out.push("{rt::E::Press, 0, 0, 0}".into());
    }
    Ok(out)
}

/// What an app asks of a board, for the runtime's boot report.
#[derive(Debug, PartialEq, Eq)]
pub struct Needs {
    /// A key handler or a pressable node: the board's keys reach it.
    pub keys: bool,
    /// A press or pan handler: a touch reaches it.
    pub touch: bool,
    /// The commands the actions emit, in first-use order.
    pub commands: Vec<String>,
}

/// What `plan` asks of a board.
///
/// # Errors
/// A refusal naming a malformed instruction.
pub fn needs(plan: &Plan) -> Result<Needs, String> {
    let mut keys = false;
    for n in plan.nodes.iter() {
        keys |= NodeType::from_wire(n.node_type) == Some(NodeType::Pressable);
    }
    let mut touch = false;
    for h in &plan.handlers {
        keys |= h.event == EventKind::Key;
        touch |= matches!(
            h.event,
            EventKind::Press | EventKind::Pan | EventKind::Panrelease
        );
    }
    let mut commands = Vec::new();
    for a in &plan.actions {
        code::commands(plan, a.body, &mut commands)?;
    }
    Ok(Needs {
        keys,
        touch,
        commands,
    })
}

/// The plan's types and record fields, for the agent's typed JSON.
fn types(plan: &Plan, s: &mut String) {
    let rows: Vec<String> = plan
        .types
        .iter()
        .map(|t| {
            let kind = match t.kind {
                TypeKind::List => "List",
                TypeKind::Record => "Record",
                TypeKind::Option => "Option",
                _ => "Other",
            };
            format!(
                "{{rt::Type::K::{kind}, {}, {}, {}}}",
                idx(t.elem.map(|e| e.0 as usize)),
                t.fields.start,
                t.fields.len
            )
        })
        .collect();
    let fields: Vec<String> = plan
        .fields
        .iter()
        .map(|f| format!("{{{}, {}}}", cpp_string(plan.str(f.name)), f.ty.0))
        .collect();
    let _ = writeln!(s, "const rt::Type kTypes[] = {{{}}};", rows.join(", "));
    let _ = writeln!(
        s,
        "const rt::Field kFields[] = {{{}}};",
        if fields.is_empty() {
            "{\"\", 0}".into()
        } else {
            fields.join(", ")
        }
    );
}

/// Per-slot, per-derive and per-action facts, and the routes table.
fn slot_tables(plan: &Plan, s: &mut String) {
    let list = |items: Vec<String>, empty: &str| {
        if items.is_empty() {
            empty.to_string()
        } else {
            items.join(", ")
        }
    };
    let _ = writeln!(
        s,
        "const uint16_t kSlotTypes[] = {{{}}};",
        list(plan.slots.iter().map(|x| x.ty.0.to_string()).collect(), "0")
    );
    let _ = writeln!(
        s,
        "const uint16_t kDeriveTypes[] = {{{}}};",
        list(
            plan.derives.iter().map(|x| x.ty.0.to_string()).collect(),
            "0"
        )
    );
    let _ = writeln!(
        s,
        "const int16_t kSlotOwners[] = {{{}}};",
        list(
            plan.slots
                .iter()
                .map(|x| idx(x.owner.map(|a| a.0 as usize)))
                .collect(),
            "-1"
        )
    );
    let _ = writeln!(
        s,
        "const uint16_t kMutationSlots[] = {{{}}};",
        list(
            plan.mutations
                .iter()
                .map(|m| m.slot.0.to_string())
                .collect(),
            "0"
        )
    );
    let _ = writeln!(
        s,
        "const uint8_t kActionParams[] = {{{}}};",
        list(
            plan.actions
                .iter()
                .map(|a| a.params.len.to_string())
                .collect(),
            "0"
        )
    );
    let routes: Vec<String> = plan
        .routes
        .iter()
        .map(|r| {
            format!(
                "{{{}, {}, {}, {}, {}}}",
                cpp_string(plan.str(r.name)),
                cpp_string(plan.str(r.pattern)),
                idx(r.parent.map(|p| p.0 as usize)),
                r.tab,
                r.notfound
            )
        })
        .collect();
    let _ = writeln!(
        s,
        "const rt::RouteRow kRoutes[] = {{{}}};",
        list(routes, "{\"\", \"\", -1, false, false}")
    );
}

/// The app's tables and `kApp`, with the counts of the tables written before it.
fn tables(
    plan: &Plan,
    regions: &[(String, String)],
    needs: &Needs,
    counts: &Counts,
    s: &mut String,
) {
    let region_rows: Vec<String> = plan
        .regions
        .iter()
        .zip(regions)
        .map(|(r, (f, key))| {
            let kind = if r.kind == RegionKind::Each {
                "Each"
            } else {
                "When"
            };
            format!(
                "{{rt::R::{kind}, {}, {}, {}, {f}, {key}, {}, {}}}",
                idx(r.parent.map(|p| p.0 as usize)),
                idx(r.arm.map(|a| a.0 as usize)),
                r.order,
                r.arms.start,
                r.arms.len
            )
        })
        .collect();
    let timers: Vec<String> = plan
        .timers
        .iter()
        .map(|t| format!("{{{}, {}, {}}}", t.interval_ms, t.action.0, t.once))
        .collect();
    let _ = writeln!(
        s,
        "const rt::Region kRegions[] = {{{}}};",
        if region_rows.is_empty() {
            "{rt::R::When, -1, -1, 0, nullptr, nullptr, 0, 0}".into()
        } else {
            region_rows.join(", ")
        }
    );
    let _ = writeln!(
        s,
        "const rt::Timer kTimers[] = {{{}}};",
        if timers.is_empty() {
            "{0, 0, false}".into()
        } else {
            timers.join(", ")
        }
    );
    slot_tables(plan, s);
    let _ = writeln!(
        s,
        "const char* const kSlotNames[] = {{{}}};",
        names(plan.slots.iter().map(|x| plan.str(x.name).to_string()))
    );
    let _ = writeln!(
        s,
        "const char* const kDeriveNames[] = {{{}}};",
        names(plan.derives.iter().map(|x| plan.str(x.name).to_string()))
    );
    let _ = writeln!(
        s,
        "const char* const kActionNames[] = {{{}}};",
        names(plan.actions.iter().map(|x| plan.str(x.name).to_string()))
    );
    let _ = writeln!(
        s,
        "const char* const kCommands[] = {{{}}};",
        names(needs.commands.iter().cloned())
    );
    let _ = writeln!(
        s,
        "extern const rt::App kApp = {{kSlots, kSlotNames, {}, kDerives, kDeriveNames, {}, kResources, {}, kActions, kActionNames, {}, kBindings, kNodes, {}, kHandlers, kRegions, {}, kTimers, {}, {}, {}, kCommands, {}, kTypes, kFields, kSlotTypes, kDeriveTypes, kSlotOwners, kActionParams, kArgs, kRoutes, {}, {}, kGlyphs, kGlyphBits, kKerns, kFontFiles, kFaces, kFaceCount, kStackFamily, kStackCount, kImages, kImageCount, kMutationSlots, {}, kExtensions, {}, kExtensionSources, {}, kTokens, {}, kSounds, {}, kSequences, {}, kEaseStops, kEases, kKeyValues, kAnimations, kAnimationValues, {}}};",
        plan.slots.len(), plan.derives.len(), plan.resources.len(), plan.actions.len(), plan.nodes.len(), plan.regions.len(), plan.timers.len(),
        needs.keys, needs.touch, needs.commands.len(), plan.routes.len(), idx(plan.router.map(|r| r.0 as usize)),
        plan.mutations.len(), counts.extensions, counts.uses, counts.tokens, counts.sounds, counts.sequences, counts.animations
    );
}
