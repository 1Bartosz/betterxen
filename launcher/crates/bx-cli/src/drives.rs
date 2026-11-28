//! Xbox title drive letters, linked in the prefix's `dosdevices` before launch.
//!
//! An Xbox title reads its package from `G:` and writes temporary files such as crash logs to
//! `T:` (observed at title startup; inferred to be the package root and temporary storage). A
//! Wine prefix has neither drive, so the title's first `G:\...` open fails with
//! `STATUS_OBJECT_PATH_NOT_FOUND`. `bx-cli run` links `g:` to the executable's directory and
//! `t:` to `drive_c/bx_temp`.
//!
//! As with system32 staging (#474), a drive letter the prefix already uses is not ours. A
//! `.bx-drives` manifest in `dosdevices` records each link this tool made, and only a link that
//! is listed and still points where it was pointed is replaced. Any other mapping to a
//! different target is reported and left unchanged.

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

const MANIFEST: &str = ".bx-drives";

/// The prefix-relative target of `t:`, in the form Wine itself uses for `c:`.
pub const TEMP_TARGET: &str = "../drive_c/bx_temp";

#[derive(Debug)]
pub enum Mapping {
    Created(char, PathBuf),
    /// A link this tool made earlier, pointed at a new target.
    Retargeted(char, PathBuf),
    Unchanged(char, PathBuf),
    /// A mapping this tool did not make, to a different target; left as it is.
    Foreign {
        letter: char,
        existing: String,
        wanted: PathBuf,
    },
}

impl Mapping {
    pub fn describe(&self) -> String {
        match self {
            Mapping::Created(l, t) => {
                format!("mapped {}: to {}", l.to_ascii_uppercase(), t.display())
            }
            Mapping::Retargeted(l, t) => {
                format!(
                    "remapped own {}: to {}",
                    l.to_ascii_uppercase(),
                    t.display()
                )
            }
            Mapping::Unchanged(l, t) => {
                format!(
                    "{}: already maps to {}",
                    l.to_ascii_uppercase(),
                    t.display()
                )
            }
            Mapping::Foreign {
                letter,
                existing,
                wanted,
            } => format!(
                "warning: {}: already maps to {existing}, not {}; left unchanged, so the guest \
                 may not find its files there",
                letter.to_ascii_uppercase(),
                wanted.display()
            ),
        }
    }
}

fn read_manifest(dosdevices: &Path) -> Result<BTreeMap<char, PathBuf>, String> {
    let path = dosdevices.join(MANIFEST);
    let text = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
        Err(e) => return Err(format!("cannot read {}: {e}", path.display())),
    };
    let mut owned = BTreeMap::new();
    for line in text.lines().filter(|l| !l.is_empty()) {
        let mut chars = line.chars();
        match (chars.next(), chars.next()) {
            (Some(letter), Some('\t')) if letter.is_ascii_lowercase() => {
                owned.insert(letter, PathBuf::from(chars.as_str()));
            }
            _ => return Err(format!("malformed line in {}: {line:?}", path.display())),
        }
    }
    Ok(owned)
}

fn write_manifest(dosdevices: &Path, owned: &BTreeMap<char, PathBuf>) -> Result<(), String> {
    let path = dosdevices.join(MANIFEST);
    let mut text = String::new();
    for (letter, target) in owned {
        text.push_str(&format!("{letter}\t{}\n", target.display()));
    }
    fs::write(&path, text).map_err(|e| format!("cannot write {}: {e}", path.display()))
}

/// Points `link` at `target`, replacing what is there through a rename so the link never
/// disappears in between.
fn relink(dosdevices: &Path, link: &Path, target: &Path) -> io::Result<()> {
    let tmp = dosdevices.join(format!(".bx-drive-tmp.{}", std::process::id()));
    let _ = fs::remove_file(&tmp);
    symlink(target, &tmp)?;
    let result = fs::rename(&tmp, link);
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result
}

