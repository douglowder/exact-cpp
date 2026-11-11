//! The app's design tokens (`app.tamber`, Exact LLP 1121) as a table the runtime looks a
//! colour up in by name. A row names a token by its name, as a literal or as a value made
//! at run time, so the board resolves the name where it parses a colour (`rt_view.cpp`
//! `parse_color`). The board has one scheme: it takes a token that is one colour, written as
//! hex, and refuses by name a token that varies by scheme, a role override, and a token that
//! is not a colour. An app with app axes, whose tokens vary by them, is refused before this
//! (`emit.rs` `unsupported_features`).

use std::fmt::Write;

use exact_plan::Plan;

use crate::code::cpp_string;

/// The plan's `theme` kind of a colour token (LLP 1121 D3).
const COLOUR: u8 = 0;

/// A `#rgb`, `#rrggbb` or `#rrggbbaa` colour as 0xAARRGGBB, as `parse_color` reads it.
fn argb(text: &str) -> Option<u32> {
    let hex = text.strip_prefix('#')?;
    if !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let n = u32::from_str_radix(hex, 16).ok()?;
    match hex.len() {
        3 => {
            let (r, g, b) = ((n >> 8) & 15, (n >> 4) & 15, n & 15);
            Some(0xff00_0000 | (r * 17) << 16 | (g * 17) << 8 | (b * 17))
        }
        6 => Some(0xff00_0000 | n),
        8 => Some((n & 0xff) << 24 | n >> 8),
        _ => None,
    }
}

/// Each token's name and colour, in plan order; refuses what the board cannot show.
fn tokens(plan: &Plan) -> Result<Vec<(String, u32)>, String> {
    let mut out = Vec::new();
    for t in &plan.theme {
        let name = plan.str(t.name);
        if !plan.str(t.role).is_empty() {
            return Err(format!(
                "design token {name} overrides the role {}; the board has no roles",
                plan.str(t.role)
            ));
        }
        if t.kind != COLOUR {
            return Err(format!(
                "design token {name} is not a colour (kind {}); not in the C++ target",
                t.kind
            ));
        }
        let value = plan.str(t.value);
        let Some(colour) = argb(value) else {
            return Err(format!(
                "design token {name} is `{value}`: the board has one scheme, so a token is \
                 one hex colour (mark it `scheme=fixed` in app.tamber)"
            ));
        };
        out.push((name.to_string(), colour));
    }
    Ok(out)
}

/// Writes `kTokens`; returns how many it holds.
pub fn tables(plan: &Plan, s: &mut String) -> Result<usize, String> {
    let rows: Vec<String> = tokens(plan)?
        .iter()
        .map(|(name, colour)| format!("{{{}, {colour:#010x}}}", cpp_string(name)))
        .collect();
    let items = if rows.is_empty() {
        "{nullptr, 0}".to_string()
    } else {
        rows.join(", ")
    };
    let _ = writeln!(s, "const rt::Token kTokens[] = {{{items}}};");
    Ok(rows.len())
}

#[cfg(test)]
mod tests {
    use super::argb;

    #[test]
    fn hex_colours_read_as_parse_color_reads_them() {
        assert_eq!(argb("#000000"), Some(0xff00_0000));
        assert_eq!(argb("#48ef8b"), Some(0xff48_ef8b));
        assert_eq!(argb("#fff"), Some(0xffff_ffff));
        assert_eq!(argb("#11223380"), Some(0x8011_2233));
        assert_eq!(argb("#12345"), None);
        assert_eq!(argb("#gggggg"), None);
        assert_eq!(argb("light-dark(#fff, #000)"), None);
        assert_eq!(argb("white"), None);
    }
}
