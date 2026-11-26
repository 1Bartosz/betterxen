//! #457: shims a guest loads by system-directory search, staged into the prefix's `system32`.
//!
//! The GDK bootstrap that titles link statically loads `xgameruntime.dll` with
//! `LoadLibraryExW(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)`. That search never consults
//! `WINEPATH`, where `bx-cli run` otherwise exposes the shims, so those shims are copied into
//! `drive_c/windows/system32` of the prefix or bottle before launch.
//!
//! The same search flags apply to the DLL's own imports, so every BetterXen shim it imports
//! (for example `bx_runtime_veh.dll`) is staged too. Otherwise the load fails unless the guest
//! happened to load that dependency earlier.

use std::collections::BTreeSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// Shims loaded with `LOAD_LIBRARY_SEARCH_SYSTEM32`.
pub const SYSTEM32_SHIMS: &[&str] = &["xgameruntime.dll"];

/// `roots` plus, transitively, every import that is itself a file in the shim folder
/// (`is_shim`). Names are compared case-insensitively, as the Windows loader does, and returned
/// lowercased in a stable order.
pub fn shim_closure<I, S>(
    roots: &[&str],
    is_shim: S,
    mut imports_of: I,
) -> Result<Vec<String>, String>
where
    I: FnMut(&str) -> Result<Vec<String>, String>,
    S: Fn(&str) -> bool,
{
    let mut done = BTreeSet::new();
    let mut todo: Vec<String> = roots.iter().map(|r| r.to_ascii_lowercase()).collect();
    while let Some(name) = todo.pop() {
        if !done.insert(name.clone()) {
            continue;
        }
        for dep in imports_of(&name)? {
            let dep = dep.to_ascii_lowercase();
            if is_shim(&dep) && !done.contains(&dep) {
                todo.push(dep);
            }
        }
    }
    Ok(done.into_iter().collect())
}

/// The shim file in `shims_dir` named `name`, matched case-insensitively.
fn find_shim(shims_dir: &Path, name: &str) -> Option<PathBuf> {
    let exact = shims_dir.join(name);
    if exact.is_file() {
        return Some(exact);
    }
    fs::read_dir(shims_dir)
        .ok()?
        .filter_map(|e| e.ok())
        .find_map(|e| {
            let file = e.file_name();
            (file.to_string_lossy().eq_ignore_ascii_case(name) && e.path().is_file())
                .then(|| e.path())
        })
}

/// DLL names a PE32+ file imports, read with `bx-probe`.
fn pe_imports(path: &Path) -> Result<Vec<String>, String> {
    let bytes = fs::read(path).map_err(|e| format!("cannot read shim {}: {e}", path.display()))?;
    let summary = bx_probe::probe_pe32_plus(&bytes)
        .map_err(|e| format!("cannot read imports of {}: {e:?}", path.display()))?;
    Ok(summary.imports.into_keys().collect())
}

/// The 64-bit system directory of a Wine prefix.
pub fn system32_dir(prefix: &Path) -> PathBuf {
    prefix.join("drive_c").join("windows").join("system32")
}

/// What staging did to one file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Staged {
    Copied(PathBuf),
    Replaced(PathBuf),
    Unchanged(PathBuf),
}

impl Staged {
    pub fn describe(&self) -> String {
        match self {
            Staged::Copied(p) => format!("staged {}", p.display()),
            Staged::Replaced(p) => format!("replaced {}", p.display()),
            Staged::Unchanged(p) => format!("already current {}", p.display()),
        }
    }
}

/// Copies every [`SYSTEM32_SHIMS`] file, and the shims they import, from `shims_dir` into
/// `system32`. The directory must already exist (an uninitialized prefix is a caller error, not
/// something to skip). A missing shim is an error. A copy already identical byte for byte is
/// left alone, a different one is replaced. Nothing outside `system32` is written.
pub fn stage_system32(shims_dir: &Path, system32: &Path) -> Result<Vec<Staged>, String> {
    stage_system32_with(shims_dir, system32, pe_imports)
}

