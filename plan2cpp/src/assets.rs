//! The fonts and images an app declares, made into C++ tables at build time.
//!
//! Fonts: every face the app's text can resolve to (its `font-family`,
//! `font-weight` and `font-size`, followed down the node tree as the runtime
//! inherits them), rasterized with hinting at each size it can use, as 4-bit
//! coverage, ASCII and Latin-1. Kerning comes from the font file's shaping,
//! once per file in font units, for ASCII pairs.
//!
//! Images: each `image` source under the app's `assets/`, decoded from PNG.
//! When every use draws it at one fixed size, it is resampled to that size
//! (an area average), so the board stores only the pixels it shows. A
//! one-color image with alpha is kept as a mask.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write;
use std::path::{Component, Path};

use exact_kernel::{NodeType, PropId, StyleId};
use exact_plan::{BindingKind, Plan, StackMemberKind};
use swash::scale::{Render, ScaleContext, Source};
use swash::shape::ShapeContext;
use swash::zeno::Format;
use swash::FontRef;

use crate::code::{self, cpp_string, Const};

/// Code points every face carries: printable ASCII and Latin-1, and the
/// ellipsis `text-overflow` draws.
fn code_points() -> impl Iterator<Item = u32> {
    (0x20..=0x7e)
        .chain(0xa0..=0xff)
        .chain(std::iter::once(0x2026))
}

/// The CSS default when nothing sets a row.
const DEFAULT_STACK: u32 = 0;
const DEFAULT_WEIGHT: u32 = 400;
const DEFAULT_SIZE16: u32 = 16 * 16;

/// The values one inherited style row can take at each node.
struct Rows<'a> {
    plan: &'a Plan,
}

impl Rows<'_> {
    /// The node's own binding for `row`: its possible values (sizes in 1/16
    /// px), and whether it may also yield nothing (and so inherit).
    fn own(&self, node: usize, row: StyleId) -> Result<Option<(BTreeSet<u32>, bool)>, String> {
        let n = &self.plan.nodes[node];
        for b in n.bindings.start..n.bindings.start + n.bindings.len {
            let b = &self.plan.bindings[b as usize];
            if b.kind != BindingKind::Style || StyleId::from_bit(u32::from(b.id)) != Some(row) {
                continue;
            }
            let mut set = BTreeSet::new();
            for c in code::constants(self.plan, b.expr)? {
                if let Const::Num(v) = c {
                    set.insert(scaled(row, v));
                }
            }
            let constant = code::is_constant(self.plan, b.expr)?;
            return Ok(Some((set, !constant)));
        }
        Ok(None)
    }

    fn values(&self, node: usize, row: StyleId, default: u32) -> Result<BTreeSet<u32>, String> {
        let n = &self.plan.nodes[node];
        let parent = n.parent.map(|p| p.0 as usize);
        let arm = n.arm.map(|a| a.0);
        match self.own(node, row)? {
            Some((set, false)) => Ok(set),
            Some((mut set, true)) => {
                set.extend(self.inherited(parent, arm, row, default)?);
                Ok(set)
            }
            None => self.inherited(parent, arm, row, default),
        }
    }

    /// What a node under `parent` (or at the top of arm `arm`) inherits.
    fn inherited(
        &self,
        parent: Option<usize>,
        arm: Option<u32>,
        row: StyleId,
        default: u32,
    ) -> Result<BTreeSet<u32>, String> {
        if let Some(p) = parent {
            return self.values(p, row, default);
        }
        let Some(arm) = arm else {
            return Ok(BTreeSet::from([default]));
        };
        for r in &self.plan.regions {
            if (r.arms.start..r.arms.start + r.arms.len).contains(&arm) {
                return self.inherited(
                    r.parent.map(|p| p.0 as usize),
                    r.arm.map(|a| a.0),
                    row,
                    default,
                );
            }
        }
        Ok(BTreeSet::from([default]))
    }
}

/// A row's number as a set key: sizes in 1/16 px, the rest as integers.
fn scaled(row: StyleId, v: f64) -> u32 {
    let v = if row == StyleId::FontSize {
        v * 16.0
    } else {
        v
    };
    // A style number is small and non-negative; the cast cannot truncate.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "see above"
    )]
    let out = v.round().max(0.0) as u32;
    out
}

