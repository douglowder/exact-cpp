//! CSS animations (Exact LLP 1055 D5): the `animation` values the board can
//! play, parsed here so the board needs no CSS parser.
//!
//! The board looks a node's `animation` value up by its text among the plan's
//! string literals that read as one: each name resolves to a `@keyframes` rule
//! (or the text is `none`). A value made at run time from other text matches
//! none of them and plays nothing. Each animation carries its timing and its
//! keyframes' values, in the order Exact's motion crate keeps them.

use std::fmt::Write;

use exact_motion::easing::{Easing, StepPosition};
use exact_motion::{Animations, Keyframes, Property, Value};
use exact_plan::Plan;

use crate::code::cpp_string;

/// The properties the board animates, as `rt::AP` names.
fn board_property(p: Property) -> Option<&'static str> {
    Some(match p {
        Property::Translate => "Translate",
        Property::Opacity => "Opacity",
        Property::Color => "Color",
        Property::BackgroundColor => "BackgroundColor",
        Property::BorderTopColor => "BorderTopColor",
        Property::BorderRightColor => "BorderRightColor",
        Property::BorderBottomColor => "BorderBottomColor",
        Property::BorderLeftColor => "BorderLeftColor",
        _ => return None,
    })
}

/// The app's `@keyframes` rules by name.
///
/// # Errors
/// A rule that does not parse.
fn rules(plan: &Plan) -> Result<Vec<(String, Keyframes)>, String> {
    let mut out = Vec::new();
    for k in &plan.keyframes {
        let name = plan.str(k.name).to_string();
        let frames = Keyframes::parse(plan.str(k.css))
            .map_err(|e| format!("@keyframes {name} does not parse: {e:?}"))?;
        out.push((name, frames));
    }
    Ok(out)
}

/// The plan's string literals that read as an `animation` value, with each
/// name resolved, in plan order.
///
/// # Errors
/// A rule that does not parse.
pub fn values(plan: &Plan) -> Result<Vec<(String, Animations)>, String> {
    let rules = rules(plan)?;
    let mut out: Vec<(String, Animations)> = Vec::new();
    for text in plan.strings.iter() {
        let text: &str = text;
        if out.iter().any(|(t, _)| t == text) {
            continue;
        }
        if text.trim() == "none" {
            out.push((text.to_string(), Animations::NONE));
            continue;
        }
        let Ok(mut parsed) = Animations::parse(text) else {
            continue;
        };
        let count = parsed.0.len();
        let dropped = parsed.resolve(|name| rules.iter().find(|(n, _)| n == name).map(|(_, k)| k));
        if count == 0 || !dropped.is_empty() || parsed.validate().is_err() {
            continue;
        }
        out.push((text.to_string(), parsed));
    }
    Ok(out)
}

/// Writes the animation tables.
///
/// # Errors
/// An animation that sets a property the board does not animate, or a color
/// it cannot hold.
pub fn tables(plan: &Plan, s: &mut String) -> Result<usize, String> {
    let mut t = Tables::default();
    let mut rows = Vec::new();
    for (text, animations) in values(plan)? {
        let start = t.animations.len();
        for a in &animations.0 {
            t.animation(a)
                .map_err(|e| format!("animation `{text}`: {e}"))?;
        }
        rows.push(format!(
            "{{{}, {start}, {}}}",
            cpp_string(&text),
            t.animations.len() - start
        ));
    }
    let _ = writeln!(s, "const double kEaseStops[] = {{{}}};", or_zero(&t.stops));
    let _ = writeln!(
        s,
        "const rt::Ease kEases[] = {{{}}};",
        or_row(&t.eases, "{0, 0, 0, 0, 0, 0, 0}")
    );
    let _ = writeln!(
        s,
        "const rt::KeyValue kKeyValues[] = {{{}}};",
        or_row(&t.values, "{0, -1, rt::AP::Opacity, {0, 0, 0, 0}}")
    );
    let _ = writeln!(
        s,
        "const rt::Animation kAnimations[] = {{{}}};",
        or_row(&t.animations, "{\"\", 0, 0, 0, 0, 0, 0, false, 0, 0}")
    );
    let _ = writeln!(
        s,
        "const rt::AnimationValue kAnimationValues[] = {{{}}};",
        or_row(&rows, "{\"\", 0, 0}")
    );
    Ok(rows.len())
}

