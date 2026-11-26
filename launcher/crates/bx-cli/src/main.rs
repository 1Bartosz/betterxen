//! Track A (M1-A1): Native ARM64 macOS CLI for BetterXen hybrid execution.

mod supervise;

use std::collections::BTreeMap;
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};
use std::time::Duration;

use bx_probe::{ShimTrack, classify_dll, probe_pe32_plus};
use bx_telemetry::summarize_jsonl;
use dxbc_accc::inspect_dxbc;
use supervise::{Outcome, Stop};

const SHIM_DLL_OVERRIDES: &str = "bx_runtime_veh,d3d12_xs,xg_xs,xmem,XCRDAPI,xgameruntime,AcpHal,xapu,chad,GameInput,\
api-ms-win-core-psm-appnotify-l1-1-0,api-ms-win-core-psm-appnotify-l1-1-1,\
ext-ms-win-rtcore-ntuser-message-l1-1-0,ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0,\
ext-ms-win-rtcore-ntuser-window-l1-1-0,ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0=n,b";

/// Accepted `--backend` values. CrossOver ignores an unknown value silently, so one is refused.
const BACKENDS: [&str; 4] = ["d3dmetal", "dxmt", "dxvk", "wined3d"];

const SAFE_CHILD_PATH: &str = "/usr/bin:/bin:/usr/sbin:/sbin";

const HOST_ENV_ALLOWLIST: &[&str] = &[
    "HOME",
    "USER",
    "LOGNAME",
    "TMPDIR",
    "LANG",
    "LC_ALL",
    "TERM",
    "DISPLAY",
    "XPC_FLAGS",
    "XPC_SERVICE_NAME",
];

fn print_usage() {
    eprintln!(
        "BetterXen Hybrid CLI (bx-cli)\n\
         \n\
         Usage:\n\
           bx-cli doctor                         Inspect Apple Silicon host, Rosetta 2, CrossOver & mingw-w64\n\
           bx-cli probe <pe32+_path>             Classify PE32+ imports by BetterXen track (A-E)\n\
           bx-cli shader <dxbc_blob_path>        Classify a DXBC shader container (DXIL vs ACCC/FSPX)\n\
           bx-cli trace <log.jsonl>              Summarize bx_runtime_veh JSONL diagnostics\n\
           bx-cli run --exe <path> [--shims <dir>] [--bottle <name> | --prefix <dir>] [--profile lockhart|anaconda] [--log <path>] [--inherit-env] [--timeout <secs>] [--backend d3dmetal|dxmt|dxvk|wined3d]\n\
           (shims default to build-shims/bin; a timeout or signal stops the whole bottle or prefix)\n"
    );
}

fn run_capture(program: &str, args: &[&str]) -> Option<String> {
    let out = Command::new(program).args(args).output().ok()?;
    if out.status.success() {
        Some(String::from_utf8_lossy(&out.stdout).trim().to_string())
    } else {
        None
    }
}

fn cmd_doctor() -> ExitCode {
    let arch = run_capture("uname", &["-m"]).unwrap_or_else(|| "unknown".into());
    let macos = run_capture("sw_vers", &["-productVersion"]).unwrap_or_else(|| "unknown".into());
    let cpu = run_capture("sysctl", &["-n", "machdep.cpu.brand_string"])
        .unwrap_or_else(|| "unknown".into());
    let mem_bytes: u64 = run_capture("sysctl", &["-n", "hw.memsize"])
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    let mem_gb = mem_bytes / (1024 * 1024 * 1024);
    let default_profile = if mem_gb >= 32 { "anaconda" } else { "lockhart" };

    let crossover_ver = run_capture(
        "defaults",
        &[
            "read",
            "/Applications/CrossOver.app/Contents/Info.plist",
            "CFBundleShortVersionString",
        ],
    );
    let d3dmetal_exists = Path::new(
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib64/apple_gptk/external/D3DMetal.framework",
    )
    .exists();
    let mingw = run_capture("which", &["x86_64-w64-mingw32-g++"]);
    let rosetta = Command::new("pgrep")
        .args(["-q", "oahd"])
        .status()
        .is_ok_and(|s| s.success());

    println!("=== BetterXen Host Doctor ===");
    println!("Host arch        : {arch}");
    println!("CPU              : {cpu}");
    println!("macOS            : {macos}");
    println!("Unified memory   : {mem_gb} GiB (recommended profile: {default_profile})");
    println!(
        "Rosetta 2 (oahd) : {}",
        if rosetta { "active" } else { "not detected" }
    );
    println!(
        "CrossOver        : {}",
        crossover_ver.as_deref().unwrap_or("not installed")
    );
    println!(
        "D3DMetal.fwk     : {}",
        if d3dmetal_exists {
            "present"
        } else {
            "not found"
        }
    );
    println!(
        "mingw-w64 g++    : {}",
        mingw
            .as_deref()
            .unwrap_or("missing (brew install mingw-w64)")
    );
    ExitCode::SUCCESS
}

