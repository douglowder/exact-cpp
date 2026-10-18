//! Host extensions: a prototype of the extension feature the RFC asks exact2
//! for, built outside exact2. An extension file (`extensions/<name>.json`)
//! declares the sources an app may read or send to, with their parameter and
//! result types, and the agent ops the extension adds. An app lists the
//! extensions it uses in `extensions.json` next to its `app.contract`, by
//! name and version, never by path.
//!
//! An extension may also declare props: `data-*` words (LLP 1075.003 §3.3)
//! the app writes on its nodes and the extension reads. exact2 admits a word
//! that the app's `app.json` declares in `data`; plan2cpp binds each such
//! word to the listed extension that declares it.
//!
//! plan2cpp checks each plan source against the declarations, records the
//! extensions in the app's C++ (name, version, SHA-256 of the file) so a board
//! can report what it lacks, prints an app's needs (`plan2cpp needs`), and
//! writes each extension's typed C++ base class (`plan2cpp ext`).

use std::fmt::Write;
use std::path::{Path, PathBuf};

use exact_plan::{Plan, TypeKind};
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::code::{self, cpp_string};

/// A scalar type an extension can declare.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Scalar {
    Number,
    String,
    Bool,
}

impl Scalar {
    fn parse(word: &str, at: &str) -> Result<Self, String> {
        match word {
            "number" => Ok(Self::Number),
            "string" => Ok(Self::String),
            "bool" => Ok(Self::Bool),
            other => Err(format!(
                "{at}: type `{other}` is not number, string, or bool"
            )),
        }
    }

    fn name(self) -> &'static str {
        match self {
            Self::Number => "number",
            Self::String => "string",
            Self::Bool => "bool",
        }
    }

    fn of(kind: TypeKind) -> Option<Self> {
        match kind {
            TypeKind::Number => Some(Self::Number),
            TypeKind::String => Some(Self::String),
            TypeKind::Bool => Some(Self::Bool),
            TypeKind::Unit | TypeKind::Option | TypeKind::List | TypeKind::Record => None,
        }
    }

    fn cpp(self) -> &'static str {
        match self {
            Self::Number => "double",
            Self::String => "std::string",
            Self::Bool => "bool",
        }
    }
}

/// A named, typed value: a parameter or a result field.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Param {
    pub name: String,
    pub ty: Scalar,
}

/// A source: what an app reads as a resource (no parameters) or sends to as
/// a mutation. `live`: its answer changes by itself, and the extension tells
/// the runtime when.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Source {
    pub name: String,
    pub summary: String,
    pub params: Vec<Param>,
    pub result: Vec<Param>,
    pub live: bool,
}

/// An agent op the extension adds, with its request's fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Op {
    pub name: String,
    pub summary: String,
    pub params: Vec<Param>,
}

/// A prop: a `data-*` word the app writes on nodes and the extension reads.
/// Its value is text, as exact2 carries every `data-*` value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Prop {
    pub name: String,
    pub summary: String,
}

/// One extension file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Extension {
    pub name: String,
    pub version: u16,
    pub summary: String,
    /// The SHA-256 of the file's bytes, in hex.
    pub digest: String,
    pub path: PathBuf,
    pub sources: Vec<Source>,
    pub ops: Vec<Op>,
    pub props: Vec<Prop>,
}

/// The agent ops the runtime answers itself; an extension may not take one.
const CORE_OPS: &[&str] = &[
    "state",
    "nodes",
    "key",
    "tap",
    "pan",
    "panrelease",
    "pointer",
    "navigate",
    "logs",
    "stats",
    "screenshot",
    "extensions",
];

/// Words C++ keeps, refused as a field or parameter name.
const CPP_WORDS: &[&str] = &[
    "auto", "bool", "break", "case", "char", "class", "const", "default", "delete", "do", "double",
    "else", "enum", "float", "for", "if", "int", "long", "new", "operator", "private", "public",
    "return", "short", "signed", "static", "struct", "switch", "template", "this", "true", "false",
    "try", "union", "unsigned", "using", "virtual", "void", "while",
];