/// The family a stack resolves to: its first member, when that is a declared
/// family (a generic family leaves the text in the board's own faces).
fn stack_family(plan: &Plan, stack: u32) -> Option<usize> {
    let s = plan.stacks.get(stack as usize)?;
    let first = plan.stack_members.get(s.members.start as usize)?;
    match first.kind {
        StackMemberKind::Family => first.family.map(|f| f.0 as usize),
        _ => None,
    }
}

/// CSS font matching's weight rule over the weights a family has.
#[must_use]
pub fn pick_weight(available: &[u32], want: u32) -> Option<u32> {
    if available.contains(&want) {
        return Some(want);
    }
    let below = available.iter().copied().filter(|w| *w < want).max();
    let above = available.iter().copied().filter(|w| *w > want).min();
    if (400..=500).contains(&want) {
        let up_to_500 = available
            .iter()
            .copied()
            .filter(|w| *w > want && *w <= 500)
            .min();
        return up_to_500.or(below).or(above);
    }
    if want < 400 {
        below.or(above)
    } else {
        above.or(below)
    }
}

/// The plan face (non-italic) that `family` uses at `weight`.
fn face_of(plan: &Plan, family: usize, weight: u32) -> Option<usize> {
    let f = plan.families.get(family)?;
    let range = f.faces.start as usize..(f.faces.start + f.faces.len) as usize;
    let upright: Vec<usize> = range.filter(|i| !plan.faces[*i].italic).collect();
    let weights: Vec<u32> = upright
        .iter()
        .map(|i| u32::from(plan.faces[*i].weight))
        .collect();
    let picked = pick_weight(&weights, weight)?;
    upright
        .into_iter()
        .find(|i| u32::from(plan.faces[*i].weight) == picked)
}

/// Each plan face the app's text can use, with the sizes (1/16 px) it needs.
///
/// # Errors
/// A refusal naming malformed code.
pub fn faces_needed(plan: &Plan) -> Result<BTreeMap<usize, BTreeSet<u32>>, String> {
    let rows = Rows { plan };
    let mut out: BTreeMap<usize, BTreeSet<u32>> = BTreeMap::new();
    for (i, n) in plan.nodes.iter().enumerate() {
        if NodeType::from_wire(n.node_type) != Some(NodeType::Text) {
            continue;
        }
        let stacks = rows.values(i, StyleId::FontFamily, DEFAULT_STACK)?;
        let weights = rows.values(i, StyleId::FontWeight, DEFAULT_WEIGHT)?;
        let sizes = rows.values(i, StyleId::FontSize, DEFAULT_SIZE16)?;
        for stack in &stacks {
            let Some(family) = stack_family(plan, *stack) else {
                continue;
            };
            for weight in &weights {
                if let Some(face) = face_of(plan, family, *weight) {
                    out.entry(face).or_default().extend(sizes.iter().copied());
                }
            }
        }
    }
    Ok(out)
}

/// A path under the app's `assets/`, read relative to the app's directory.
fn asset(root: &Path, source: &str) -> Result<Vec<u8>, String> {
    let rel = Path::new(source.strip_prefix('/').unwrap_or(source));
    let under_assets =
        matches!(rel.components().next(), Some(Component::Normal(c)) if c == "assets");
    let plain = rel.components().all(|c| matches!(c, Component::Normal(_)));
    if !under_assets || !plain {
        return Err(format!(
            "the image or font source {source:?} is not a file under the app's assets/; not in the C++ target"
        ));
    }
    let path = root.join(rel);
    std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))
}

/// One rasterized face, its glyphs packed as 4-bit coverage.
struct Raster {
    face: usize,
    px16: u32,
    ascent: i32,
    descent: i32,
    leading: i32,
    glyphs: Vec<[i64; 7]>, // code, left, top, w, h, advance (1/64 px), bits offset
}

fn fixed(v: f32) -> i64 {
    // Font metrics at screen sizes are far inside i64; the cast is exact enough.
    #[expect(clippy::cast_possible_truncation, reason = "see above")]
    let out = (v * 64.0).round() as i64;
    out
}

