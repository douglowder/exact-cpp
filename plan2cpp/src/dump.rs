//! A readable listing of a plan: every table the C++ backend reads, with each
//! code body decoded instruction by instruction.

use std::fmt::Write;

use exact_plan::{Code, Plan};

fn code(plan: &Plan, body: Code, out: &mut String) {
    for ins in exact_runner::vm::instructions(plan.code(body)) {
        match ins {
            Ok(i) => {
                let _ = writeln!(
                    out,
                    "      {:>4} {:?} {:?} {}",
                    i.pc, i.op, i.args, i.number
                );
            }
            Err(e) => {
                let _ = writeln!(out, "      trap {e:?}");
            }
        }
    }
}

/// The listing.
#[must_use]
pub fn dump(plan: &Plan) -> String {
    let mut out = String::new();
    for (i, t) in plan.types.iter().enumerate() {
        let _ = writeln!(
            out,
            "type {i}: {:?} {:?} fields {:?} name {:?}",
            t.kind,
            t.elem,
            t.fields,
            plan.str(t.name)
        );
    }
    for (i, s) in plan.slots.iter().enumerate() {
        let _ = writeln!(
            out,
            "slot {i} {} ty {:?} owner {:?} late {}",
            plan.str(s.name),
            s.ty,
            s.owner,
            s.late
        );
        code(plan, s.init, &mut out);
    }
    for (i, d) in plan.derives.iter().enumerate() {
        let _ = writeln!(out, "derive {i} {} ty {:?}", plan.str(d.name), d.ty);
        code(plan, d.body, &mut out);
    }
    for (i, r) in plan.resources.iter().enumerate() {
        let _ = writeln!(
            out,
            "resource {i} {} source {} args {:?} ty {:?}",
            plan.str(r.name),
            plan.str(r.source),
            r.args,
            r.ty
        );
    }
    for (i, a) in plan.actions.iter().enumerate() {
        let _ = writeln!(
            out,
            "action {i} {} params {:?} writes {:?}",
            plan.str(a.name),
            a.params,
            a.writes
        );
        code(plan, a.body, &mut out);
    }
    for (i, t) in plan.timers.iter().enumerate() {
        let _ = writeln!(
            out,
            "timer {i} {} every {} action {:?} once {} frame {} gated {} keyed {}",
            plan.str(t.name),
            t.interval_ms,
            t.action,
            t.once,
            t.frame,
            t.gated,
            t.keyed
        );
    }
    for (i, n) in plan.nodes.iter().enumerate() {
        let _ = writeln!(
            out,
            "node {i} type {} parent {:?} arm {:?} order {}",
            n.node_type, n.parent, n.arm, n.order
        );
        for b in n.bindings.iter() {
            let row = plan.binding(b);
            let _ = writeln!(out, "    binding {:?} id {}", row.kind, row.id);
            code(plan, row.expr, &mut out);
        }
        for h in n.handlers.iter() {
            let row = plan.handler(h);
            let _ = writeln!(
                out,
                "    handler {:?} action {:?} args {:?}",
                row.event, row.action, row.args
            );
        }
    }
    for (i, r) in plan.regions.iter().enumerate() {
        let _ = writeln!(
            out,
            "region {i} {:?} parent {:?} arm {:?} order {} arms {:?}",
            r.kind, r.parent, r.arm, r.order, r.arms
        );
        code(plan, r.subject, &mut out);
    }
    out
}