/// Lowercase words joined by `-`: an extension's name, and a `data-*` word
/// as HTML spells one.
fn is_word(s: &str) -> bool {
    let mut chars = s.chars();
    chars.next().is_some_and(|c| c.is_ascii_lowercase())
        && chars.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
        && !s.ends_with('-')
        && !s.contains("--")
}

fn is_identifier(s: &str) -> bool {
    let mut chars = s.chars();
    chars.next().is_some_and(|c| c.is_ascii_alphabetic())
        && chars.all(|c| c.is_ascii_alphanumeric())
        && !CPP_WORDS.contains(&s)
}

fn text(v: &Value, key: &str, at: &str) -> Result<String, String> {
    match v.get(key) {
        Some(Value::String(s)) => Ok(s.clone()),
        Some(_) => Err(format!("{at}: `{key}` is not a string")),
        None => Ok(String::new()),
    }
}

fn params(v: &Value, key: &str, at: &str) -> Result<Vec<Param>, String> {
    let mut out: Vec<Param> = Vec::new();
    let items = match v.get(key) {
        None => return Ok(out),
        Some(Value::Array(items)) => items,
        Some(_) => return Err(format!("{at}: `{key}` is not a list")),
    };
    for item in items {
        let name = text(item, "name", at)?;
        if !is_identifier(&name) {
            return Err(format!("{at}: `{name}` is not a name C++ can use"));
        }
        if out.iter().any(|p| p.name == name) {
            return Err(format!("{at}: `{name}` is declared twice"));
        }
        let ty = Scalar::parse(&text(item, "type", at)?, &format!("{at} `{name}`"))?;
        out.push(Param { name, ty });
    }
    Ok(out)
}

fn result(v: &Value, at: &str) -> Result<Vec<Param>, String> {
    let Some(Value::Object(fields)) = v.get("result") else {
        return Err(format!("{at}: `result` must be an object of field types"));
    };
    let mut out = Vec::new();
    for (name, ty) in fields {
        if !is_identifier(name) {
            return Err(format!("{at}: `{name}` is not a name C++ can use"));
        }
        let Value::String(word) = ty else {
            return Err(format!("{at} `{name}`: a field's type is a string"));
        };
        out.push(Param {
            name: name.clone(),
            ty: Scalar::parse(word, &format!("{at} `{name}`"))?,
        });
    }
    Ok(out)
}

fn source(v: &Value, file: &str) -> Result<Source, String> {
    let name = text(v, "name", file)?;
    let at = format!("{file}: source `{name}`");
    if !is_identifier(&name) || name.starts_with("exact") {
        return Err(format!(
            "{at}: a source name is letters and digits and does not start with `exact` (the runner's)"
        ));
    }
    let live = match v.get("live") {
        None => false,
        Some(Value::Bool(b)) => *b,
        Some(_) => return Err(format!("{at}: `live` is true or false")),
    };
    Ok(Source {
        summary: text(v, "summary", &at)?,
        params: params(v, "params", &at)?,
        result: result(v, &at)?,
        live,
        name,
    })
}

fn op(v: &Value, file: &str) -> Result<Op, String> {
    let name = text(v, "name", file)?;
    let at = format!("{file}: op `{name}`");
    let lower = name.chars().all(|c| c.is_ascii_lowercase());
    if name.is_empty() || !lower || CORE_OPS.contains(&name.as_str()) {
        return Err(format!(
            "{at}: an op name is lowercase letters and not one of the runtime's ({})",
            CORE_OPS.join(", ")
        ));
    }
    let params = params(v, "params", &at)?;
    if params.iter().any(|p| p.ty == Scalar::Bool) {
        return Err(format!("{at}: an op's fields are numbers or strings"));
    }
    Ok(Op {
        summary: text(v, "summary", &at)?,
        params,
        name,
    })
}

/// The C++ method that reads prop `word`: `snap-align` → `data_snap_align`.
fn prop_method(word: &str) -> String {
    format!("data_{}", word.replace('-', "_"))
}