/// Links `g:` to `package_dir` and `t:` to [`TEMP_TARGET`] in `prefix`'s `dosdevices`.
pub fn map_xbox_drives(prefix: &Path, package_dir: &Path) -> Result<Vec<Mapping>, String> {
    let dosdevices = prefix.join("dosdevices");
    if !dosdevices.is_dir() {
        return Err(format!(
            "{} does not exist (start the prefix once so Wine creates it)",
            dosdevices.display()
        ));
    }
    let temp_dir = prefix.join("drive_c/bx_temp");
    fs::create_dir_all(&temp_dir)
        .map_err(|e| format!("cannot create {}: {e}", temp_dir.display()))?;

    let mut owned = read_manifest(&dosdevices)?;
    let mut mappings = Vec::new();
    for (letter, target) in [
        ('g', package_dir.to_path_buf()),
        ('t', PathBuf::from(TEMP_TARGET)),
    ] {
        let link = dosdevices.join(format!("{letter}:"));
        let current = match fs::symlink_metadata(&link) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => None,
            Err(e) => return Err(format!("cannot inspect {}: {e}", link.display())),
            Ok(meta) if meta.file_type().is_symlink() => Some(
                fs::read_link(&link).map_err(|e| format!("cannot read {}: {e}", link.display()))?,
            ),
            Ok(_) => {
                mappings.push(Mapping::Foreign {
                    letter,
                    existing: format!("a non-link entry {}", link.display()),
                    wanted: target,
                });
                continue;
            }
        };
        let mapping = match current {
            None => {
                owned.insert(letter, target.clone());
                write_manifest(&dosdevices, &owned)?;
                relink(&dosdevices, &link, &target)
                    .map_err(|e| format!("cannot create {}: {e}", link.display()))?;
                Mapping::Created(letter, target.clone())
            }
            Some(current) if current == target => Mapping::Unchanged(letter, target.clone()),
            Some(current) if owned.get(&letter) == Some(&current) => {
                owned.insert(letter, target.clone());
                write_manifest(&dosdevices, &owned)?;
                if let Err(error) = relink(&dosdevices, &link, &target) {
                    owned.insert(letter, current);
                    let restore = write_manifest(&dosdevices, &owned);
                    let detail = restore
                        .err()
                        .map(|e| format!("; also could not restore the drive manifest: {e}"))
                        .unwrap_or_default();
                    return Err(format!(
                        "cannot replace {}: {error}{detail}",
                        link.display()
                    ));
                }
                Mapping::Retargeted(letter, target.clone())
            }
            Some(current) => {
                mappings.push(Mapping::Foreign {
                    letter,
                    existing: current.display().to_string(),
                    wanted: target,
                });
                continue;
            }
        };
        mappings.push(mapping);
    }
    Ok(mappings)
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDir(PathBuf);
    impl TempDir {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!(
                "bx-drives-{tag}-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = fs::remove_dir_all(&p);
            fs::create_dir_all(p.join("prefix/dosdevices")).unwrap();
            fs::create_dir_all(p.join("game")).unwrap();
            TempDir(p)
        }
        fn prefix(&self) -> PathBuf {
            self.0.join("prefix")
        }
        fn game(&self) -> PathBuf {
            self.0.join("game")
        }
        fn link(&self, letter: char) -> PathBuf {
            self.0.join(format!("prefix/dosdevices/{letter}:"))
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn missing_drives_are_created_and_resolve() {
        let t = TempDir::new("create");
        fs::write(t.game().join("package.bin"), b"pkg").unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(
            mappings[..],
            [Mapping::Created('g', _), Mapping::Created('t', _)]
        ));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), t.game());
        assert_eq!(
            fs::read_link(t.link('t')).unwrap(),
            PathBuf::from(TEMP_TARGET)
        );
        // Both links resolve the way Wine resolves them, from inside dosdevices.
        assert_eq!(fs::read(t.link('g').join("package.bin")).unwrap(), b"pkg");
        assert!(t.link('t').is_dir());
    }

    #[test]
    fn a_second_run_changes_nothing() {
        let t = TempDir::new("rerun");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(
            mappings[..],
            [Mapping::Unchanged('g', _), Mapping::Unchanged('t', _)]
        ));
    }

    #[test]
    fn an_own_link_follows_a_new_package_directory() {
        let t = TempDir::new("retarget");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(mappings[0], Mapping::Retargeted('g', _)));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), other);
    }

    #[test]
    fn a_foreign_mapping_is_reported_and_kept() {
        let t = TempDir::new("foreign");
        let elsewhere = t.0.join("elsewhere");
        symlink(&elsewhere, t.link('g')).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&mappings[0], Mapping::Foreign { letter: 'g', .. }));
        assert!(mappings[0].describe().starts_with("warning: G:"));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), elsewhere);
        // A link this tool did not record is never treated as its own on a later run.
        let again = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&again[0], Mapping::Foreign { letter: 'g', .. }));
    }

    #[test]
    fn an_own_link_changed_by_someone_else_becomes_foreign() {
        let t = TempDir::new("changed");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        fs::remove_file(t.link('g')).unwrap();
        let elsewhere = t.0.join("elsewhere");
        symlink(&elsewhere, t.link('g')).unwrap();
        let other = t.0.join("other-game");
        let mappings = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(&mappings[0], Mapping::Foreign { letter: 'g', .. }));
        assert_eq!(fs::read_link(t.link('g')).unwrap(), elsewhere);
    }

    #[test]
    fn a_non_link_entry_is_never_touched() {
        let t = TempDir::new("nonlink");
        fs::create_dir_all(t.link('t')).unwrap();
        let mappings = map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        assert!(matches!(&mappings[1], Mapping::Foreign { letter: 't', .. }));
        assert!(t.link('t').is_dir() && !fs::symlink_metadata(t.link('t')).unwrap().is_symlink());
    }

    #[test]
    fn a_prefix_without_dosdevices_is_an_error() {
        let t = TempDir::new("nodos");
        fs::remove_dir_all(t.prefix().join("dosdevices")).unwrap();
        let e = map_xbox_drives(&t.prefix(), &t.game()).unwrap_err();
        assert!(e.contains("dosdevices"), "{e}");
    }

    #[test]
    fn a_malformed_manifest_is_an_error_not_an_ownership_claim() {
        let t = TempDir::new("badmanifest");
        fs::write(t.prefix().join("dosdevices").join(MANIFEST), "g /x\n").unwrap();
        assert!(map_xbox_drives(&t.prefix(), &t.game()).is_err());
    }

    #[test]
    fn a_failed_manifest_write_leaves_no_unrecorded_link() {
        let t = TempDir::new("orphan");
        let manifest = t.prefix().join("dosdevices").join(MANIFEST);
        symlink(t.0.join("missing-dir/x"), &manifest).unwrap();
        assert!(map_xbox_drives(&t.prefix(), &t.game()).is_err());
        assert!(
            fs::symlink_metadata(t.link('g')).is_err(),
            "g: must not be created before ownership is recorded"
        );

        fs::remove_file(&manifest).unwrap();
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();
        let again = map_xbox_drives(&t.prefix(), &other).unwrap();
        assert!(matches!(&again[0], Mapping::Retargeted('g', _)));
    }

    #[test]
    fn a_failed_retarget_restores_the_previous_manifest_and_link() {
        let t = TempDir::new("retarget-failure");
        map_xbox_drives(&t.prefix(), &t.game()).unwrap();
        let other = t.0.join("other-game");
        fs::create_dir_all(&other).unwrap();

        let temp_link = t
            .prefix()
            .join("dosdevices")
            .join(format!(".bx-drive-tmp.{}", std::process::id()));
        fs::create_dir(&temp_link).unwrap();
        assert!(map_xbox_drives(&t.prefix(), &other).is_err());
        assert_eq!(fs::read_link(t.link('g')).unwrap(), t.game());
        assert_eq!(
            read_manifest(&t.prefix().join("dosdevices")).unwrap()[&'g'],
            t.game()
        );
    }
}