fn cmd_probe(path: &str) -> ExitCode {
    let data = match fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let summary = match probe_pe32_plus(&data) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error: {e}");
            return ExitCode::FAILURE;
        }
    };
    let total_syms: usize = summary.imports.values().map(Vec::len).sum();
    println!("ImageBase        : 0x{:x}", summary.image_base);
    println!("SizeOfImage      : {} bytes", summary.size_of_image);
    println!("EntryPoint RVA   : 0x{:x}", summary.entry_point_rva);
    println!("Sections         : {}", summary.section_count);
    println!(
        "Static Imports   : {} DLLs, {} symbols",
        summary.imports.len(),
        total_syms
    );

    let mut by_track: BTreeMap<ShimTrack, Vec<(&String, usize)>> = BTreeMap::new();
    for (dll, syms) in &summary.imports {
        by_track
            .entry(classify_dll(dll))
            .or_default()
            .push((dll, syms.len()));
    }
    for (track, dlls) in by_track {
        println!("\n[{track}] ({} DLLs):", dlls.len());
        for (dll, cnt) in dlls {
            println!("  - {dll} ({cnt})");
        }
    }
    ExitCode::SUCCESS
}

fn format_clwb_line(clwb_events: usize, clwb_trap_lower_bound: Option<u64>) -> String {
    if let Some(lower_bound) = clwb_trap_lower_bound {
        format!("clwb VEH traps   : at least {lower_bound} (sampled)")
    } else if clwb_events == 0 {
        "clwb VEH traps   : 0".to_string()
    } else {
        format!("clwb VEH samples : {clwb_events}")
    }
}

fn cmd_trace(path: &str) -> ExitCode {
    let text = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    print!("{}", render_trace(&text));
    ExitCode::SUCCESS
}

fn render_trace(text: &str) -> String {
    let mut out = String::new();
    let summary = summarize_jsonl(text);
    let _ = writeln!(out, "Trace events     : {}", summary.total_lines);
    let _ = writeln!(
        out,
        "{}",
        format_clwb_line(summary.clwb_events, summary.clwb_trap_lower_bound)
    );
    let _ = writeln!(out, "ADX illegal ops  : {}", summary.adx_errors);
    if !summary.unhandled_exceptions.is_empty() {
        let _ = writeln!(
            out,
            "\nUnhandled exceptions ({}):",
            summary.unhandled_exceptions.len()
        );
        for crash in summary.unhandled_exceptions {
            let _ = writeln!(
                out,
                "  code={} module_base={} + RVA={} RIP={}",
                crash.exception_code.as_deref().unwrap_or("unknown"),
                crash.module_base.as_deref().unwrap_or("unknown"),
                crash.rva.as_deref().unwrap_or("unknown"),
                crash.rip.as_deref().unwrap_or("unknown"),
            );
        }
    }
    if !summary.unimpl_apis.is_empty() {
        let _ = writeln!(out, "\nUnimplemented APIs:");
        for (api, count) in summary.unimpl_apis {
            let _ = writeln!(out, "  {count:5}x  {api}");
        }
    }
    if !summary.unimpl_vtables.is_empty() {
        let _ = writeln!(out, "\nUnimplemented COM vtable slots:");
        for (slot, count) in summary.unimpl_vtables {
            let _ = writeln!(out, "  {count:5}x  {slot}");
        }
    }
    out
}

fn unix_to_wine_z_path(path: &Path) -> String {
    let s = path.to_string_lossy();
    format!("Z:{}", s.replace('/', "\\"))
}

fn collect_isolated_host_env<F>(mut lookup: F) -> BTreeMap<String, String>
where
    F: FnMut(&str) -> Option<String>,
{
    let mut env_map = BTreeMap::new();
    env_map.insert("PATH".to_string(), SAFE_CHILD_PATH.to_string());
    for &key in HOST_ENV_ALLOWLIST {
        if let Some(val) = lookup(key) {
            env_map.insert(key.to_string(), val);
        }
    }
    env_map
}