fn prop(v: &Value, file: &str, sources: &[Source]) -> Result<Prop, String> {
    let name = text(v, "name", file)?;
    let at = format!("{file}: prop `{name}`");
    if !is_word(&name) {
        return Err(format!(
            "{at}: a prop is a `data-*` word: lowercase words joined by `-`"
        ));
    }
    let method = prop_method(&name);
    let taken =
        |s: &&Source| snake(&s.name) == method || format!("{}_changed", snake(&s.name)) == method;
    if let Some(s) = sources.iter().find(taken) {
        return Err(format!(
            "{at}: its C++ method `{method}` is source `{}`'s",
            s.name
        ));
    }
    Ok(Prop {
        summary: text(v, "summary", &at)?,
        name,
    })
}

fn list<'a>(v: &'a Value, key: &str, at: &str) -> Result<&'a [Value], String> {
    match v.get(key) {
        None => Ok(&[]),
        Some(Value::Array(items)) => Ok(items),
        Some(_) => Err(format!("{at}: `{key}` is not a list")),
    }
}

/// Reads and checks one extension file.
///
/// # Errors
/// The file cannot be read, is not JSON, or breaks a rule named in the error.
pub fn read(path: &Path) -> Result<Extension, String> {
    let at = path.display().to_string();
    let bytes = std::fs::read(path).map_err(|e| format!("{at}: {e}"))?;
    let v: Value = serde_json::from_slice(&bytes).map_err(|e| format!("{at}: {e}"))?;
    if let Value::Object(map) = &v {
        for key in map.keys() {
            if !["extension", "version", "summary", "sources", "ops", "props"]
                .contains(&key.as_str())
            {
                return Err(format!("{at}: unknown key `{key}`"));
            }
        }
    } else {
        return Err(format!("{at}: an extension file is a JSON object"));
    }
    let name = text(&v, "extension", &at)?;
    if !is_word(&name) {
        return Err(format!(
            "{at}: `extension` is lowercase words joined by `-`, not `{name}`"
        ));
    }
    let version = v
        .get("version")
        .and_then(Value::as_u64)
        .and_then(|n| u16::try_from(n).ok())
        .filter(|n| *n > 0)
        .ok_or_else(|| format!("{at}: `version` is a whole number from 1 to 65535"))?;
    let mut sources: Vec<Source> = Vec::new();
    for item in list(&v, "sources", &at)? {
        let s = source(item, &at)?;
        if sources.iter().any(|x| x.name == s.name) {
            return Err(format!("{at}: source `{}` is declared twice", s.name));
        }
        if pascal(&s.name) == class_name(&name) {
            return Err(format!(
                "{at}: source `{}` would share its C++ name with the extension's class",
                s.name
            ));
        }
        sources.push(s);
    }
    let mut ops: Vec<Op> = Vec::new();
    for item in list(&v, "ops", &at)? {
        let o = op(item, &at)?;
        if ops.iter().any(|x| x.name == o.name) {
            return Err(format!("{at}: op `{}` is declared twice", o.name));
        }
        ops.push(o);
    }
    let mut props: Vec<Prop> = Vec::new();
    for item in list(&v, "props", &at)? {
        let p = prop(item, &at, &sources)?;
        if props.iter().any(|x| x.name == p.name) {
            return Err(format!("{at}: prop `{}` is declared twice", p.name));
        }
        props.push(p);
    }
    let mut digest = String::new();
    for b in Sha256::digest(&bytes) {
        let _ = write!(digest, "{b:02x}");
    }
    Ok(Extension {
        summary: text(&v, "summary", &at)?,
        name,
        version,
        digest,
        path: path.to_path_buf(),
        sources,
        ops,
        props,
    })
}

/// Where extension `name` lives: `extensions/<name>.json` in the nearest
/// directory at or above `dir` that has one.
fn find(dir: &Path, name: &str) -> Option<PathBuf> {
    let file = format!("{name}.json");
    dir.ancestors()
        .map(|d| d.join("extensions").join(&file))
        .find(|p| p.is_file())
}

