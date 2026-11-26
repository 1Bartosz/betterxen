//! Running the x86_64 probe under Rosetta 2 with an isolated environment.

use std::io::{self, Read};
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// Variables passed through to the probe; everything else is cleared so host
/// credentials and stray settings never reach D3DMetal.
pub const ENV_ALLOWLIST: [&str; 3] = ["HOME", "TMPDIR", "DMN_LOG"];
/// `PATH` given to the probe.
const PROBE_PATH: &str = "/usr/bin:/bin";
const POLL_INTERVAL: Duration = Duration::from_millis(10);

/// What a probe process produced.
#[derive(Debug)]
pub struct ProbeRun {
    /// `None` when the probe was killed after the timeout.
    pub status: Option<ExitStatus>,
    pub stdout: String,
    pub stderr: String,
}

fn drain(mut pipe: impl Read + Send + 'static) -> JoinHandle<io::Result<String>> {
    thread::spawn(move || {
        let mut bytes = Vec::new();
        pipe.read_to_end(&mut bytes)?;
        Ok(String::from_utf8_lossy(&bytes).into_owned())
    })
}

fn join(handle: JoinHandle<io::Result<String>>) -> io::Result<String> {
    handle
        .join()
        .map_err(|_| io::Error::other("output reader thread panicked"))?
}

fn wait_with_timeout(child: &mut Child, timeout: Duration) -> io::Result<Option<ExitStatus>> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if Instant::now() >= deadline {
            child.kill()?;
            child.wait()?;
            return Ok(None);
        }
        thread::sleep(POLL_INTERVAL);
    }
}

/// Runs `probe --framework <framework_binary>` and collects its output.
pub fn run_probe(probe: &Path, framework_binary: &Path, timeout: Duration) -> io::Result<ProbeRun> {
    let mut command = Command::new(probe);
    command
        .arg("--framework")
        .arg(framework_binary)
        .env_clear()
        .env("PATH", PROBE_PATH)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    for key in ENV_ALLOWLIST {
        if let Some(value) = std::env::var_os(key) {
            command.env(key, value);
        }
    }
    let mut child = command.spawn()?;
    let stdout = drain(child.stdout.take().expect("stdout is piped"));
    let stderr = drain(child.stderr.take().expect("stderr is piped"));
    let status = wait_with_timeout(&mut child, timeout)?;
    Ok(ProbeRun {
        status,
        stdout: join(stdout)?,
        stderr: join(stderr)?,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;

    /// A throwaway executable shell script standing in for the probe.
    struct Script(PathBuf);

    impl Script {
        fn new(name: &str, body: &str) -> Self {
            let path =
                std::env::temp_dir().join(format!("bx-harness-{name}-{}.sh", std::process::id()));
            std::fs::write(&path, format!("#!/bin/sh\n{body}\n")).unwrap();
            std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
            Self(path)
        }
    }

    impl Drop for Script {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }

    #[test]
    fn passes_the_framework_and_only_allowlisted_environment() {
        let script = Script::new("env", "echo \"args=$*\"; env; echo oops >&2; exit 3");
        let run = run_probe(
            &script.0,
            Path::new("/fw/D3DMetal"),
            Duration::from_secs(10),
        )
        .unwrap();
        assert_eq!(run.status.and_then(|s| s.code()), Some(3));
        assert!(run.stdout.contains("args=--framework /fw/D3DMetal"));
        assert_eq!(run.stderr, "oops\n");
        // /bin/sh itself adds PWD, SHLVL and `_`.
        let allowed = ["PATH", "PWD", "SHLVL", "_"];
        for line in run.stdout.lines().skip(1) {
            let key = line.split_once('=').map_or(line, |(k, _)| k);
            assert!(
                allowed.contains(&key) || ENV_ALLOWLIST.contains(&key),
                "unexpected variable {key} reached the probe"
            );
        }
        assert!(run.stdout.contains(&format!("PATH={PROBE_PATH}")));
    }

    #[test]
    fn kills_a_probe_that_exceeds_the_timeout() {
        let script = Script::new("hang", "echo started; exec sleep 30");
        let started = Instant::now();
        let run = run_probe(&script.0, Path::new("/fw"), Duration::from_millis(300)).unwrap();
        assert!(run.status.is_none());
        assert_eq!(run.stdout, "started\n");
        assert!(started.elapsed() < Duration::from_secs(10));
    }
}
