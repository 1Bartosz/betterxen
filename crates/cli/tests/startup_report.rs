// SPDX-License-Identifier: GPL-3.0-or-later

use std::process::Command;

#[test]
fn trusted_fixture_reports_verified_progress_and_step_failure() {
    let folder = std::env::temp_dir().join(format!("betterxen-report-test-{}", std::process::id()));
    std::fs::create_dir(&folder).unwrap();
    let fixture = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../fixtures/pe/trusted-runner.exe"
    );
    for (steps, outcome, last, success) in [
        ("100", "verified", "framebuffer", true),
        ("1", "stage_failure", "entry", false),
    ] {
        let path = folder.join(steps);
        let output = Command::new(env!("CARGO_BIN_EXE_betterxen"))
            .args([
                "run",
                "--experimental",
                "--pe",
                fixture,
                "--max-steps",
                steps,
                "--startup-report",
            ])
            .arg(&path)
            .output()
            .unwrap();
        let report = std::fs::read_to_string(&path);
        assert_eq!(output.status.success(), success, "{output:?}, {report:?}");
        let report = report.unwrap();
        assert!(
            report.contains(&format!(r#""outcome":"{outcome}""#)),
            "{report}"
        );
        assert!(
            report.contains(&format!(r#""last_completed_stage":"{last}""#)),
            "{report}"
        );
        assert!(report.contains(r#""schema_version":1"#), "{report}");
        let mut remaining = report.as_str();
        for stage in [
            "parsed",
            "mapped",
            "relocated",
            "imports_bound",
            "tls_completed",
            "entry",
        ] {
            remaining = remaining
                .split_once(&format!(r#""stage":"{stage}""#))
                .unwrap()
                .1;
        }
        if success {
            assert!(remaining.contains(r#""stage":"exit""#));
            assert!(remaining.contains(r#""stage":"framebuffer""#));
        } else {
            assert!(report.contains("step budget exhausted"), "{report}");
        }
    }
    std::fs::remove_dir_all(folder).unwrap();
}