/// The extensions the app at `source` lists in the `extensions.json` beside
/// it (`{"extensions": {"<name>": <version>}}`); none without that file.
///
/// # Errors
/// The roster or an extension cannot be read, names an extension no
/// `extensions/` directory has, asks another version than the file's, or two
/// extensions declare one source, op, or prop.
pub fn roster(source: &Path) -> Result<Vec<Extension>, String> {
    let dir = source.parent().unwrap_or_else(|| Path::new("."));
    let path = dir.join("extensions.json");
    if !path.is_file() {
        return Ok(Vec::new());
    }
    let at = path.display().to_string();
    let bytes = std::fs::read(&path).map_err(|e| format!("{at}: {e}"))?;
    let v: Value = serde_json::from_slice(&bytes).map_err(|e| format!("{at}: {e}"))?;
    let Some(Value::Object(names)) = v.get("extensions") else {
        return Err(format!(
            "{at}: expected {{\"extensions\": {{\"<name>\": <version>}}}}"
        ));
    };
    let mut out: Vec<Extension> = Vec::new();
    for (name, version) in names {
        let file = find(dir, name).ok_or_else(|| {
            format!(
                "{at}: no extensions/{name}.json at or above {}",
                dir.display()
            )
        })?;
        let ext = read(&file)?;
        if ext.name != *name || version.as_u64() != Some(u64::from(ext.version)) {
            return Err(format!(
                "{at}: asks for {name} version {version}, but {} is {} version {}",
                file.display(),
                ext.name,
                ext.version
            ));
        }
        for other in &out {
            for s in &ext.sources {
                if other.sources.iter().any(|x| x.name == s.name) {
                    return Err(format!(
                        "{at}: {} and {name} both declare source `{}`",
                        other.name, s.name
                    ));
                }
            }
            for o in &ext.ops {
                if other.ops.iter().any(|x| x.name == o.name) {
                    return Err(format!(
                        "{at}: {} and {name} both declare op `{}`",
                        other.name, o.name
                    ));
                }
            }
            for p in &ext.props {
                if other.props.iter().any(|x| x.name == p.name) {
                    return Err(format!(
                        "{at}: {} and {name} both declare prop `{}`",
                        other.name, p.name
                    ));
                }
            }
        }
        out.push(ext);
    }
    Ok(out)
}

/// A source the plan uses, bound to its extension.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Use {
    pub source: String,
    /// The index of its extension in the roster.
    pub extension: usize,
    /// The plan type of its answer (a record).
    pub ty: u32,
}

fn declared<'a>(exts: &'a [Extension], name: &str) -> Option<(usize, &'a Source)> {
    for (i, e) in exts.iter().enumerate() {
        if let Some(s) = e.sources.iter().find(|s| s.name == name) {
            return Some((i, s));
        }
    }
    None
}

/// Checks that plan type `ty` is a record whose fields the declaration has,
/// each with the declared type. The app may read fewer fields than declared.
fn check_type(plan: &Plan, ty: u32, decl: &Source, at: &str) -> Result<u32, String> {
    let t = &plan.types[ty as usize];
    if t.kind == TypeKind::Option {
        if let Some(inner) = t.elem {
            return check_type(plan, inner.0, decl, at);
        }
    }
    if t.kind != TypeKind::Record {
        return Err(format!(
            "{at}: the app reads it as {:?}, but it answers a record",
            t.kind
        ));
    }
    let start = t.fields.start as usize;
    for f in &plan.fields[start..start + t.fields.len as usize] {
        let name = plan.str(f.name);
        let Some(d) = decl.result.iter().find(|d| d.name == name) else {
            return Err(format!(
                "{at}: the app reads field `{name}`, which the extension does not declare"
            ));
        };
        let kind = plan.types[f.ty.0 as usize].kind;
        if Scalar::of(kind) != Some(d.ty) {
            return Err(format!(
                "{at}: the app reads `{name}` as {kind:?}, but the extension declares {}",
                d.ty.name()
            ));
        }
    }
    Ok(ty)
}