fn rasterize(data: &[u8], face: usize, px16: u32, bits: &mut Vec<u8>) -> Result<Raster, String> {
    let font = FontRef::from_index(data, 0).ok_or("a font file is not a TTF or OTF font")?;
    // Sizes are whole sixteenths of a pixel, far below f32's exact range.
    #[expect(clippy::cast_precision_loss, reason = "see above")]
    let px = px16 as f32 / 16.0;
    let metrics = font.metrics(&[]).scale(px);
    let advances = font.glyph_metrics(&[]).scale(px);
    let mut context = ScaleContext::new();
    let mut scaler = context.builder(font).size(px).hint(true).build();
    let charmap = font.charmap();
    let mut glyphs = Vec::new();
    for c in code_points() {
        let id = charmap.map(c);
        if id == 0 {
            continue;
        }
        let image = Render::new(&[Source::Outline])
            .format(Format::Alpha)
            .render(&mut scaler, id)
            .ok_or_else(|| format!("glyph U+{c:04X} did not render"))?;
        let p = image.placement;
        let offset = bits.len();
        let mut nibbles = image.data.iter().map(|a| (u16::from(*a) * 15 + 127) / 255);
        while let Some(hi) = nibbles.next() {
            let lo = nibbles.next().unwrap_or(0);
            bits.push(u8::try_from(hi << 4 | lo).map_err(|e| e.to_string())?);
        }
        glyphs.push([
            i64::from(c),
            i64::from(p.left),
            i64::from(p.top),
            i64::from(p.width),
            i64::from(p.height),
            fixed(advances.advance_width(id)),
            i64::try_from(offset).map_err(|e| e.to_string())?,
        ]);
    }
    Ok(Raster {
        face,
        px16,
        ascent: i32::try_from(fixed(metrics.ascent)).map_err(|e| e.to_string())?,
        descent: i32::try_from(fixed(metrics.descent)).map_err(|e| e.to_string())?,
        leading: i32::try_from(fixed(metrics.leading)).map_err(|e| e.to_string())?,
        glyphs,
    })
}

/// A kerning pair: left and right code points, and the adjustment in font units.
type Kern = (u32, u32, i32);

/// A font file's kerning for ASCII pairs, in font units, from its shaping
/// (ligatures and contextual forms off), and its units per em.
fn kerning(data: &[u8]) -> Result<(Vec<Kern>, u16), String> {
    let font = FontRef::from_index(data, 0).ok_or("a font file is not a TTF or OTF font")?;
    let upem = font.metrics(&[]).units_per_em;
    let unscaled = font.glyph_metrics(&[]);
    let charmap = font.charmap();
    let mut context = ShapeContext::new();
    let mut out = Vec::new();
    let off = [("liga", 0u16), ("clig", 0), ("calt", 0), ("dlig", 0)];
    for a in 0x21u32..=0x7e {
        for b in 0x21u32..=0x7e {
            let (ga, gb) = (charmap.map(a), charmap.map(b));
            if ga == 0 || gb == 0 {
                continue;
            }
            let (Some(ca), Some(cb)) = (char::from_u32(a), char::from_u32(b)) else {
                continue;
            };
            let mut shaper = context
                .builder(font)
                .size(f32::from(upem))
                .features(off)
                .build();
            shaper.add_str(&format!("{ca}{cb}"));
            let mut total = 0.0f32;
            shaper.shape_with(|cluster| {
                for g in cluster.glyphs {
                    total += g.advance;
                }
            });
            let kern = total - unscaled.advance_width(ga) - unscaled.advance_width(gb);
            // Font units fit i32; a kern is a few hundred units at most.
            #[expect(clippy::cast_possible_truncation, reason = "see above")]
            let units = kern.round() as i32;
            if units != 0 {
                out.push((a, b, units));
            }
        }
    }
    Ok((out, upem))
}

