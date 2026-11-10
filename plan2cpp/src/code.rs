//! Plan bytecode to C++. Each code body becomes one function over a value
//! stack and a locals stack, instruction by instruction, with `goto` for the
//! plan's forward jumps: a direct, checkable translation of the runner's VM.
//! Opcodes outside the subset are refused with their name.

use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write;

use exact_plan::{Code, Opcode, Plan, Stdlib};
use exact_runner::vm::instructions;

/// A decoded instruction (the runner's `Instruction`, copied out).
struct Ins {
    pc: usize,
    op: Opcode,
    args: [u64; 3],
    number: f64,
}

fn decode(plan: &Plan, body: Code) -> Result<Vec<Ins>, String> {
    instructions(plan.code(body))
        .map(|r| {
            r.map(|i| Ins {
                pc: i.pc,
                op: i.op,
                args: i.args,
                number: i.number,
            })
            .map_err(|e| format!("malformed code: {e:?}"))
        })
        .collect()
}

fn text(plan: &Plan, id: u64) -> Result<&str, String> {
    let id = u32::try_from(id).map_err(|e| format!("string id {id}: {e}"))?;
    Ok(plan.str(exact_plan::StrId(id)))
}

/// A C++ string literal for `s`, with every non-ASCII or special byte escaped in octal.
#[must_use]
pub fn cpp_string(s: &str) -> String {
    let mut out = String::from("\"");
    for b in s.bytes() {
        match b {
            b'"' => out.push_str("\\\""),
            b'\\' => out.push_str("\\\\"),
            0x20..=0x7e => out.push(char::from(b)),
            _ => {
                let _ = write!(out, "\\{b:03o}");
            }
        }
    }
    out.push('"');
    out
}

fn number(n: f64) -> Result<String, String> {
    if !n.is_finite() {
        return Err(format!("the number {n} has no C++ literal here"));
    }
    let text = format!("{n:?}");
    Ok(if text.contains(['.', 'e']) {
        text
    } else {
        format!("{text}.0")
    })
}

/// The router standard functions (LLP 1038 §3): `open` … `encodeRouteSegment`,
/// in roster order, then `fragment` (LLP 1153 D5). The runtime runs them on
/// the plan's positional Router.
pub const ROUTE_CALLS: [&str; 14] = [
    "open",
    "push",
    "replace",
    "back",
    "select",
    "go",
    "stack",
    "top",
    "depth",
    "params",
    "searchParam",
    "encodeURIComponent",
    "encodeRouteSegment",
    "fragment",
];