/// Binds every source the plan reads or sends to (other than the runner's
/// own) to a declaration in `exts`, checking its types and argument count.
///
/// # Errors
/// A source no extension declares, or one the app uses with other types.
pub fn check(plan: &Plan, exts: &[Extension]) -> Result<Vec<Use>, String> {
    let mut out: Vec<Use> = Vec::new();
    let mut bind = |source: &str, ty: u32, argc: usize, what: &str| -> Result<(), String> {
        let at = format!("{what} `{source}`");
        let Some((ext, decl)) = declared(exts, source) else {
            let listed: Vec<&str> = exts.iter().map(|e| e.name.as_str()).collect();
            return Err(format!(
                "{at}: no extension the app lists declares it (listed: {})",
                if listed.is_empty() {
                    "none".to_string()
                } else {
                    listed.join(", ")
                }
            ));
        };
        if decl.params.len() != argc {
            return Err(format!(
                "{at}: the app gives {argc} arguments, but {} declares {}",
                exts[ext].name,
                decl.params.len()
            ));
        }
        let ty = check_type(plan, ty, decl, &at)?;
        match out.iter().find(|u| u.source == source) {
            Some(u) if u.ty != ty => Err(format!("{at}: the app reads it as two types")),
            Some(_) => Ok(()),
            None => {
                out.push(Use {
                    source: source.to_string(),
                    extension: ext,
                    ty,
                });
                Ok(())
            }
        }
    };
    for r in &plan.resources {
        let source = plan.str(r.source);
        if !exact_plan::runner_owned_source(source) {
            bind(source, r.ty.0, r.args.len as usize, "resource source")?;
        }
    }
    let mut all = Vec::new();
    for a in &plan.actions {
        code::sends(plan, a.body, &mut all)?;
    }
    for (m, source, argc) in all {
        let ty = plan
            .mutations
            .get(m as usize)
            .ok_or_else(|| format!("a send names mutation {m}, which the plan lacks"))?
            .ty
            .0;
        bind(&source, ty, argc, "send to")?;
    }
    Ok(out)
}

/// A `data-*` word the app declares, bound to its extension.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PropUse {
    pub word: String,
    /// The index of its extension in the roster.
    pub extension: usize,
}

/// Binds each `data-*` word that the app at `source` declares (its
/// `app.json` `data`, read as exact2 reads it) to the listed extension that
/// declares it as a prop.
///
/// # Errors
/// `app.json` cannot be read, or a word no listed extension declares.
pub fn props(source: &Path, exts: &[Extension]) -> Result<Vec<PropUse>, String> {
    let dir = source.parent().unwrap_or_else(|| Path::new("."));
    let manifest = contract::Manifest::read(dir)?;
    let mut out = Vec::new();
    for word in contract::native::data_words(&manifest)? {
        let Some(extension) = exts
            .iter()
            .position(|e| e.props.iter().any(|p| p.name == word))
        else {
            let listed: Vec<&str> = exts.iter().map(|e| e.name.as_str()).collect();
            return Err(format!(
                "app.json word `data-{word}`: no extension the app lists declares it as a prop (listed: {})",
                if listed.is_empty() {
                    "none".to_string()
                } else {
                    listed.join(", ")
                }
            ));
        };
        out.push(PropUse { word, extension });
    }
    Ok(out)
}

/// The C++ tables the runtime binds extensions with: the roster
/// (`kExtensions`) and each used source's extension and answer type
/// (`kExtensionSources`).
pub fn tables(exts: &[Extension], uses: &[Use], s: &mut String) {
    let rows: Vec<String> = exts
        .iter()
        .map(|e| {
            format!(
                "{{{}, {}, {}}}",
                cpp_string(&e.name),
                e.version,
                cpp_string(&e.digest)
            )
        })
        .collect();
    let sources: Vec<String> = uses
        .iter()
        .map(|u| format!("{{{}, {}, {}}}", cpp_string(&u.source), u.extension, u.ty))
        .collect();
    let _ = writeln!(
        s,
        "const rt::ExtensionUse kExtensions[] = {{{}}};",
        if rows.is_empty() {
            "{\"\", 0, \"\"}".into()
        } else {
            rows.join(", ")
        }
    );
    let _ = writeln!(
        s,
        "const rt::ExtensionSource kExtensionSources[] = {{{}}};",
        if sources.is_empty() {
            "{\"\", 0, 0}".into()
        } else {
            sources.join(", ")
        }
    );
}