/// The C++ font tables: files (kerning), faces, glyphs, coverage, and each
/// stack's family.
///
/// # Errors
/// An unreadable font file or a glyph that does not render.
pub fn fonts(plan: &Plan, root: &Path, s: &mut String) -> Result<(), String> {
    let needed = faces_needed(plan)?;
    let mut files: Vec<String> = Vec::new();
    let mut kerns = Vec::new();
    let mut file_rows = Vec::new();
    let mut rasters = Vec::new();
    let mut bits = Vec::new();
    for (face, sizes) in &needed {
        let source = plan.str(plan.faces[*face].source).to_string();
        let data = asset(root, &source)?;
        if !files.contains(&source) {
            let (pairs, upem) = kerning(&data)?;
            file_rows.push(format!("{{{}, {}, {upem}}}", kerns.len(), pairs.len()));
            kerns.extend(pairs);
            files.push(source.clone());
        }
        for px16 in sizes {
            rasters.push(rasterize(&data, *face, *px16, &mut bits)?);
        }
    }
    let family_of_face = |face: usize| {
        plan.families.iter().position(|f| {
            (f.faces.start as usize..(f.faces.start + f.faces.len) as usize).contains(&face)
        })
    };
    let mut glyph_rows = Vec::new();
    let mut face_rows = Vec::new();
    for r in &rasters {
        let source = plan.str(plan.faces[r.face].source);
        let file = files.iter().position(|f| f == source).unwrap_or(0);
        face_rows.push(format!(
            "{{{}, {}, {:?}f, {}, {}, {}, {}, {}, {file}}}",
            family_of_face(r.face).map_or(-1, |f| i64::try_from(f).unwrap_or(-1)),
            plan.faces[r.face].weight,
            f64::from(r.px16) / 16.0,
            r.ascent,
            r.descent,
            r.leading,
            glyph_rows.len(),
            r.glyphs.len()
        ));
        for g in &r.glyphs {
            glyph_rows.push(format!(
                "{{{}, {}, {}, {}, {}, {}, {}}}",
                g[0], g[1], g[2], g[3], g[4], g[5], g[6]
            ));
        }
    }
    let stacks: Vec<String> = (0..plan.stacks.len())
        .map(|i| {
            u32::try_from(i)
                .ok()
                .and_then(|i| stack_family(plan, i))
                .map_or("-1".to_string(), |f| f.to_string())
        })
        .collect();
    let kern_rows: Vec<String> = kerns
        .iter()
        .map(|(a, b, u)| format!("{{{a}, {b}, {u}}}"))
        .collect();
    table(
        s,
        "rt::Glyph",
        "kGlyphs",
        &glyph_rows,
        "{0, 0, 0, 0, 0, 0, 0}",
    );
    bytes(s, "kGlyphBits", &bits);
    table(s, "rt::Kern", "kKerns", &kern_rows, "{0, 0, 0}");
    table(s, "rt::FontFile", "kFontFiles", &file_rows, "{0, 0, 0}");
    table(
        s,
        "rt::Face",
        "kFaces",
        &face_rows,
        "{-1, 0, 0.0f, 0, 0, 0, 0, 0, 0}",
    );
    table(s, "int16_t", "kStackFamily", &stacks, "-1");
    let _ = writeln!(s, "constexpr uint16_t kFaceCount = {};", face_rows.len());
    let _ = writeln!(s, "constexpr uint16_t kStackCount = {};", stacks.len());
    Ok(())
}

fn table(s: &mut String, ty: &str, name: &str, rows: &[String], empty: &str) {
    let body = if rows.is_empty() {
        empty.to_string()
    } else {
        rows.join(",\n  ")
    };
    let _ = writeln!(s, "const {ty} {name}[] = {{\n  {body}\n}};");
}

fn bytes(s: &mut String, name: &str, data: &[u8]) {
    let _ = write!(s, "const uint8_t {name}[] = {{");
    if data.is_empty() {
        s.push('0');
    }
    for (i, b) in data.iter().enumerate() {
        if i % 32 == 0 {
            s.push_str("\n  ");
        }
        let _ = write!(s, "{b},");
    }
    s.push_str("\n};\n");
}

/// A decoded image, RGBA8.
struct Rgba {
    w: usize,
    h: usize,
    px: Vec<[u8; 4]>,
}