/// How the C++ target runs a stdlib call: a pure runtime function, a route
/// function the runtime answers through `Ctx::call` (its ordinal), or
/// `performanceNow`, the milliseconds since the app started (`Ctx::now`).
enum Call {
    Pure(&'static str),
    Route(u8),
    Now,
}

/// Arity of a stdlib call the backend admits, and how it runs.
fn stdlib(id: u64) -> Result<(usize, Call), String> {
    let ordinal = u8::try_from(id).map_err(|e| format!("stdlib id {id}: {e}"))?;
    let call = Stdlib::from_wire(ordinal);
    let f = match call {
        Some(Stdlib::ToFixed) => Call::Pure("rt::to_fixed"),
        Some(Stdlib::Max) => Call::Pure("rt::max"),
        Some(Stdlib::Min) => Call::Pure("rt::min"),
        Some(Stdlib::Floor) => Call::Pure("rt::floor"),
        Some(Stdlib::Ceil) => Call::Pure("rt::ceil"),
        Some(Stdlib::Round) => Call::Pure("rt::round"),
        Some(Stdlib::ToString) => Call::Pure("rt::to_string"),
        Some(Stdlib::PerformanceNow) => Call::Now,
        Some(c) if ROUTE_CALLS.contains(&c.name()) => Call::Route(ordinal),
        other => {
            return Err(format!(
                "the standard function {other:?} is not in the C++ target"
            ))
        }
    };
    let arity = call.map_or(0, Stdlib::arity);
    let admitted = if matches!(f, Call::Now) { 0..=0 } else { 1..=2 };
    if !admitted.contains(&arity) {
        return Err(format!(
            "the standard function {call:?} with {arity} arguments is not in the C++ target"
        ));
    }
    Ok((arity, f))
}

/// The field count of the record type `id`, which a `Record` instruction pops.
fn record_fields(plan: &Plan, id: u64) -> Result<usize, String> {
    let t = usize::try_from(id).map_err(|e| format!("type id {id}: {e}"))?;
    let ty = plan.types.get(t).ok_or_else(|| format!("no type {t}"))?;
    Ok(ty.fields.len as usize)
}

/// Stack effect of one instruction: (values popped, values pushed, locals delta).
fn effect(plan: &Plan, i: &Ins) -> Result<(usize, usize, isize), String> {
    use Opcode as O;
    Ok(match i.op {
        O::Record => (record_fields(plan, i.args[0])?, 1, 0),
        O::List => (usize::try_from(i.args[0]).map_err(|e| e.to_string())?, 1, 0),
        O::Number
        | O::Bool
        | O::Str
        | O::None
        | O::Unit
        | O::LoadSlot
        | O::LoadDerive
        | O::LoadResource
        | O::LoadParam
        | O::LoadItem
        | O::LoadLocal => (0, 1, 0),
        O::Field | O::Not | O::Neg | O::Some => (1, 1, 0),
        O::Add
        | O::Sub
        | O::Mul
        | O::Div
        | O::Rem
        | O::Eq
        | O::Ne
        | O::Lt
        | O::Le
        | O::Gt
        | O::Ge
        | O::Concat => (2, 1, 0),
        O::JumpIfFalse | O::StoreSlot | O::Pop => (1, 0, 0),
        O::Jump | O::Return => (0, 0, 0),
        O::BindLocal => (1, 0, 1),
        O::DropLocal => (0, 0, -1),
        O::Call => (stdlib(i.args[0])?.0, 1, 0),
        O::Command => (usize::try_from(i.args[1]).map_err(|e| e.to_string())?, 0, 0),
        O::Send => (usize::try_from(i.args[2]).map_err(|e| e.to_string())?, 0, 0),
        O::NativeProps => (
            2 * usize::try_from(i.args[0]).map_err(|e| e.to_string())?,
            1,
            0,
        ),
        other => return Err(format!("the opcode {other:?} is not in the C++ target")),
    })
}

/// Maximum value-stack and locals depth, checking every path agrees at each label.
fn depths(plan: &Plan, code: &[Ins]) -> Result<(usize, usize), String> {
    let mut at: BTreeMap<usize, (usize, usize)> = BTreeMap::new();
    let (mut stack, mut locals, mut max_stack, mut max_locals) = (0usize, 0usize, 0usize, 0usize);
    let mut live = true;
    for i in code {
        if let Some(&(s, l)) = at.get(&i.pc) {
            if live && (s, l) != (stack, locals) {
                return Err(format!("stack depths disagree at {}", i.pc));
            }
            (stack, locals, live) = (s, l, true);
        }
        if !live {
            return Err(format!("unreachable code at {}", i.pc));
        }
        let (pop, push, delta) = effect(plan, i)?;
        stack = stack
            .checked_sub(pop)
            .ok_or_else(|| format!("stack underflow at {}", i.pc))?
            + push;
        locals = locals
            .checked_add_signed(delta)
            .ok_or_else(|| format!("locals underflow at {}", i.pc))?;
        (max_stack, max_locals) = (max_stack.max(stack), max_locals.max(locals));
        match i.op {
            Opcode::Jump | Opcode::JumpIfFalse => {
                let target = usize::try_from(i.args[0]).map_err(|e| e.to_string())?;
                at.insert(target, (stack, locals));
                live = i.op == Opcode::JumpIfFalse;
            }
            Opcode::Return => live = false,
            _ => {}
        }
    }
    Ok((max_stack, max_locals))
}

fn statement(plan: &Plan, i: &Ins) -> Result<String, String> {
    use Opcode as O;
    let a = i.args[0];
    Ok(match i.op {
        O::Number => format!("st[sp++] = V::num({});", number(i.number)?),
        O::Bool => format!("st[sp++] = V::boolean({});", a != 0),
        O::Str => format!("st[sp++] = V::str({});", cpp_string(text(plan, a)?)),
        O::None | O::Unit => "st[sp++] = V();".into(),
        O::LoadSlot => format!("st[sp++] = c.slot({a});"),
        O::LoadDerive => format!("st[sp++] = c.derive({a});"),
        O::LoadResource => format!("st[sp++] = c.resource({a});"),
        O::LoadParam => format!("st[sp++] = c.param({a});"),
        O::LoadLocal => format!("st[sp++] = lc[{a}];"),
        O::LoadItem => format!("st[sp++] = c.item({a});"),
        O::Field => format!("st[sp - 1] = st[sp - 1].field({a});"),
        O::Record | O::List => {
            let (n, make) = if i.op == O::Record {
                (record_fields(plan, a)?, "record")
            } else {
                (usize::try_from(a).map_err(|e| e.to_string())?, "list")
            };
            format!("sp -= {n}; st[sp] = V::{make}(std::vector<V>(st + sp, st + sp + {n})); ++sp;")
        }
        O::Not => "st[sp - 1] = V::boolean(!st[sp - 1].b);".into(),
        O::Neg => "st[sp - 1] = V::num(-st[sp - 1].n);".into(),
        O::Some => "// some(v): the runtime holds it as v itself.".into(),
        O::Add
        | O::Sub
        | O::Mul
        | O::Div
        | O::Rem
        | O::Eq
        | O::Ne
        | O::Lt
        | O::Le
        | O::Gt
        | O::Ge
        | O::Concat => {
            let f = format!("{:?}", i.op).to_lowercase();
            format!("{{ V rhs = st[--sp]; st[sp - 1] = rt::{f}(st[sp - 1], rhs); }}")
        }
        O::Jump => format!("goto L{a};"),
        O::JumpIfFalse => format!("if (!st[--sp].b) goto L{a};"),
        O::StoreSlot => format!("c.write({a}, st[--sp]);"),
        O::Pop => "--sp;".into(),
        O::BindLocal => "lc[lp++] = st[--sp];".into(),
        O::DropLocal => "--lp;".into(),
        O::Call => call(a)?,
        O::Command => command(plan, i)?,
        O::Send => send(plan, i)?,
        O::NativeProps => format!(
            "sp -= {}; st[sp] = rt::native_props(&st[sp], {a}); ++sp;",
            2 * a
        ),
        O::Return => "return sp > 0 ? st[sp - 1] : V();".into(),
        other => return Err(format!("the opcode {other:?} is not in the C++ target")),
    })
}

fn call(id: u64) -> Result<String, String> {
    let (arity, f) = stdlib(id)?;
    Ok(match (f, arity) {
        (Call::Pure(f), 1) => format!("st[sp - 1] = {f}(st[sp - 1]);"),
        (Call::Pure(f), _) => format!("{{ V rhs = st[--sp]; st[sp - 1] = {f}(st[sp - 1], rhs); }}"),
        (Call::Route(n), 1) => format!("st[sp - 1] = c.call({n}, &st[sp - 1], 1);"),
        (Call::Route(n), _) => format!("--sp; st[sp - 1] = c.call({n}, &st[sp - 1], 2);"),
        (Call::Now, _) => "st[sp++] = c.now();".into(),
    })
}

/// A send to mutation `args[0]`: the data module's `args[1]` with `args[2]`
/// arguments, answered when the action's commit ends.
fn send(plan: &Plan, i: &Ins) -> Result<String, String> {
    let name = text(plan, i.args[1])?;
    let argc = i.args[2];
    Ok(format!(
        "sp -= {argc}; c.send({}, {}, st + sp, {argc});",
        i.args[0],
        cpp_string(name)
    ))
}

fn command(plan: &Plan, i: &Ins) -> Result<String, String> {
    let name = text(plan, i.args[0])?;
    let argc = i.args[1];
    Ok(format!(
        "sp -= {argc}; c.command({}, st + sp, {argc});",
        cpp_string(name)
    ))
}

/// The names of the commands `body` emits, without `preventDefault` (the
/// runtime handles it), added to `out` once each.
///
/// # Errors
/// A refusal naming a malformed instruction.
pub fn commands(plan: &Plan, body: Code, out: &mut Vec<String>) -> Result<(), String> {
    for i in decode(plan, body)? {
        if i.op != Opcode::Command {
            continue;
        }
        let name = text(plan, i.args[0])?;
        if name != "preventDefault" && !out.iter().any(|n| n == name) {
            out.push(name.to_string());
        }
    }
    Ok(())
}

/// Every send in `body`, in code order: its mutation, its source, and its
/// argument count.
///
/// # Errors
/// A refusal naming a malformed instruction.
pub fn sends(plan: &Plan, body: Code, out: &mut Vec<(u32, String, usize)>) -> Result<(), String> {
    for i in decode(plan, body)? {
        if i.op == Opcode::Send {
            let m = u32::try_from(i.args[0]).map_err(|e| e.to_string())?;
            let argc = usize::try_from(i.args[2]).map_err(|e| e.to_string())?;
            out.push((m, text(plan, i.args[1])?.to_string(), argc));
        }
    }
    Ok(())
}

/// A constant a code body can push.
#[derive(Debug, Clone, PartialEq)]
pub enum Const {
    Num(f64),
    Str(String),
}

/// The number and string constants `body` pushes, in code order.
///
/// # Errors
/// A refusal naming malformed code.
pub fn constants(plan: &Plan, body: Code) -> Result<Vec<Const>, String> {
    let mut out = Vec::new();
    for i in decode(plan, body)? {
        match i.op {
            Opcode::Number => out.push(Const::Num(i.number)),
            Opcode::Str => out.push(Const::Str(text(plan, i.args[0])?.to_string())),
            _ => {}
        }
    }
    Ok(out)
}

/// Whether `body` is one constant push and a return.
///
/// # Errors
/// A refusal naming malformed code.
pub fn is_constant(plan: &Plan, body: Code) -> Result<bool, String> {
    let code = decode(plan, body)?;
    Ok(code.len() == 2
        && matches!(
            code[0].op,
            Opcode::Number | Opcode::Str | Opcode::Bool | Opcode::None | Opcode::Unit
        ))
}

/// The literal a body that pushes one value and returns it gives.
#[derive(Debug, Clone, PartialEq)]
pub enum Literal {
    Num(f64),
    Str(String),
    EmptyList,
}

/// `body`'s literal when it is one push and a return; `None` otherwise.
///
/// # Errors
/// A refusal naming malformed code.
pub fn literal(plan: &Plan, body: Code) -> Result<Option<Literal>, String> {
    let code = decode(plan, body)?;
    if code.len() != 2 || code[1].op != Opcode::Return {
        return Ok(None);
    }
    Ok(match code[0].op {
        Opcode::Number => Some(Literal::Num(code[0].number)),
        Opcode::Str => Some(Literal::Str(text(plan, code[0].args[0])?.to_string())),
        Opcode::List if code[0].args[0] == 0 => Some(Literal::EmptyList),
        _ => None,
    })
}

/// One C++ function named `name` computing `body`.
///
/// # Errors
/// A refusal naming the first unsupported or malformed instruction.
pub fn function(plan: &Plan, body: Code, name: &str) -> Result<String, String> {
    let code = decode(plan, body)?;
    let (max_stack, max_locals) = depths(plan, &code)?;
    let targets: BTreeSet<u64> = code
        .iter()
        .filter(|i| matches!(i.op, Opcode::Jump | Opcode::JumpIfFalse))
        .map(|i| i.args[0])
        .collect();
    let mut out = format!("static V {name}(Ctx& c) {{\n  (void)c;\n");
    let _ = writeln!(out, "  V st[{}]; int sp = 0;", max_stack.max(1));
    if max_locals > 0 {
        let _ = writeln!(out, "  V lc[{max_locals}]; int lp = 0;");
    }
    for i in &code {
        if targets.contains(&(i.pc as u64)) {
            let _ = writeln!(out, "L{}:", i.pc);
        }
        let _ = writeln!(
            out,
            "  {}",
            statement(plan, i).map_err(|e| format!("{name}: {e}"))?
        );
    }
    out.push_str("  return sp > 0 ? st[sp - 1] : V();\n}\n");
    Ok(out)
}