fn json_string(s: &str) -> String {
    Value::String(s.to_string()).to_string()
}

fn json_params(params: &[Param]) -> String {
    let fields: Vec<String> = params
        .iter()
        .map(|p| format!("{}:{}", json_string(&p.name), json_string(p.ty.name())))
        .collect();
    format!("{{{}}}", fields.join(","))
}

/// What an app needs, as one JSON object: what it asks of a board (`view`,
/// `keys`, `touch`, `commands`) and, for each extension it lists, the
/// version and digest, the sources it uses (with their types), the ops, and
/// the props (`data-*` words) it writes.
///
/// # Errors
/// A refusal from [`check`] or a malformed instruction.
pub fn needs_json(plan: &Plan, exts: &[Extension], props: &[PropUse]) -> Result<String, String> {
    let uses = check(plan, exts)?;
    let board = crate::emit::needs(plan)?;
    let commands: Vec<String> = board.commands.iter().map(|c| json_string(c)).collect();
    let mut out = format!(
        "{{\"view\":{},\"keys\":{},\"touch\":{},\"commands\":[{}],\"extensions\":[",
        !plan.nodes.is_empty(),
        board.keys,
        board.touch,
        commands.join(",")
    );
    for (i, e) in exts.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        let mut sources = Vec::new();
        for s in &e.sources {
            if uses.iter().any(|u| u.source == s.name) {
                sources.push(format!(
                    "{{\"name\":{},\"params\":{},\"result\":{},\"live\":{}}}",
                    json_string(&s.name),
                    json_params(&s.params),
                    json_params(&s.result),
                    s.live
                ));
            }
        }
        let ops: Vec<String> = e
            .ops
            .iter()
            .map(|o| {
                format!(
                    "{{\"name\":{},\"params\":{}}}",
                    json_string(&o.name),
                    json_params(&o.params)
                )
            })
            .collect();
        let words: Vec<String> = props
            .iter()
            .filter(|p| p.extension == i)
            .map(|p| json_string(&p.word))
            .collect();
        let _ = write!(
            out,
            "{{\"name\":{},\"version\":{},\"sha256\":{},\"sources\":[{}],\"ops\":[{}],\"props\":[{}]}}",
            json_string(&e.name),
            e.version,
            json_string(&e.digest),
            sources.join(","),
            ops.join(","),
            words.join(",")
        );
    }
    out.push_str("]}");
    Ok(out)
}

/// `badgeBattery` → `badge_battery`.
fn snake(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars() {
        if c.is_ascii_uppercase() {
            if !out.is_empty() {
                out.push('_');
            }
            out.push(c.to_ascii_lowercase());
        } else {
            out.push(c);
        }
    }
    out
}

/// The C++ class for extension `name`: `init-badge` → `InitBadgeExtension`.
fn class_name(name: &str) -> String {
    format!("{}Extension", pascal(name))
}

/// `init-badge` → `InitBadge`; `badgeBattery` → `BadgeBattery`.
fn pascal(name: &str) -> String {
    let mut out = String::new();
    for word in name.split('-') {
        let mut chars = word.chars();
        if let Some(c) = chars.next() {
            out.push(c.to_ascii_uppercase());
            out.extend(chars);
        }
    }
    out
}

fn value(p: &Param, expr: &str) -> String {
    match p.ty {
        Scalar::Number => format!("rt::V::num({expr})"),
        Scalar::String => format!("rt::V::str({expr})"),
        Scalar::Bool => format!("rt::V::boolean({expr})"),
    }
}