fn decode_png(data: &[u8], source: &str) -> Result<Rgba, String> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(data));
    decoder.set_transformations(png::Transformations::normalize_to_color8());
    let mut reader = decoder
        .read_info()
        .map_err(|e| format!("{source}: not a PNG image ({e})"))?;
    let size = reader
        .output_buffer_size()
        .ok_or_else(|| format!("{source}: too large"))?;
    let mut buf = vec![0; size];
    let info = reader
        .next_frame(&mut buf)
        .map_err(|e| format!("{source}: {e}"))?;
    let (w, h) = (info.width as usize, info.height as usize);
    let channels = info.color_type.samples();
    let mut px = Vec::with_capacity(w * h);
    for y in 0..h {
        let row = &buf[y * info.line_size..];
        for x in 0..w {
            let p = &row[x * channels..x * channels + channels];
            px.push(match channels {
                1 => [p[0], p[0], p[0], 255],
                2 => [p[0], p[0], p[0], p[1]],
                3 => [p[0], p[1], p[2], 255],
                _ => [p[0], p[1], p[2], p[3]],
            });
        }
    }
    Ok(Rgba { w, h, px })
}

/// Each output index's source indices along one axis, with their overlap.
fn weights(from: usize, to: usize) -> Vec<Vec<(usize, f64)>> {
    // Image sides are far below 2^52; the casts are exact.
    #[expect(clippy::cast_precision_loss, reason = "see above")]
    let scale = from as f64 / to as f64;
    (0..to)
        .map(|o| {
            #[expect(clippy::cast_precision_loss, reason = "see above")]
            let (start, end) = (o as f64 * scale, (o + 1) as f64 * scale);
            #[expect(
                clippy::cast_possible_truncation,
                clippy::cast_sign_loss,
                reason = "see above"
            )]
            let (first, last) = (start.floor() as usize, (end.ceil() as usize).min(from));
            (first..last)
                .map(|i| {
                    #[expect(clippy::cast_precision_loss, reason = "see above")]
                    let lo = start.max(i as f64);
                    #[expect(clippy::cast_precision_loss, reason = "see above")]
                    let hi = end.min((i + 1) as f64);
                    (i, (hi - lo).max(0.0))
                })
                .collect()
        })
        .collect()
}

/// An area-average resample, with alpha premultiplied.
fn resample(img: &Rgba, w: usize, h: usize) -> Rgba {
    let (xs, ys) = (weights(img.w, w), weights(img.h, h));
    let mut px = Vec::with_capacity(w * h);
    for row in &ys {
        for col in &xs {
            let mut sum = [0.0f64; 4];
            let mut total = 0.0;
            for (sy, wy) in row {
                for (sx, wx) in col {
                    let p = img.px[sy * img.w + sx];
                    let wgt = wy * wx;
                    let a = f64::from(p[3]) / 255.0;
                    for c in 0..3 {
                        sum[c] += f64::from(p[c]) * a * wgt;
                    }
                    sum[3] += a * wgt;
                    total += wgt;
                }
            }
            let alpha = if total > 0.0 { sum[3] / total } else { 0.0 };
            let channel = |v: f64| {
                // Clamped to 0..=255 before the cast.
                #[expect(
                    clippy::cast_possible_truncation,
                    clippy::cast_sign_loss,
                    reason = "see above"
                )]
                let out = v.round().clamp(0.0, 255.0) as u8;
                out
            };
            let un = |c: usize| if sum[3] > 0.0 { sum[c] / sum[3] } else { 0.0 };
            px.push([
                channel(un(0)),
                channel(un(1)),
                channel(un(2)),
                channel(alpha * 255.0),
            ]);
        }
    }
    Rgba { w, h, px }
}

fn rgb565(p: [u8; 4]) -> u16 {
    (u16::from(p[0]) & 0xf8) << 8 | (u16::from(p[1]) & 0xfc) << 3 | u16::from(p[2]) >> 3
}

