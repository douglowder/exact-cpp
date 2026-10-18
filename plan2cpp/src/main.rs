//! `plan2cpp dump app.contract` prints the plan's tables and decoded code;
//! `plan2cpp emit app.contract out.cpp` writes the app's C++;
//! `plan2cpp needs app.contract` prints, as JSON, what the app needs of a
//! board and of the extensions it lists;
//! `plan2cpp ext extensions/name.json out.h` writes an extension's C++ base class.

use plan2cpp::{dump, emit, ext};

use std::io::Write;
use std::path::Path;
use std::process::ExitCode;

const USAGE: &str = "usage: plan2cpp dump <app.contract> | plan2cpp emit <app.contract> <out.cpp> | plan2cpp needs <app.contract> | plan2cpp ext <extension.json> <out.h>";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.as_slice() {
        [verb, source] if verb == "dump" => {
            compile(source).and_then(|plan| print(&dump::dump(&plan)))
        }
        [verb, source, out] if verb == "emit" => compile(source)
            .and_then(|plan| emit::emit(&plan, Path::new(source)))
            .and_then(|cpp| std::fs::write(out, cpp).map_err(|e| format!("{out}: {e}"))),
        [verb, source] if verb == "needs" => compile(source)
            .and_then(|plan| {
                let extensions = ext::roster(Path::new(source))?;
                let props = ext::props(Path::new(source), &extensions)?;
                ext::needs_json(&plan, &extensions, &props)
            })
            .and_then(|json| print(&format!("{json}\n"))),
        [verb, file, out] if verb == "ext" => ext::read(Path::new(file)).and_then(|e| {
            std::fs::write(out, ext::header(&e)).map_err(|err| format!("{out}: {err}"))
        }),
        _ => Err(USAGE.to_string()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            let _ = writeln!(std::io::stderr(), "plan2cpp: {e}");
            ExitCode::FAILURE
        }
    }
}

fn print(text: &str) -> Result<(), String> {
    std::io::stdout()
        .write_all(text.as_bytes())
        .map_err(|e| format!("stdout: {e}"))
}

fn compile(source: &str) -> Result<exact_plan::Plan, String> {
    let plan = contract::compile_path(Path::new(source)).map_err(|e| format!("{source}: {e}"))?;
    contract::check(&plan).map_err(|e| format!("{source}: {e}"))?;
    Ok(plan)
}
