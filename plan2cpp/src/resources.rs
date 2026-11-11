//! The resource table: each resource's source, arguments, and what it shows
//! while its source has not answered (Exact LLP 1048.003 D6, LLP 1054.000.002
//! D1/D3): its declared placeholder row (`else source(args)`), else its
//! declared value (`else empty(…)`), else its type's zero. A placeholder row
//! shows nothing of its own, as the runner's `placeholder` has it.

use std::fmt::Write;

use exact_plan::{Plan, TypeKind, TypesId, Value};

use crate::code::cpp_string;

/// Writes each resource's fallback as a code function into `functions`, and
/// returns their names, `nullptr` where a resource has none.
///
/// # Errors
/// A declared placeholder value the C++ target cannot hold.
pub fn fallbacks(plan: &Plan, functions: &mut String) -> Result<Vec<String>, String> {
    let mut names = Vec::with_capacity(plan.resources.len());
    for i in 0..plan.resources.len() {
        names.push(match fallback(plan, i)? {
            Some(expr) => {
                let _ = writeln!(
                    functions,
                    "static V fallback_{i}(Ctx&) {{ return {expr}; }}\n"
                );
                format!("fallback_{i}")
            }
            None => "nullptr".into(),
        });
    }
    Ok(names)
}

/// Writes `kResources`, with the fallbacks `fallbacks` names.
pub fn table(plan: &Plan, fallbacks: &[String], s: &mut String) {
    let mut rows = Vec::new();
    for (r, fallback) in plan.resources.iter().zip(fallbacks) {
        rows.push(format!(
            "{{{}, {}, {}, {}, {}, {}, {fallback}}}",
            cpp_string(plan.str(r.name)),
            cpp_string(plan.str(r.source)),
            r.ty.0,
            r.args.start,
            r.args.len,
            r.placeholder.map_or(-1, |p| i64::from(p.0)),
        ));
    }
    let _ = writeln!(
        s,
        "const rt::Resource kResources[] = {{{}}};",
        if rows.is_empty() {
            "{\"\", \"\", 0, 0, 0, -1, nullptr}".into()
        } else {
            rows.join(", ")
        }
    );
}

/// The C++ value resource `i` shows while its source has not answered and it
/// has no placeholder row; None for a placeholder row, which shows nothing.
fn fallback(plan: &Plan, i: usize) -> Result<Option<String>, String> {
    let r = &plan.resources[i];
    let is_placeholder_row = plan
        .resources
        .iter()
        .any(|o| o.placeholder.is_some_and(|p| p.0 as usize == i));
    if r.placeholder.is_some() || is_placeholder_row {
        return Ok(None);
    }
    let value = if r.placeholder_value.len > 0 {
        Value::from_bytes(plan.bytes(r.placeholder_value)).map_err(|e| {
            format!(
                "resource {}: its placeholder value does not read: {e:?}",
                plan.str(r.name)
            )
        })?
    } else {
        let Some(zero) = zero(plan, r.ty) else {
            return Ok(None);
        };
        zero
    };
    cpp_value(&value)
        .map(Some)
        .map_err(|e| format!("resource {}: {e}", plan.str(r.name)))
}

/// The zero of type `ty`, as Exact's runner makes it (runner settlement.rs
/// `zero`): a string's first field name (an enum's first case) or "".
fn zero(plan: &Plan, ty: TypesId) -> Option<Value> {
    let row = plan.types.get(ty.0 as usize)?;
    Some(match row.kind {
        TypeKind::Number => Value::Number(0.0),
        TypeKind::String => Value::str(
            row.fields
                .iter()
                .next()
                .map_or("", |f| plan.str(plan.field(f).name)),
        ),
        TypeKind::Bool => Value::Bool(false),
        TypeKind::Unit => Value::Unit,
        TypeKind::Option => Value::NONE,
        TypeKind::List => Value::list(Vec::new()),
        TypeKind::Record => {
            let start = row.fields.start as usize;
            let fields = plan.fields.get(start..start + row.fields.len as usize)?;
            let mut zeros = Vec::with_capacity(fields.len());
            for f in fields {
                zeros.push(zero(plan, f.ty)?);
            }
            Value::record(zeros)
        }
    })
}

/// A C++ expression for `v`; the runtime holds `some(v)` as `v` itself and
/// `none` and the unit value as none.
///
/// # Errors
/// A number that is not finite.
pub fn cpp_value(v: &Value) -> Result<String, String> {
    Ok(match v {
        Value::Number(n) if n.is_finite() => format!("V::num({n:?})"),
        Value::Number(n) => return Err(format!("the number {n} is not finite")),
        Value::Bool(b) => format!("V::boolean({b})"),
        Value::HeapStr(_) | Value::InlineStr(_) => {
            format!("V::str({})", cpp_string(v.text()))
        }
        Value::Unit | Value::Option(None) => "V()".into(),
        Value::Option(Some(inner)) => cpp_value(inner)?,
        Value::List(items) => format!("V::list({{{}}})", values(items)?),
        Value::Record(items) => format!("V::record({{{}}})", values(items)?),
    })
}

fn values(items: &[Value]) -> Result<String, String> {
    let mut out = Vec::with_capacity(items.len());
    for item in items {
        out.push(cpp_value(item)?);
    }
    Ok(out.join(", "))
}
