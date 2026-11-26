//! Track A / F: Structured JSONL shim trace aggregator.
//!
//! Summarizes `bx_runtime_veh.dll` logs (stub API calls, unimplemented COM vtable
//! slots, `clwb` traps, and ADX illegal-instruction tripwires) so tracks can prioritize
//! the exact next blocking API.

use std::collections::BTreeMap;

#[derive(Debug, Default, PartialEq, Eq)]
pub struct TraceSummary {
    pub total_lines: usize,
    pub clwb_events: usize,
    pub adx_errors: usize,
    pub unimpl_apis: BTreeMap<String, usize>,
    pub unimpl_vtables: BTreeMap<String, usize>,
}

fn extract_json_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let needle = format!("\"{key}\":\"");
    let start = line.find(&needle)? + needle.len();
    let rest = &line[start..];
    let end = rest.find('"')?;
    Some(&rest[..end])
}

pub fn summarize_jsonl(input: &str) -> TraceSummary {
    let mut summary = TraceSummary::default();
    for line in input.lines() {
        let trimmed = line.trim();
        if !trimmed.starts_with('{') {
            continue;
        }
        summary.total_lines += 1;
        let event = extract_json_str(trimmed, "event").unwrap_or("");
        let detail = extract_json_str(trimmed, "detail").unwrap_or("");
        match event {
            "clwb_trapped" => summary.clwb_events += 1,
            "adx_illegal_instruction" => summary.adx_errors += 1,
            "unimpl_api" => *summary.unimpl_apis.entry(detail.to_string()).or_insert(0) += 1,
            "unimpl_vtable" => {
                *summary
                    .unimpl_vtables
                    .entry(detail.to_string())
                    .or_insert(0) += 1
            }
            _ => {}
        }
    }
    summary
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aggregates_stub_and_veh_events() {
        let sample = r#"
{"ts_ms":10,"tid":1,"level":"INFO","sub":"runtime_veh","event":"clwb_trapped","detail":"rip=0x1000 len=4 total_traps=1"}
{"ts_ms":12,"tid":1,"level":"WARN","sub":"stub","event":"unimpl_api","detail":"d3d12_xs.dll!D3D12XboxCreateDevice"}
{"ts_ms":14,"tid":1,"level":"WARN","sub":"stub","event":"unimpl_api","detail":"d3d12_xs.dll!D3D12XboxCreateDevice"}
{"ts_ms":16,"tid":1,"level":"WARN","sub":"com_vtable","event":"unimpl_vtable","detail":"ID3D12Device11[44]::CreatePlacedResourceX"}
"#;
        let s = summarize_jsonl(sample);
        assert_eq!(s.total_lines, 4);
        assert_eq!(s.clwb_events, 1);
        assert_eq!(s.adx_errors, 0);
        assert_eq!(
            s.unimpl_apis.get("d3d12_xs.dll!D3D12XboxCreateDevice"),
            Some(&2)
        );
        assert_eq!(
            s.unimpl_vtables
                .get("ID3D12Device11[44]::CreatePlacedResourceX"),
            Some(&1)
        );
    }
}
