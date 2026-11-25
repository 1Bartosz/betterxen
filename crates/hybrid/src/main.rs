use betterxen_hybrid::{crossover, frames, host, json::Json, memory, toolchain};
use std::path::PathBuf;
use std::process::ExitCode;

const USAGE: &str = "usage: betterxen-hybrid <command>
  host                  host receipt (JSON)
  crossover [--app P]   CrossOver installs and documented preferences (JSON)
  toolchain             Windows cross-compilers found on PATH (JSON)
  frames <file>         frame-time statistics; file has one interval in ms per line (JSON)
  guard [--min-start-free N] [--min-free N] -- CMD ARGS...
                        run CMD; refuse to start / stop it when free memory is low
  receipt               host + crossover + toolchain in one JSON document";

fn crossover_json(app: Option<PathBuf>) -> Json {
    let home = PathBuf::from(std::env::var("HOME").unwrap_or_default());
    let cands = match app {
        Some(p) => vec![p],
        None => crossover::candidate_paths(&home),
    };
    let installs = crossover::discover(&cands);
    Json::obj(vec![
        ("installed", Json::Bool(!installs.is_empty())),
        (
            "installs",
            Json::Arr(installs.iter().map(|i| i.to_json()).collect()),
        ),
        (
            "bottle_dir_preference",
            Json::opt_str(crossover::preference("BottleDir")),
        ),
        (
            "managed_bottle_dirs_preference",
            Json::opt_str(crossover::preference("ManagedBottleDirs")),
        ),
        (
            "searched",
            Json::Arr(
                cands
                    .iter()
                    .map(|p| Json::str(p.display().to_string()))
                    .collect(),
            ),
        ),
        (
            "note",
            Json::str(
                "Only Info.plist metadata, lipo -archs and documented defaults keys are read. Bottle graphics backend and bundle internals are not interpreted (undocumented, #350).",
            ),
        ),
    ])
}

fn toolchain_json() -> Json {
    let path = std::env::var("PATH").unwrap_or_default();
    Json::Arr(
        toolchain::probe(&path)
            .iter()
            .map(|c| c.to_json())
            .collect(),
    )
}

fn guard(args: &[String]) -> ExitCode {
    let mut limits = memory::Limits {
        min_start_free: 70,
        min_free: 20,
        poll: std::time::Duration::from_secs(1),
        grace: std::time::Duration::from_secs(10),
    };
    let mut i = 0;
    while i < args.len() && args[i] != "--" {
        let v = args.get(i + 1).and_then(|v| v.parse::<u8>().ok());
        match (args[i].as_str(), v) {
            ("--min-start-free", Some(v)) => limits.min_start_free = v,
            ("--min-free", Some(v)) => limits.min_free = v,
            _ => {
                eprintln!("{USAGE}");
                return ExitCode::from(2);
            }
        }
        i += 2;
    }
    let Some(cmd) = args.get(i + 1) else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };
    match memory::supervise(cmd, &args[i + 2..], limits, &mut memory::free_percentage) {
        memory::Outcome::Exited(Some(0)) => ExitCode::SUCCESS,
        memory::Outcome::Exited(c) => ExitCode::from(c.map_or(1, |c| c.clamp(1, 255)) as u8),
        other => {
            eprintln!("guard: {other:?}");
            ExitCode::from(3)
        }
    }
}

fn main() -> ExitCode {
    if std::env::args().nth(1).as_deref() == Some("guard") {
        return guard(&std::env::args().skip(2).collect::<Vec<_>>());
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result: Result<Json, String> = match args.first().map(String::as_str) {
        Some("host") => Ok(host::collect().to_json()),
        Some("crossover") => {
            let app = match args.get(1).map(String::as_str) {
                Some("--app") => args.get(2).map(PathBuf::from),
                _ => None,
            };
            Ok(crossover_json(app))
        }
        Some("toolchain") => Ok(toolchain_json()),
        Some("frames") => match args.get(1) {
            None => Err("frames needs a file".into()),
            Some(f) => std::fs::read_to_string(f)
                .map_err(|e| format!("{f}: {e}"))
                .and_then(|t| frames::parse_intervals(&t).map_err(|e| e.to_string()))
                .and_then(|v| frames::analyze(&v).map_err(|e| e.to_string()))
                .map(|s| s.to_json()),
        },
        Some("receipt") => Ok(Json::obj(vec![
            ("schema", Json::str("betterxen-hybrid-receipt/0")),
            ("host", host::collect().to_json()),
            ("crossover", crossover_json(None)),
            ("windows_cross_compilers", toolchain_json()),
        ])),
        _ => Err(USAGE.into()),
    };
    match result {
        Ok(j) => {
            print!("{}", j.render());
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