fn arg_check(p: &Param, i: usize) -> String {
    let kind = match p.ty {
        Scalar::Number => "Num",
        Scalar::String => "Str",
        Scalar::Bool => "Bool",
    };
    format!("args[{i}].k == rt::V::K::{kind}")
}

fn arg_value(p: &Param, i: usize) -> String {
    match p.ty {
        Scalar::Number => format!("args[{i}].n"),
        Scalar::String => format!("args[{i}].s"),
        Scalar::Bool => format!("args[{i}].b"),
    }
}

fn header_decls(e: &Extension, s: &mut String) {
    for src in &e.sources {
        let _ = writeln!(
            s,
            "  // {}",
            if src.summary.is_empty() {
                &src.name
            } else {
                &src.summary
            }
        );
        let fields: Vec<String> = src
            .result
            .iter()
            .map(|f| {
                let init = match f.ty {
                    Scalar::Number => " = 0",
                    Scalar::Bool => " = false",
                    Scalar::String => "",
                };
                format!("{} {}{init};", f.ty.cpp(), f.name)
            })
            .collect();
        let _ = writeln!(
            s,
            "  struct {} {{ {} }};",
            pascal(&src.name),
            fields.join(" ")
        );
        let mut params: Vec<String> = src
            .params
            .iter()
            .map(|p| match p.ty {
                Scalar::String => format!("const std::string& {}", p.name),
                other => format!("{} {}", other.cpp(), p.name),
            })
            .collect();
        params.push(format!("{}& out", pascal(&src.name)));
        let _ = writeln!(
            s,
            "  virtual bool {}({}) = 0;",
            snake(&src.name),
            params.join(", ")
        );
    }
    for o in &e.ops {
        let _ = writeln!(s, "  // The `{}` op: {}", o.name, o.summary);
        let mut params = vec!["rt::Runtime& runtime".to_string()];
        for p in &o.params {
            params.push(match p.ty {
                Scalar::String => format!("const std::string& {}", p.name),
                other => format!("{} {}", other.cpp(), p.name),
            });
        }
        let _ = writeln!(
            s,
            "  virtual std::string {}({}) = 0;",
            o.name,
            params.join(", ")
        );
    }
}

fn header_answer(e: &Extension, s: &mut String) {
    s.push_str("  bool answer(const rt::App& app, const char* source, const rt::V* args, int argc, rt::V& out) override {\n");
    if e.sources.is_empty() {
        s.push_str("    (void)app;\n    (void)source;\n    (void)args;\n    (void)argc;\n    (void)out;\n    return false;\n  }\n");
        return;
    }
    s.push_str("    const int ty = rt::source_type(app, source);\n    if (ty < 0) return false;\n");
    if e.sources.iter().all(|src| src.params.is_empty()) {
        s.push_str("    (void)args;\n");
    }
    for src in &e.sources {
        let mut cond = vec![
            format!("std::strcmp(source, {}) == 0", cpp_string(&src.name)),
            format!("argc == {}", src.params.len()),
        ];
        let mut call = Vec::new();
        for (i, p) in src.params.iter().enumerate() {
            cond.push(arg_check(p, i));
            call.push(arg_value(p, i));
        }
        call.push("r".into());
        let fields: Vec<String> = src
            .result
            .iter()
            .map(|f| {
                format!(
                    "{{{}, {}}}",
                    cpp_string(&f.name),
                    value(f, &format!("r.{}", f.name))
                )
            })
            .collect();
        let _ = writeln!(
            s,
            "    if ({}) {{\n      {} r;\n      if (!{}({})) return false;\n      out = rt::record(app, static_cast<uint16_t>(ty), {{{}}});\n      return true;\n    }}",
            cond.join(" && "),
            pascal(&src.name),
            snake(&src.name),
            call.join(", "),
            fields.join(", ")
        );
    }
    s.push_str("    return false;\n  }\n");
}

