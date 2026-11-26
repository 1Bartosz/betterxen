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
//!
//! #474: a bottle's `system32` is not ours. Staging records what it wrote in a `.bx-staged`
//! manifest there (one SHA-256 per file) and afterwards replaces only files that are listed and
//! still have the recorded hash. A file of the same name that the bottle already had, or that
//! was changed since, is never modified: staging stops with an error that names it.

use std::collections::{BTreeMap, BTreeSet};
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

/// SHA-256 (FIPS 180-4), used only to recognize files this tool staged earlier.
pub fn sha256_hex(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
        0x5be0cd19,
    ];
    let mut message = data.to_vec();
    message.push(0x80);
    while message.len() % 64 != 56 {
        message.push(0);
    }
    message.extend_from_slice(&((data.len() as u64).wrapping_mul(8)).to_be_bytes());
    for block in message.chunks_exact(64) {
        let mut w = [0u32; 64];
        for (i, word) in block.chunks_exact(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = hh
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (state, value) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
            *state = state.wrapping_add(value);
        }
    }
    h.iter().map(|word| format!("{word:08x}")).collect()
}

/// Name of the ownership record kept in `system32`.
pub const MANIFEST_NAME: &str = ".bx-staged";

/// The files this tool staged into a `system32`, as lowercase name -> SHA-256 of what it wrote.
///
/// A file is BetterXen's to refresh only while it is listed here and still has the recorded
/// hash. Anything else in `system32` belongs to the bottle and is never modified (#474).
fn read_manifest(system32: &Path) -> Result<BTreeMap<String, String>, String> {
    let path = system32.join(MANIFEST_NAME);
    let text = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    let mut owned = BTreeMap::new();
    for line in text.lines() {
        // "<64 hex digits>  <name>", as `shasum -a 256` prints. Other lines are ignored, which
        // only ever makes a file look foreign.
        if let Some((hash, name)) = line.split_once("  ")
            && hash.len() == 64
            && hash.bytes().all(|b| b.is_ascii_hexdigit())
            && !name.is_empty()
        {
            owned.insert(name.to_ascii_lowercase(), hash.to_ascii_lowercase());
        }
    }
    Ok(owned)
}

fn write_manifest(system32: &Path, owned: &BTreeMap<String, String>) -> Result<(), String> {
    let mut text = String::from(
        "# Files staged by bx-cli (sha256, two spaces, name). bx-cli replaces only these.\n",
    );
    for (name, hash) in owned {
        text.push_str(&format!("{hash}  {name}\n"));
    }
    let path = system32.join(MANIFEST_NAME);
    let tmp = system32.join(format!("{MANIFEST_NAME}.{}.bx-staging", std::process::id()));
    fs::write(&tmp, text).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot write {}: {e}", tmp.display())
    })?;
    fs::rename(&tmp, &path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {e}", path.display())
    })
}

/// What staging did to one file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Staged {
    Copied(PathBuf),
    /// An earlier copy staged by this tool, refreshed because the shim changed.
    Replaced(PathBuf),
    Unchanged(PathBuf),
    /// A [`SYSTEM32_SHIMS`] file the shim folder does not have.
    Absent(PathBuf),
}

impl Staged {
    pub fn describe(&self) -> String {
        match self {
            Staged::Copied(p) => format!("staged {}", p.display()),
            Staged::Replaced(p) => format!("refreshed own earlier copy {}", p.display()),
            Staged::Unchanged(p) => format!("already current {}", p.display()),
            Staged::Absent(p) => format!("not staged, {} not found", p.display()),
        }
    }
}

/// True if the shim folder has any [`SYSTEM32_SHIMS`] file, so there is something to stage.
pub fn has_system32_shims(shims_dir: &Path) -> bool {
    SYSTEM32_SHIMS
        .iter()
        .any(|name| find_shim(shims_dir, name).is_some())
}

