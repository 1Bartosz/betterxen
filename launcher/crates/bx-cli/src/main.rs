//! Track A (M1-A1): Native ARM64 macOS CLI for BetterXen hybrid execution.

use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

use bx_probe::{ShimTrack, classify_dll, probe_pe32_plus};
use bx_telemetry::summarize_jsonl;
use dxbc_accc::inspect_dxbc;

const SHIM_DLL_OVERRIDES: &str = "bx_runtime_veh,d3d12_xs,xg_xs,xmem,XCRDAPI,xgameruntime,AcpHal,xapu,chad,GameInput,\
api-ms-win-core-psm-appnotify-l1-1-0,api-ms-win-core-psm-appnotify-l1-1-1,\
ext-ms-win-rtcore-ntuser-message-l1-1-0,ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0,\
ext-ms-win-rtcore-ntuser-window-l1-1-0,ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0=n,b";

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
           bx-cli run --exe <path> [--shims <dir>] [--bottle <name> | --prefix <dir>] [--profile lockhart|anaconda] [--log <path>] [--inherit-env]\n"
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

fn cmd_trace(path: &str) -> ExitCode {
    let text = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("error reading {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let summary = summarize_jsonl(&text);
    println!("Trace events     : {}", summary.total_lines);
    if let Some(lower_bound) = summary.clwb_trap_lower_bound {
        println!("clwb VEH traps   : at least {lower_bound} (sampled)");
    } else {
        println!("clwb VEH samples : {}", summary.clwb_events);
    }
    println!("ADX illegal ops  : {}", summary.adx_errors);
    if !summary.unhandled_exceptions.is_empty() {
        println!(
            "\nUnhandled exceptions ({}):",
            summary.unhandled_exceptions.len()
        );
        for crash in summary.unhandled_exceptions {
            println!(
                "  code={} module_base={} + RVA={} RIP={}",
                crash.exception_code.as_deref().unwrap_or("unknown"),
                crash.module_base.as_deref().unwrap_or("unknown"),
                crash.rva.as_deref().unwrap_or("unknown"),
                crash.rip.as_deref().unwrap_or("unknown"),
            );
        }
    }
    if !summary.unimpl_apis.is_empty() {
        println!("\nUnimplemented APIs:");
        for (api, count) in summary.unimpl_apis {
            println!("  {count:5}x  {api}");
        }
    }
    if !summary.unimpl_vtables.is_empty() {
        println!("\nUnimplemented COM vtable slots:");
        for (slot, count) in summary.unimpl_vtables {
            println!("  {count:5}x  {slot}");
        }
    }
    ExitCode::SUCCESS
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

fn cmd_run(args: &[String]) -> ExitCode {
    let mut exe_path: Option<PathBuf> = None;
    let mut shims_dir = PathBuf::from("build-shims/bin");
    let mut profile = String::from("lockhart");
    let mut bottle: Option<String> = None;
    let mut wine_prefix: Option<PathBuf> = None;
    let mut log_file: Option<PathBuf> = None;
    let mut inherit_env = false;

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
            "--inherit-env" => {
                inherit_env = true;
                i += 1;
            }
            other => {
                eprintln!("unknown argument: {other}");
                return ExitCode::FAILURE;
            }
        }
    }

    let Some(exe) = exe_path else {
        eprintln!("error: --exe <path> is required");
        return ExitCode::FAILURE;
    };

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

    let mut cmd = if let Some(ref bname) = bottle {
        let mut c = Command::new(&wine_wrapper);
        c.args(["--bottle", bname]).arg(&abs_exe);
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

    if let Some(ref lf) = log_file {
        let abs_log = if lf.is_absolute() {
            lf.clone()
        } else {
            env::current_dir().unwrap_or_default().join(lf)
        };
        cmd.env("BX_LOG_FILE", unix_to_wine_z_path(&abs_log));
    }

    match cmd.status() {
        Ok(st) if st.success() => ExitCode::SUCCESS,
        Ok(st) => {
            eprintln!("guest exited with status: {st}");
            ExitCode::FAILURE
        }
        Err(e) => {
            eprintln!("failed to launch CrossOver wine: {e}");
            ExitCode::FAILURE
        }
    }
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
}
