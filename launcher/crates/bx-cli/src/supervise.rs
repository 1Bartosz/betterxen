//! Supervision for `bx-cli run` (#408): the launcher gets its own process group, and a timeout
//! or signal stops that group and then the environment's `wineserver`.
//!
//! The `wineserver` is stopped only after a timeout or signal, never after a normal exit,
//! because it ends every program in that bottle or prefix, not only this run's.

use std::io::IsTerminal;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicI32, Ordering};
use std::time::{Duration, Instant};

const POLL: Duration = Duration::from_millis(50);
/// How long the launcher group has to exit after SIGTERM before SIGKILL.
const GRACE: Duration = Duration::from_secs(5);
const SERVER_WAIT: Duration = Duration::from_secs(15);

static SIGNAL: AtomicI32 = AtomicI32::new(0);

extern "C" fn note_signal(signal: libc::c_int) {
    SIGNAL.store(signal, Ordering::SeqCst);
}

/// Records SIGINT, SIGTERM and SIGHUP instead of dying, so the run can be cleaned up.
pub fn install_signal_handlers() {
    for signal in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
        // SAFETY: the handler only stores to an atomic, which is async-signal-safe.
        unsafe {
            libc::signal(
                signal,
                note_signal as extern "C" fn(libc::c_int) as libc::sighandler_t,
            );
        }
    }
}

pub fn received_signal() -> i32 {
    SIGNAL.load(Ordering::SeqCst)
}

#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Stop {
    TimedOut,
    Signalled(i32),
}

#[derive(Debug)]
pub enum Outcome {
    Exited(ExitStatus),
    Stopped(Stop),
}

impl Outcome {
    /// 0 or 1 for the guest's own result, 124 for a timeout (as `timeout(1)`), 128 + N for a
    /// signal.
    pub fn exit_code(&self) -> u8 {
        match self {
            Outcome::Exited(status) => u8::from(!status.success()),
            Outcome::Stopped(Stop::TimedOut) => 124,
            Outcome::Stopped(Stop::Signalled(signal)) => 128u8.saturating_add(*signal as u8),
        }
    }
}

fn signal_group(child: &Child, signal: libc::c_int) {
    // SAFETY: kill takes no pointers. The child leads its own group and has not been reaped,
    // so the group id cannot have been reused.
    unsafe {
        libc::kill(-(child.id() as libc::pid_t), signal);
    }
}

fn wait_until(child: &mut Child, deadline: Instant) -> std::io::Result<Option<ExitStatus>> {
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if Instant::now() >= deadline {
            return Ok(None);
        }
        std::thread::sleep(POLL);
    }
}

/// Runs `command` in a new process group until it exits, `timeout` passes, or `signal`
/// returns non-zero. On a stop the whole group is terminated before this returns.
pub fn supervise(
    mut command: Command,
    timeout: Option<Duration>,
    signal: impl Fn() -> i32,
) -> std::io::Result<Outcome> {
    // A background process group is stopped by SIGTTIN if it reads the terminal, so a terminal
    // is replaced by end-of-file. Piped and redirected input is passed through.
    if std::io::stdin().is_terminal() {
        command.stdin(Stdio::null());
    }
    let mut child = command.process_group(0).spawn()?;
    let started = Instant::now();
    let stop = loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Outcome::Exited(status));
        }
        let received = signal();
        if received != 0 {
            break Stop::Signalled(received);
        }
        if timeout.is_some_and(|limit| started.elapsed() >= limit) {
            break Stop::TimedOut;
        }
        std::thread::sleep(POLL);
    };
    signal_group(&child, libc::SIGTERM);
    // Wait without reaping first, so SIGKILL still reaches members that outlive the leader.
    std::thread::sleep(POLL);
    let deadline = Instant::now() + GRACE;
    while Instant::now() < deadline && group_leader_running(&child) {
        std::thread::sleep(POLL);
    }
    signal_group(&child, libc::SIGKILL);
    wait_until(&mut child, Instant::now() + GRACE)?;
    Ok(Outcome::Stopped(stop))
}