#[derive(Debug)]
struct RunArgs {
    exe: PathBuf,
    shims_dir: PathBuf,
    profile: String,
    bottle: Option<String>,
    prefix: Option<PathBuf>,
    log: Option<PathBuf>,
    inherit_env: bool,
    timeout: Option<Duration>,
    backend: Option<String>,
}

fn parse_run_args(args: &[String]) -> Result<RunArgs, String> {
    let mut exe_path: Option<PathBuf> = None;
    let mut shims_dir = PathBuf::from("build-shims/bin");
    let mut profile = String::from("lockhart");
    let mut bottle: Option<String> = None;
    let mut wine_prefix: Option<PathBuf> = None;
    let mut log_file: Option<PathBuf> = None;
    let mut inherit_env = false;
    let mut timeout = None;
    let mut backend: Option<String> = None;

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--exe" if i + 1 < args.len() => {
                exe_path = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--shims" if i + 1 < args.len() => {
                shims_dir = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--profile" if i + 1 < args.len() => {
                profile = args[i + 1].clone();
                i += 2;
            }
            "--bottle" if i + 1 < args.len() => {
                bottle = Some(args[i + 1].clone());
                i += 2;
            }
            "--prefix" if i + 1 < args.len() => {
                wine_prefix = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--log" if i + 1 < args.len() => {
                log_file = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--timeout" if i + 1 < args.len() => {
                let seconds: u64 = args[i + 1]
                    .parse()
                    .ok()
                    .filter(|s| *s > 0)
                    .ok_or("--timeout needs a whole number of seconds above 0")?;
                timeout = Some(Duration::from_secs(seconds));
                i += 2;
            }
            "--backend" if i + 1 < args.len() => {
                let name = args[i + 1].clone();
                if !BACKENDS.contains(&name.as_str()) {
                    return Err(format!(
                        "unknown backend {name:?}; use one of {}",
                        BACKENDS.join(", ")
                    ));
                }
                backend = Some(name);
                i += 2;
            }
            "--inherit-env" => {
                inherit_env = true;
                i += 1;
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }

    if let Some(name) = &bottle
        && (name.is_empty() || name.starts_with(['-', '.']) || name.contains('/'))
    {
        return Err(format!("invalid bottle name {name:?}"));
    }
    Ok(RunArgs {
        exe: exe_path.ok_or("error: --exe <path> is required")?,
        shims_dir,
        profile,
        bottle,
        prefix: wine_prefix,
        log: log_file,
        inherit_env,
        timeout,
        backend,
    })
}

/// `bin/wine` deletes an inherited `WINEDLLOVERRIDES` and applies the bottle's
/// `[EnvironmentVariables]` over what it is given, so bottle mode passes both as options.
fn bottle_options(backend: Option<&str>) -> Vec<String> {
    let mut options = vec!["--dll".to_string(), SHIM_DLL_OVERRIDES.to_string()];
    if let Some(backend) = backend {
        options.push("--env".to_string());
        options.push(format!("CX_GRAPHICS_BACKEND={backend}"));
    }
    options
}