/// Each image source the app uses, with the fixed sizes it is drawn at (none
/// when a use leaves its size to the image).
fn image_uses(plan: &Plan) -> Result<BTreeMap<String, BTreeSet<(usize, usize)>>, String> {
    let mut out: BTreeMap<String, BTreeSet<(usize, usize)>> = BTreeMap::new();
    let mut free = BTreeSet::new();
    for n in &plan.nodes {
        if NodeType::from_wire(n.node_type) != Some(NodeType::Image) {
            continue;
        }
        let (mut sources, mut w, mut h) = (Vec::new(), None, None);
        for b in n.bindings.start..n.bindings.start + n.bindings.len {
            let b = &plan.bindings[b as usize];
            let consts = code::constants(plan, b.expr)?;
            let constant = code::is_constant(plan, b.expr)?;
            let number = match consts.first() {
                Some(Const::Num(v)) if constant && *v >= 1.0 => Some(*v),
                _ => None,
            };
            match (b.kind, b.id) {
                // A literal source must be an asset; a computed one may also
                // name an image the board makes at run time, so only its
                // strings under assets/ are assets.
                (BindingKind::Prop, id) if PropId::from_wire(id) == Some(PropId::ImageSource) => {
                    for c in consts {
                        if let Const::Str(s) = c {
                            if constant || s.trim_start_matches('/').starts_with("assets/") {
                                sources.push(s);
                            }
                        }
                    }
                }
                (BindingKind::Style, id)
                    if StyleId::from_bit(u32::from(id)) == Some(StyleId::Width) =>
                {
                    w = number;
                }
                (BindingKind::Style, id)
                    if StyleId::from_bit(u32::from(id)) == Some(StyleId::Height) =>
                {
                    h = number;
                }
                _ => {}
            }
        }
        for source in sources {
            let entry = out.entry(source.clone()).or_default();
            match (w, h) {
                // Sides of a few hundred pixels; the casts are exact.
                #[expect(
                    clippy::cast_possible_truncation,
                    clippy::cast_sign_loss,
                    reason = "see above"
                )]
                (Some(w), Some(h)) => {
                    entry.insert((w.round() as usize, h.round() as usize));
                }
                _ => {
                    free.insert(source);
                }
            }
        }
    }
    for source in free {
        out.insert(source, BTreeSet::new());
    }
    Ok(out)
}

/// The C++ image tables: each source's pixels (RGB565, or a mask's alpha and
/// one color) and its alpha when it has any.
///
/// # Errors
/// A source outside `assets/`, an unreadable file, or a file that is not PNG.
pub fn images(plan: &Plan, root: &Path, s: &mut String) -> Result<(), String> {
    let mut rows = Vec::new();
    for (k, (source, sizes)) in image_uses(plan)?.iter().enumerate() {
        let mut img = decode_png(&asset(root, source)?, source)?;
        if let [(w, h)] = sizes.iter().copied().collect::<Vec<_>>().as_slice() {
            if (*w, *h) != (img.w, img.h) {
                img = resample(&img, *w, *h);
            }
        }
        let opaque = img.px.iter().all(|p| p[3] == 255);
        let colors: BTreeSet<[u8; 3]> = img
            .px
            .iter()
            .filter(|p| p[3] > 0)
            .map(|p| [p[0], p[1], p[2]])
            .collect();
        let alpha: Vec<u8> = img.px.iter().map(|p| p[3]).collect();
        let (format, color, rgb, a) = if !opaque && colors.len() <= 1 {
            let c = colors.first().copied().unwrap_or([0, 0, 0]);
            let color = u32::from(c[0]) << 16 | u32::from(c[1]) << 8 | u32::from(c[2]);
            (2, color, None, Some(alpha))
        } else {
            let rgb: Vec<u8> = img
                .px
                .iter()
                .flat_map(|p| rgb565(*p).to_le_bytes())
                .collect();
            (u8::from(!opaque), 0, Some(rgb), (!opaque).then_some(alpha))
        };
        if let Some(rgb) = &rgb {
            bytes(s, &format!("kImageRgb{k}"), rgb);
        }
        if let Some(a) = &a {
            bytes(s, &format!("kImageAlpha{k}"), a);
        }
        rows.push(format!(
            "{{{}, {}, {}, {format}, {color:#08x}, {}, {}}}",
            cpp_string(source),
            img.w,
            img.h,
            rgb.map_or("nullptr".into(), |_| format!("kImageRgb{k}")),
            a.map_or("nullptr".into(), |_| format!("kImageAlpha{k}"))
        ));
    }
    let _ = writeln!(s, "constexpr uint16_t kImageCount = {};", rows.len());
    table(
        s,
        "rt::Image",
        "kImages",
        &rows,
        "{\"\", 0, 0, 0, 0, nullptr, nullptr}",
    );
    Ok(())
}