fn stage_system32_with<I>(
    shims_dir: &Path,
    system32: &Path,
    mut imports_of: I,
) -> Result<Vec<Staged>, String>
where
    I: FnMut(&Path) -> Result<Vec<String>, String>,
{
    if !system32.is_dir() {
        return Err(format!(
            "{} does not exist; the prefix is not initialized",
            system32.display()
        ));
    }
    let names = shim_closure(
        SYSTEM32_SHIMS,
        |dep| find_shim(shims_dir, dep).is_some(),
        |name| match find_shim(shims_dir, name) {
            Some(path) => imports_of(&path),
            None => Err(format!(
                "cannot read shim {}: not found",
                shims_dir.join(name).display()
            )),
        },
    )?;
    let mut staged = Vec::new();
    for name in &names {
        let src = find_shim(shims_dir, name).unwrap_or_else(|| shims_dir.join(name));
        let bytes =
            fs::read(&src).map_err(|e| format!("cannot read shim {}: {e}", src.display()))?;
        let dst = system32.join(name);
        let result = match fs::read(&dst) {
            Ok(existing) if existing == bytes => Staged::Unchanged(dst.clone()),
            Ok(_) => Staged::Replaced(dst.clone()),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Staged::Copied(dst.clone()),
            Err(e) => return Err(format!("cannot read {}: {e}", dst.display())),
        };
        if !matches!(result, Staged::Unchanged(_)) {
            // Write beside the target, then rename, so a guest never sees a half-written DLL.
            let tmp = system32.join(format!(".{name}.bx-staging"));
            fs::write(&tmp, &bytes).map_err(|e| format!("cannot write {}: {e}", tmp.display()))?;
            fs::rename(&tmp, &dst).map_err(|e| {
                let _ = fs::remove_file(&tmp);
                format!("cannot replace {}: {e}", dst.display())
            })?;
        }
        staged.push(result);
    }
    Ok(staged)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDir(PathBuf);
    impl TempDir {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!(
                "bx-stage-{tag}-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = fs::remove_dir_all(&p);
            fs::create_dir_all(&p).unwrap();
            TempDir(p)
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    fn setup(tag: &str) -> (TempDir, PathBuf, PathBuf) {
        let t = TempDir::new(tag);
        let shims = t.0.join("shims");
        let prefix = t.0.join("prefix");
        fs::create_dir_all(&shims).unwrap();
        fs::create_dir_all(system32_dir(&prefix)).unwrap();
        for name in SYSTEM32_SHIMS {
            fs::write(shims.join(name), b"shim v1").unwrap();
        }
        fs::write(shims.join("bx_runtime_veh.dll"), b"veh v1").unwrap();
        fs::write(shims.join("unrelated.dll"), b"not imported").unwrap();
        (t, shims, prefix)
    }

    /// The import graph of the fake shims: the runtime imports the VEH logger and a system DLL.
    fn fake_imports(path: &Path) -> Result<Vec<String>, String> {
        let name = path
            .file_name()
            .unwrap()
            .to_string_lossy()
            .to_ascii_lowercase();
        Ok(match name.as_str() {
            "xgameruntime.dll" => vec!["BX_RUNTIME_VEH.dll".into(), "KERNEL32.dll".into()],
            _ => vec!["KERNEL32.dll".into()],
        })
    }

    fn stage(shims: &Path, sys: &Path) -> Result<Vec<Staged>, String> {
        stage_system32_with(shims, sys, fake_imports)
    }

    #[test]
    fn closure_follows_shim_imports_only() {
        let shims: BTreeSet<&str> = ["a.dll", "b.dll", "c.dll"].into();
        let graph = |n: &str| -> Result<Vec<String>, String> {
            Ok(match n {
                "a.dll" => vec!["B.DLL".into(), "kernel32.dll".into()],
                "b.dll" => vec!["c.dll".into(), "a.dll".into()],
                _ => vec![],
            })
        };
        let got = shim_closure(&["A.dll"], |d| shims.contains(d), graph).unwrap();
        assert_eq!(got, vec!["a.dll", "b.dll", "c.dll"]);
    }

    #[test]
    fn stages_the_runtime_and_its_shim_dependency() {
        let (_t, shims, prefix) = setup("deps");
        let sys = system32_dir(&prefix);
        let staged = stage(&shims, &sys).unwrap();
        assert_eq!(staged.len(), 2, "{staged:?}");
        assert_eq!(fs::read(sys.join("bx_runtime_veh.dll")).unwrap(), b"veh v1");
        assert!(
            !sys.join("unrelated.dll").exists(),
            "only imported shims are staged"
        );
    }

    #[test]
    fn system32_is_under_drive_c() {
        assert_eq!(
            system32_dir(Path::new("/p")),
            PathBuf::from("/p/drive_c/windows/system32")
        );
    }

    #[test]
    fn copies_then_leaves_identical_copy_alone() {
        let (_t, shims, prefix) = setup("idem");
        let sys = system32_dir(&prefix);
        let first = stage(&shims, &sys).unwrap();
        assert!(first.iter().all(|s| matches!(s, Staged::Copied(_))));
        assert_eq!(fs::read(sys.join("xgameruntime.dll")).unwrap(), b"shim v1");
        let second = stage(&shims, &sys).unwrap();
        assert!(second.iter().all(|s| matches!(s, Staged::Unchanged(_))));
    }

    #[test]
    fn replaces_an_older_copy() {
        let (_t, shims, prefix) = setup("replace");
        let sys = system32_dir(&prefix);
        fs::write(sys.join("xgameruntime.dll"), b"old").unwrap();
        let staged = stage(&shims, &sys).unwrap();
        assert!(staged.contains(&Staged::Replaced(sys.join("xgameruntime.dll"))));
        assert_eq!(fs::read(sys.join("xgameruntime.dll")).unwrap(), b"shim v1");
        let leftovers: Vec<_> = fs::read_dir(&sys)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().contains("bx-staging"))
            .collect();
        assert!(leftovers.is_empty(), "temporary file left behind");
    }

    #[test]
    fn missing_shim_is_an_error() {
        let (_t, shims, prefix) = setup("missing");
        fs::remove_file(shims.join("xgameruntime.dll")).unwrap();
        let err = stage(&shims, &system32_dir(&prefix)).unwrap_err();
        assert!(err.contains("cannot read shim"), "{err}");
    }

    #[test]
    fn uninitialized_prefix_is_an_error_not_a_skip() {
        let (t, shims, _prefix) = setup("uninit");
        let sys = system32_dir(&t.0.join("fresh"));
        let err = stage(&shims, &sys).unwrap_err();
        assert!(err.contains("not initialized"), "{err}");
        assert!(!sys.exists(), "staging must not create the prefix itself");
    }

    #[test]
    fn writes_only_inside_system32() {
        let (_t, shims, prefix) = setup("scope");
        let sys = system32_dir(&prefix);
        stage(&shims, &sys).unwrap();
        let windows: Vec<_> = fs::read_dir(prefix.join("drive_c/windows"))
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(windows, vec![std::ffi::OsString::from("system32")]);
    }
}