fn cmd_run(args: &[String]) -> ExitCode {
    let run = match parse_run_args(args) {
        Ok(run) => run,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::FAILURE;
        }
    };
    let RunArgs {
        exe,
        shims_dir,
        profile,
        bottle,
        prefix: wine_prefix,
        log: log_file,
        inherit_env,
        timeout,
        backend,
    } = run;

    let cx_root = Path::new("/Applications/CrossOver.app/Contents/SharedSupport/CrossOver");
    let wine_wrapper = cx_root.join("bin/wine");
    let wineloader = cx_root.join("bin/wineloader");
    if !wineloader.exists() {
        eprintln!("error: CrossOver not found under /Applications/CrossOver.app");
        return ExitCode::FAILURE;
    }

    let abs_shims = fs::canonicalize(&shims_dir).unwrap_or(shims_dir);
    let abs_exe = fs::canonicalize(&exe).unwrap_or(exe);
    let wine_shims_path = unix_to_wine_z_path(&abs_shims);

    let resolved_prefix =
        wine_prefix.unwrap_or_else(|| PathBuf::from("/tmp/betterxen-wine-prefix"));

    // The environment a timeout or signal has to stop.
    let environment_dir = match &bottle {
        Some(name) => PathBuf::from(env::var("HOME").unwrap_or_default())
            .join("Library/Application Support/CrossOver/Bottles")
            .join(name),
        None => resolved_prefix.clone(),
    };

    let mut cmd = if let Some(ref bname) = bottle {
        let mut c = Command::new(&wine_wrapper);
        c.args(["--bottle", bname])
            .args(bottle_options(backend.as_deref()))
            .arg(&abs_exe);
        c
    } else {
        let _ = fs::create_dir_all(&resolved_prefix);
        let mut c = Command::new(&wineloader);
        c.arg(&abs_exe);
        c
    };

    if !inherit_env {
        cmd.env_clear();
        for (k, v) in collect_isolated_host_env(|k| env::var(k).ok()) {
            cmd.env(k, v);
        }
    }
    // Inherited values must not point the launcher at a different bottle than cleanup does.
    if bottle.is_some() {
        cmd.env_remove("WINEPREFIX").env_remove("CX_BOTTLE");
    }

    if bottle.is_none() {
        cmd.env("WINEPREFIX", &resolved_prefix)
            .env("WINESERVER", cx_root.join("bin/wineserver"))
            .env("WINELOADER", &wineloader)
            .env(
                "WINEDLLPATH",
                format!(
                    "{}:{}",
                    cx_root.join("lib/wine").display(),
                    cx_root.join("lib64/wine").display()
                ),
            )
            .env(
                "DYLD_FALLBACK_LIBRARY_PATH",
                format!(
                    "{}:{}:/usr/lib",
                    cx_root.join("lib64").display(),
                    cx_root.join("lib").display()
                ),
            );
    }

    cmd.env("ROSETTA_ADVERTISE_AVX", "1")
        .env("WINEDLLOVERRIDES", SHIM_DLL_OVERRIDES)
        .env("WINEPATH", &wine_shims_path)
        .env("BX_PROFILE", &profile)
        .env("WINEDEBUG", "-all");
    if let Some(backend) = &backend {
        cmd.env("CX_GRAPHICS_BACKEND", backend);
    }

    let abs_log = log_file.map(|lf| {
        if lf.is_absolute() {
            lf
        } else {
            env::current_dir().unwrap_or_default().join(lf)
        }
    });
    if let Some(log) = &abs_log {
        cmd.env("BX_LOG_FILE", unix_to_wine_z_path(log));
    }
    let log_size = |log: &PathBuf| fs::metadata(log).map_or(0, |m| m.len());
    let log_size_before = abs_log.as_ref().map_or(0, log_size);

    supervise::install_signal_handlers();
    let outcome = supervise::supervise(cmd, timeout, supervise::received_signal);
    let code = match &outcome {
        Ok(outcome) => outcome.exit_code(),
        Err(e) => {
            eprintln!("failed to run CrossOver wine: {e}");
            1
        }
    };
    let mut cleanup_failed = false;
    match &outcome {
        Ok(Outcome::Exited(status)) if !status.success() => {
            eprintln!("guest exited with status: {status}");
        }
        Ok(Outcome::Stopped(stop)) => {
            match stop {
                Stop::TimedOut => eprintln!("timed out; stopping the environment"),
                Stop::Signalled(signal) => {
                    eprintln!("received signal {signal}; stopping the environment");
                }
            }
            let server_env: Vec<(String, String)> = collect_isolated_host_env(|k| env::var(k).ok())
                .into_iter()
                .collect();
            if let Err(e) = supervise::stop_server(
                &cx_root.join("bin/wineserver"),
                &environment_dir,
                &server_env,
            ) {
                eprintln!("cleanup failed, programs may still be running: {e}");
                cleanup_failed = true;
            }
        }
        _ => {}
    }

    // Printed for failed and stopped runs too, which is when it matters most.
    if let Some(log) = &abs_log
        && log_size(log) > log_size_before
        && let Ok(bytes) = fs::read(log)
    {
        // Guest text may not be valid UTF-8; a summary is still worth printing.
        print!(
            "\nTrace summary of {}:\n{}",
            log.display(),
            render_trace(&String::from_utf8_lossy(&bytes))
        );
    }
    // The reason for the stop is kept; a failed cleanup only turns success into failure.
    ExitCode::from(if cleanup_failed { code.max(1) } else { code })
}