/// True while the leader has not exited; does not reap it.
fn group_leader_running(child: &Child) -> bool {
    // SAFETY: `info` is a valid, writable siginfo_t and WNOWAIT leaves the child unreaped.
    unsafe {
        let mut info: libc::siginfo_t = std::mem::zeroed();
        libc::waitid(
            libc::P_PID,
            child.id() as libc::id_t,
            &mut info,
            libc::WEXITED | libc::WNOHANG | libc::WNOWAIT,
        ) == 0
            && info.si_pid() == 0
    }
}

/// `wineserver -k` then `-w` for the environment at `prefix`.
///
/// `-k` exits 1 when no server is running (observed on CrossOver 26.3), so 1 is accepted
/// there; `-w` must still succeed, which a surviving server prevents.
pub fn stop_server(
    wineserver: &Path,
    prefix: &Path,
    environment: &[(String, String)],
) -> Result<(), String> {
    for (flag, idle_code) in [("-k", Some(1)), ("-w", None)] {
        let mut child = Command::new(wineserver)
            .arg(flag)
            .env_clear()
            .envs(environment.iter().map(|(k, v)| (k, v)))
            .env("WINEPREFIX", prefix)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|e| format!("cannot start wineserver {flag}: {e}"))?;
        match wait_until(&mut child, Instant::now() + SERVER_WAIT) {
            Ok(Some(status)) if status.success() => {}
            Ok(Some(status)) if idle_code.is_some() && status.code() == idle_code => {}
            other => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("wineserver {flag} did not succeed: {other:?}"));
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn shell(script: &str) -> Command {
        let mut command = Command::new("/bin/sh");
        command.args(["-c", script]);
        command
    }

    fn scratch(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("bx-cli-{name}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn classifies_exit_timeout_and_signal() {
        let ok = supervise(shell("exit 0"), None, || 0).unwrap();
        assert_eq!(ok.exit_code(), 0);
        let failed = supervise(shell("exit 7"), Some(Duration::from_secs(10)), || 0).unwrap();
        assert_eq!(failed.exit_code(), 1);
        let slow = supervise(shell("sleep 30"), Some(Duration::from_millis(200)), || 0).unwrap();
        assert!(matches!(slow, Outcome::Stopped(Stop::TimedOut)));
        assert_eq!(slow.exit_code(), 124);
        let interrupted = supervise(shell("sleep 30"), None, || libc::SIGTERM).unwrap();
        assert_eq!(interrupted.exit_code(), 128 + libc::SIGTERM as u8);
    }

    #[test]
    fn a_timeout_stops_children_that_ignore_sigterm() {
        let dir = scratch("group");
        let pid_file = dir.join("child.pid");
        let script = format!(
            "(trap '' TERM; exec sleep 30) & echo $! > '{}'; wait",
            pid_file.display()
        );
        supervise(shell(&script), Some(Duration::from_millis(500)), || 0).unwrap();
        let pid: libc::pid_t = std::fs::read_to_string(&pid_file)
            .unwrap()
            .trim()
            .parse()
            .unwrap();
        let gone = (0..100).any(|_| {
            std::thread::sleep(POLL);
            // SAFETY: signal 0 only checks whether the process exists.
            unsafe { libc::kill(pid, 0) != 0 }
        });
        assert!(gone, "grandchild {pid} survived the timeout");
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn an_idle_server_is_accepted_only_when_the_wait_succeeds() {
        let dir = scratch("server");
        for (name, kill, wait, ok) in [
            ("running", 0, 0, true),
            ("idle", 1, 0, true),
            ("stuck", 1, 1, false),
            ("broken", 2, 0, false),
        ] {
            let server = dir.join(name);
            std::fs::write(
                &server,
                format!("#!/bin/sh\n[ \"$1\" = -k ] && exit {kill}\nexit {wait}\n"),
            )
            .unwrap();
            std::fs::set_permissions(&server, std::fs::Permissions::from_mode(0o755)).unwrap();
            assert_eq!(stop_server(&server, &dir, &[]).is_ok(), ok, "{name}");
        }
        std::fs::remove_dir_all(dir).unwrap();
    }
}