/// Copies every [`SYSTEM32_SHIMS`] file, and the shims they import, from `shims_dir` into
/// `system32`. The directory must already exist (an uninitialized prefix is a caller error, not
/// something to skip). A shim the folder does not have is reported as [`Staged::Absent`], so a
/// run with a partial `--shims` folder still starts. A copy already identical byte for byte is
/// left alone. A different one is replaced only if this tool staged it and it is unchanged since
/// (see [`MANIFEST_NAME`]); any other existing file is an error and nothing is written, so a
/// DLL the bottle already had is never modified or deleted. Nothing outside `system32` is
/// written.
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
    let (roots, absent): (Vec<&str>, Vec<&str>) = SYSTEM32_SHIMS
        .iter()
        .copied()
        .partition(|name| find_shim(shims_dir, name).is_some());
    let mut staged: Vec<Staged> = absent
        .iter()
        .map(|name| Staged::Absent(shims_dir.join(name)))
        .collect();
    if roots.is_empty() {
        return Ok(staged);
    }
    if !system32.is_dir() {
        return Err(format!(
            "{} does not exist; the prefix is not initialized",
            system32.display()
        ));
    }
    let names = shim_closure(
        &roots,
        |dep| find_shim(shims_dir, dep).is_some(),
        |name| match find_shim(shims_dir, name) {
            Some(path) => imports_of(&path),
            None => Err(format!(
                "cannot read shim {}: not found",
                shims_dir.join(name).display()
            )),
        },
    )?;
    let mut owned = read_manifest(system32)?;
    // Decide everything before writing anything, so a foreign file stops the run with
    // `system32` exactly as it was.
    let mut plan = Vec::new();
    for name in &names {
        let src = find_shim(shims_dir, name).unwrap_or_else(|| shims_dir.join(name));
        let bytes =
            fs::read(&src).map_err(|e| format!("cannot read shim {}: {e}", src.display()))?;
        let dst = system32.join(name);
        let result = match fs::read(&dst) {
            Ok(existing) if existing == bytes => Staged::Unchanged(dst.clone()),
            Ok(existing) if owned.get(name) == Some(&sha256_hex(&existing)) => {
                Staged::Replaced(dst.clone())
            }
            Ok(_) => {
                return Err(format!(
                    "{} already exists and was not staged by bx-cli (or was changed since); \
                     it is left untouched. Use a bottle or prefix without its own {name}, or \
                     move that file aside yourself, then run again",
                    dst.display()
                ));
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => Staged::Copied(dst.clone()),
            Err(e) => return Err(format!("cannot read {}: {e}", dst.display())),
        };
        plan.push((name, bytes, dst, result));
    }
    let mut manifest_changed = false;
    for (name, bytes, dst, result) in plan {
        if !matches!(result, Staged::Unchanged(_)) {
            // Write beside the target, then rename, so a guest never sees a half-written DLL.
            // The name is per process, so a concurrent run cannot rename a partial write.
            let tmp = system32.join(format!(".{name}.{}.bx-staging", std::process::id()));
            fs::write(&tmp, &bytes).map_err(|e| {
                let _ = fs::remove_file(&tmp);
                format!("cannot write {}: {e}", tmp.display())
            })?;
            fs::rename(&tmp, &dst).map_err(|e| {
                let _ = fs::remove_file(&tmp);
                format!("cannot replace {}: {e}", dst.display())
            })?;
            // Recorded only after the file is in place: a crash in between leaves a copy that
            // looks foreign later, which is the safe direction.
            owned.insert(name.clone(), sha256_hex(&bytes));
            manifest_changed = true;
        }
        staged.push(result);
    }
    if manifest_changed {
        write_manifest(system32, &owned)?;
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
    fn sha256_matches_the_published_test_vectors() {
        // FIPS 180-4 / NIST example values.
        assert_eq!(
            sha256_hex(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(
            sha256_hex(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        // Padding boundaries (one block, two blocks, exactly one block of data); expected
        // values computed independently with Python's hashlib.
        for (len, expected) in [
            (
                55usize,
                "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
            ),
            (
                56usize,
                "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
            ),
            (
                64usize,
                "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
            ),
            (
                1000usize,
                "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
            ),
        ] {
            assert_eq!(sha256_hex(&vec![b'a'; len]), expected, "{len} bytes");
        }
    }

    /// Every regular file in `dir` with its bytes, for before/after comparison.
    fn snapshot(dir: &Path) -> BTreeMap<String, Vec<u8>> {
        fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap())
            .map(|e| {
                (
                    e.file_name().to_string_lossy().into_owned(),
                    fs::read(e.path()).unwrap(),
                )
            })
            .collect()
    }

    #[test]
    fn a_foreign_file_with_different_bytes_is_never_modified() {
        let (_t, shims, prefix) = setup("foreign");
        let sys = system32_dir(&prefix);
        // The bottle's own runtime, put there by something other than bx-cli.
        fs::write(sys.join("xgameruntime.dll"), b"the bottle's own runtime").unwrap();
        let before = snapshot(&sys);
        let err = stage(&shims, &sys).unwrap_err();
        assert!(err.contains("xgameruntime.dll"), "{err}");
        assert!(err.contains("not staged by bx-cli"), "{err}");
        // Nothing changed: not the foreign file, no dependency copied beside it, no manifest.
        assert_eq!(snapshot(&sys), before);
        // Still refused on the next run.
        assert!(stage(&shims, &sys).is_err());
        assert_eq!(snapshot(&sys), before);
    }

    #[test]
    fn a_foreign_dependency_blocks_staging_before_anything_is_written() {
        let (_t, shims, prefix) = setup("foreign-dep");
        let sys = system32_dir(&prefix);
        fs::write(sys.join("bx_runtime_veh.dll"), b"someone else's file").unwrap();
        let before = snapshot(&sys);
        let err = stage(&shims, &sys).unwrap_err();
        assert!(err.contains("bx_runtime_veh.dll"), "{err}");
        assert_eq!(
            snapshot(&sys),
            before,
            "xgameruntime.dll must not be staged either"
        );
    }

    #[test]
    fn an_owned_copy_changed_by_someone_else_is_not_replaced() {
        let (_t, shims, prefix) = setup("tampered");
        let sys = system32_dir(&prefix);
        stage(&shims, &sys).unwrap();
        // Something else (an installer) replaced our copy after we staged it.
        fs::write(sys.join("xgameruntime.dll"), b"installed over ours").unwrap();
        fs::write(shims.join("xgameruntime.dll"), b"shim v2").unwrap();
        let before = snapshot(&sys);
        assert!(stage(&shims, &sys).is_err());
        assert_eq!(snapshot(&sys), before);
    }

    #[test]
    fn an_identical_foreign_file_is_left_alone_and_not_claimed() {
        let (_t, shims, prefix) = setup("identical");
        let sys = system32_dir(&prefix);
        fs::write(sys.join("xgameruntime.dll"), b"shim v1").unwrap();
        let staged = stage(&shims, &sys).unwrap();
        assert!(staged.contains(&Staged::Unchanged(sys.join("xgameruntime.dll"))));
        assert!(
            !read_manifest(&sys)
                .unwrap()
                .contains_key("xgameruntime.dll")
        );
        // Not ours, so a later shim change must not replace it.
        fs::write(shims.join("xgameruntime.dll"), b"shim v2").unwrap();
        assert!(stage(&shims, &sys).is_err());
        assert_eq!(fs::read(sys.join("xgameruntime.dll")).unwrap(), b"shim v1");
    }

    #[test]
    fn refreshes_a_stale_copy_it_staged_itself() {
        let (_t, shims, prefix) = setup("replace");
        let sys = system32_dir(&prefix);
        let first = stage(&shims, &sys).unwrap();
        assert!(first.iter().all(|s| matches!(s, Staged::Copied(_))));
        let manifest = fs::read_to_string(sys.join(MANIFEST_NAME)).unwrap();
        assert!(
            manifest.contains(&format!("{}  xgameruntime.dll\n", sha256_hex(b"shim v1"))),
            "{manifest}"
        );
        // The shim is rebuilt; the copy in system32 is now stale but still ours.
        fs::write(shims.join("xgameruntime.dll"), b"shim v2").unwrap();
        let staged = stage(&shims, &sys).unwrap();
        assert!(staged.contains(&Staged::Replaced(sys.join("xgameruntime.dll"))));
        assert!(staged.contains(&Staged::Unchanged(sys.join("bx_runtime_veh.dll"))));
        assert_eq!(fs::read(sys.join("xgameruntime.dll")).unwrap(), b"shim v2");
        let owned = read_manifest(&sys).unwrap();
        assert_eq!(owned.get("xgameruntime.dll"), Some(&sha256_hex(b"shim v2")));
        assert_eq!(
            owned.get("bx_runtime_veh.dll"),
            Some(&sha256_hex(b"veh v1"))
        );
        let leftovers: Vec<_> = fs::read_dir(&sys)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().contains("bx-staging"))
            .collect();
        assert!(leftovers.is_empty(), "temporary file left behind");
    }

    #[test]
    fn a_damaged_manifest_only_makes_files_look_foreign() {
        let (_t, shims, prefix) = setup("manifest");
        let sys = system32_dir(&prefix);
        stage(&shims, &sys).unwrap();
        fs::write(
            sys.join(MANIFEST_NAME),
            "not a manifest\nabc  xgameruntime.dll\n",
        )
        .unwrap();
        assert!(read_manifest(&sys).unwrap().is_empty());
        fs::write(shims.join("xgameruntime.dll"), b"shim v2").unwrap();
        assert!(stage(&shims, &sys).is_err());
        assert_eq!(fs::read(sys.join("xgameruntime.dll")).unwrap(), b"shim v1");
    }

    #[test]
    fn missing_shim_is_reported_and_nothing_is_staged() {
        let (t, shims, prefix) = setup("missing");
        fs::remove_file(shims.join("xgameruntime.dll")).unwrap();
        assert!(!has_system32_shims(&shims));
        let sys = system32_dir(&prefix);
        let expected = vec![Staged::Absent(shims.join("xgameruntime.dll"))];
        assert_eq!(stage(&shims, &sys).unwrap(), expected);
        assert_eq!(fs::read_dir(&sys).unwrap().count(), 0);
        // With nothing to stage, a prefix without system32 is not an error either.
        let fresh = system32_dir(&t.0.join("fresh"));
        assert_eq!(stage(&shims, &fresh).unwrap(), expected);
        assert!(!fresh.exists());
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