fn cmd_shader(path: &str) -> ExitCode {
    let data = match fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    match inspect_dxbc(&data) {
        Ok(info) => {
            println!("Kind             : {:?}", info.kind);
            println!(
                "Stage / SM       : {:?} ({}.{})",
                info.stage, info.shader_model_major, info.shader_model_minor
            );
            println!("Container size   : {} bytes", info.container_size);
            println!("DXIL chunk       : {} bytes", info.dxil_size);
            println!(
                "ACCC chunk       : {} bytes (FSPX={})",
                info.accc_size, info.accc_is_fspx
            );
            println!("Root signature   : {}", info.has_root_signature);
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("error: {e}");
            ExitCode::FAILURE
        }
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let Some(sub) = args.first() else {
        print_usage();
        return ExitCode::SUCCESS;
    };
    match sub.as_str() {
        "doctor" => cmd_doctor(),
        "probe" if args.len() >= 2 => cmd_probe(&args[1]),
        "shader" if args.len() >= 2 => cmd_shader(&args[1]),
        "trace" if args.len() >= 2 => cmd_trace(&args[1]),
        "run" => cmd_run(&args[1..]),
        _ => {
            print_usage();
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn parses_run_arguments() {
        let run = parse_run_args(&args(&[
            "--exe",
            "a.exe",
            "--bottle",
            "b",
            "--timeout",
            "30",
            "--backend",
            "d3dmetal",
        ]))
        .unwrap();
        assert_eq!(run.timeout, Some(Duration::from_secs(30)));
        assert_eq!(run.backend.as_deref(), Some("d3dmetal"));
        assert_eq!(run.shims_dir, PathBuf::from("build-shims/bin"));
        assert_eq!(run.profile, "lockhart");
        let plain = parse_run_args(&args(&["--exe", "a.exe"])).unwrap();
        assert!(plain.timeout.is_none() && plain.bottle.is_none() && plain.prefix.is_none());
        for bad in [
            &["--exe", "a.exe", "--timeout", "0"][..],
            &["--exe", "a.exe", "--timeout", "soon"],
            &["--exe", "a.exe", "--backend", "dmxt2"],
            &["--exe", "a.exe", "--bottle", "../x"],
            &["--exe", "a.exe", "--timeout"],
            &["--bottle", "b"],
        ] {
            assert!(parse_run_args(&args(bad)).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn bottle_mode_passes_overrides_and_backend_as_launcher_options() {
        assert_eq!(bottle_options(None), ["--dll", SHIM_DLL_OVERRIDES]);
        assert_eq!(
            bottle_options(Some("dxvk"))[2..],
            ["--env", "CX_GRAPHICS_BACKEND=dxvk"]
        );
    }

    #[test]
    fn isolated_env_strips_secrets_and_pins_path() {
        let host = BTreeMap::from([
            ("HOME", "/Users/tester"),
            ("USER", "tester"),
            ("PATH", "/custom/dev/bin:/usr/bin"),
            ("GH_TOKEN", "secret_token"),
            ("AWS_SECRET_ACCESS_KEY", "secret_aws"),
            ("CX_GRAPHICS_BACKEND", "wined3d"),
        ]);
        let child = collect_isolated_host_env(|k| host.get(k).map(|s| (*s).to_string()));
        assert_eq!(child.get("PATH").map(String::as_str), Some(SAFE_CHILD_PATH));
        assert_eq!(child.get("HOME").map(String::as_str), Some("/Users/tester"));
        assert_eq!(child.get("USER").map(String::as_str), Some("tester"));
        assert!(!child.contains_key("GH_TOKEN"));
        assert!(!child.contains_key("AWS_SECRET_ACCESS_KEY"));
        assert!(!child.contains_key("CX_GRAPHICS_BACKEND"));
    }

    #[test]
    fn converts_unix_path_to_wine_z_drive() {
        assert_eq!(
            unix_to_wine_z_path(Path::new("/tmp/bx-test.jsonl")),
            "Z:\\tmp\\bx-test.jsonl"
        );
    }

    #[test]
    fn formats_clwb_summary_lines() {
        assert_eq!(format_clwb_line(0, None), "clwb VEH traps   : 0");
        assert_eq!(format_clwb_line(2, None), "clwb VEH samples : 2");
        assert_eq!(
            format_clwb_line(3, Some(4)),
            "clwb VEH traps   : at least 4 (sampled)"
        );
    }
}