fn header_op(e: &Extension, s: &mut String) {
    s.push_str("  bool op(rt::Runtime& runtime, const std::string& line, std::string& answer) override {\n");
    if e.ops.is_empty() {
        s.push_str(
            "    (void)runtime;\n    (void)line;\n    (void)answer;\n    return false;\n  }\n",
        );
        return;
    }
    s.push_str("    const std::string name = rt::string_field(line, \"op\");\n");
    for o in &e.ops {
        let mut call = vec!["runtime".to_string()];
        for p in &o.params {
            let field = cpp_string(&p.name);
            call.push(match p.ty {
                Scalar::String => format!("rt::string_field(line, {field})"),
                Scalar::Number | Scalar::Bool => format!("rt::number_field(line, {field})"),
            });
        }
        let _ = writeln!(
            s,
            "    if (name == {}) {{\n      answer = {}({});\n      return true;\n    }}",
            cpp_string(&o.name),
            o.name,
            call.join(", ")
        );
    }
    s.push_str("    return false;\n  }\n");
}

/// The typed C++ base class for extension `e`: a struct and a pure virtual
/// method per source, a pure virtual method per op, and the `rt::Extension`
/// overrides that turn the runtime's untyped calls into them.
#[must_use]
pub fn header(e: &Extension) -> String {
    let class = class_name(&e.name);
    let mut s = format!(
        "// Generated by plan2cpp from {} (version {}). Do not edit.\n",
        e.path.display(),
        e.version
    );
    if !e.summary.is_empty() {
        let _ = writeln!(s, "// {}", e.summary);
    }
    let names = |items: Vec<&str>| {
        if items.is_empty() {
            "nullptr".to_string()
        } else {
            items
                .iter()
                .map(|n| cpp_string(n))
                .collect::<Vec<_>>()
                .join(", ")
        }
    };
    let live: Vec<&Source> = e.sources.iter().filter(|x| x.live).collect();
    let _ = write!(
        s,
        "#pragma once\n\n#include <cstring>\n#include <string>\n#include <vector>\n\n#include \"contract_rt.h\"\n\nnamespace ext {{\n\nclass {class} : public rt::Extension {{\n public:\n  static constexpr const char* kSources[] = {{{}}};\n  static constexpr const char* kOps[] = {{{}}};\n  static constexpr const char* kProps[] = {{{}}};\n\n  const char* name() const override {{ return {}; }}\n  uint16_t version() const override {{ return {}; }}\n  const char* digest() const override {{ return {}; }}\n  const char* const* sources(uint16_t& count) const override {{\n    count = {};\n    return kSources;\n  }}\n  const char* const* ops(uint16_t& count) const override {{\n    count = {};\n    return kOps;\n  }}\n  const char* const* props(uint16_t& count) const override {{\n    count = {};\n    return kProps;\n  }}\n\n",
        names(e.sources.iter().map(|x| x.name.as_str()).collect()),
        names(e.ops.iter().map(|x| x.name.as_str()).collect()),
        names(e.props.iter().map(|x| x.name.as_str()).collect()),
        cpp_string(&e.name),
        e.version,
        cpp_string(&e.digest),
        e.sources.len(),
        e.ops.len(),
        e.props.len()
    );
    header_decls(e, &mut s);
    s.push('\n');
    header_answer(e, &mut s);
    header_op(e, &mut s);
    if !live.is_empty() || !e.props.is_empty() {
        s.push_str("\n protected:\n");
    }
    for p in &e.props {
        let _ = writeln!(
            s,
            "  // The nodes that carry `data-{}`{}.\n  std::vector<rt::Mark> {}(const rt::Runtime& runtime) const {{ return runtime.marks({}); }}",
            p.name,
            if p.summary.is_empty() {
                String::new()
            } else {
                format!(": {}", p.summary)
            },
            prop_method(&p.name),
            cpp_string(&p.name)
        );
    }
    for src in live {
        let _ = writeln!(
            s,
            "  // `{}` changed: the runtime asks it again.\n  void {}_changed(rt::Runtime& runtime) {{ runtime.changed({}); }}",
            src.name,
            snake(&src.name),
            cpp_string(&src.name)
        );
    }
    let _ = write!(s, "}};\n\n}}  // namespace ext\n");
    s
}