fn or_row(rows: &[String], empty: &str) -> String {
    if rows.is_empty() {
        empty.to_string()
    } else {
        rows.join(", ")
    }
}

fn or_zero(numbers: &[String]) -> String {
    if numbers.is_empty() {
        "0".into()
    } else {
        numbers.join(", ")
    }
}

#[derive(Default)]
struct Tables {
    stops: Vec<String>,
    eases: Vec<String>,
    values: Vec<String>,
    animations: Vec<String>,
}

impl Tables {
    fn animation(&mut self, a: &exact_motion::Animation) -> Result<(), String> {
        let ease = self.ease(&a.easing);
        let start = self.values.len();
        for frame in a.keyframes.0.iter() {
            let frame_ease = match &frame.easing {
                Some(e) => i64::try_from(self.ease(e)).map_err(|e| e.to_string())?,
                None => -1,
            };
            for (p, v) in &frame.values {
                let Some(name) = board_property(*p) else {
                    return Err(format!(
                        "it animates `{}`, which the board does not animate; not in the C++ target",
                        p.css_name()
                    ));
                };
                self.values.push(format!(
                    "{{{}, {frame_ease}, rt::AP::{name}, {{{}}}}}",
                    number(frame.offset),
                    components(*p, *v)?
                ));
            }
        }
        self.animations.push(format!(
            "{{{}, {}, {}, {}, {}, {}, {}, {}, {start}, {}}}",
            cpp_string(&a.name),
            number(a.duration * 1000.0),
            number(a.delay * 1000.0),
            if a.iterations.is_infinite() {
                "INFINITY".to_string()
            } else {
                number(a.iterations)
            },
            a.direction as u8,
            a.fill as u8,
            ease,
            a.paused,
            self.values.len() - start
        ));
        Ok(())
    }

    fn ease(&mut self, e: &Easing) -> usize {
        let bezier = |x1: f64, y1: f64, x2: f64, y2: f64| {
            format!(
                "{{1, {}, {}, {}, {}, 0, 0}}",
                number(x1),
                number(y1),
                number(x2),
                number(y2)
            )
        };
        let row = match e {
            Easing::Linear => "{0, 0, 0, 0, 0, 0, 0}".to_string(),
            Easing::Ease => bezier(0.25, 0.1, 0.25, 1.0),
            Easing::EaseIn => bezier(0.42, 0.0, 1.0, 1.0),
            Easing::EaseOut => bezier(0.0, 0.0, 0.58, 1.0),
            Easing::EaseInOut => bezier(0.42, 0.0, 0.58, 1.0),
            Easing::CubicBezier { x1, y1, x2, y2 } => bezier(*x1, *y1, *x2, *y2),
            Easing::Steps { count, position } => {
                let at = match position {
                    StepPosition::JumpStart => 0,
                    StepPosition::JumpEnd => 1,
                    StepPosition::JumpNone => 2,
                    StepPosition::JumpBoth => 3,
                };
                format!("{{2, {count}, {at}, 0, 0, 0, 0}}")
            }
            Easing::PiecewiseLinear(stops) => {
                let start = self.stops.len() / 2;
                for stop in stops {
                    self.stops.push(number(stop.input));
                    self.stops.push(number(stop.output));
                }
                format!("{{3, 0, 0, 0, 0, {start}, {}}}", stops.len())
            }
        };
        if let Some(i) = self.eases.iter().position(|r| *r == row) {
            return i;
        }
        self.eases.push(row);
        self.eases.len() - 1
    }
}

/// A keyframe value's four components; a color premultiplied sRGB.
fn components(p: Property, v: Value) -> Result<String, String> {
    if v.oklab {
        return Err(format!(
            "its `{}` is a color the board does not interpolate (OKLab); not in the C++ target",
            p.css_name()
        ));
    }
    Ok(v.components().map(number).join(", "))
}

/// A finite number as a C++ double literal.
fn number(n: f64) -> String {
    format!("{n:?}")
}
