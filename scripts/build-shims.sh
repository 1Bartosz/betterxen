#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BX_SHIM_BUILD_DIR:-$ROOT_DIR/build-shims}"
JOBS="${BX_BUILD_JOBS:-4}"

for tool in cmake x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-objdump; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: required cross-compilation tool '$tool' not found in PATH" >&2
        exit 1
    fi
done

cmake -S "$ROOT_DIR/shims" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/shims/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo "=== Verifying built PE32+ DLL Export Directories against abi/defs/*.def ==="
python3 - "$ROOT_DIR/abi/defs" "$BUILD_DIR/bin" <<'PY'
import os
import re
import subprocess
import sys

defs_dir, bin_dir = sys.argv[1], sys.argv[2]
FORBIDDEN_IMPORTS = {"libwinpthread-1.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll"}


def parse_def(path):
    """Return (dll_name, dict[name -> (expected_ordinal_or_None, expected_forwarder_or_None)])."""
    dll_name = None
    expected = {}
    in_exports = False
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.split(";")[0].strip()
            if not line:
                continue
            if line.upper().startswith("LIBRARY "):
                dll_name = line.split(None, 1)[1].strip()
            elif line.upper() == "EXPORTS":
                in_exports = True
            elif in_exports:
                # Supported forms:
                #   SymName
                #   SymName @1
                #   SymName = user32.SymName
                #   SymName = user32.SymName @2
                ord_match = re.search(r"@(\d+)\b", line)
                expected_ord = int(ord_match.group(1)) if ord_match else None
                body = re.sub(r"@\d+\b", "", line).strip()
                if "=" in body:
                    sym, fwd = [p.strip() for p in body.split("=", 1)]
                else:
                    sym, fwd = body.strip(), None
                if sym:
                    expected[sym] = (expected_ord, fwd)
    return dll_name, expected


def parse_objdump_pe(out):
    """Parse format, PE Export Directory (name/ordinal/EAT), and Import DLL names strictly."""
    is_pe32_plus = "file format pei-x86-64" in out
    import_dlls = {
        m.group(1).strip().lower()
        for m in re.finditer(r"^\s*DLL Name:\s*(\S+)", out, re.MULTILINE)
    }

    edata_name = None
    eat_by_ord = {}  # ordinal -> ("rva", None) | ("forwarder", target)
    name_to_ord = {}  # export_name -> ordinal

    mode = None
    in_export_section = False
    for line in out.splitlines():
        if line.startswith("The Export Tables"):
            in_export_section = True
            mode = "header"
            continue
        if in_export_section and (
            line.startswith("The Function Table")
            or line.startswith("There is an import table")
            or line.startswith("PE File Base Relocations")
            or line.startswith("Dump of ")
        ):
            in_export_section = False
            mode = None
            continue
        if not in_export_section:
            continue

        if mode == "header":
            m = re.match(r"^Name\s+[0-9a-fA-F]+\s+(\S+)", line)
            if m:
                edata_name = m.group(1).strip()

        if line.startswith("Export Address Table --"):
            mode = "eat"
            continue
        if line.startswith("[Ordinal/Name Pointer] Table --"):
            mode = "names"
            continue

        if mode == "eat":
            # [   0] +base[   1] 00001330 Export RVA
            # [   0] +base[   1] 00008076 Forwarder RVA -- user32.CreateWindowExA
            m_fwd = re.match(
                r"^\s*\[\s*\d+\]\s*\+base\[\s*(\d+)\]\s+[0-9a-fA-F]+\s+Forwarder RVA\s+--\s+(\S+)",
                line,
            )
            if m_fwd:
                eat_by_ord[int(m_fwd.group(1))] = ("forwarder", m_fwd.group(2).strip())
                continue
            m_rva = re.match(
                r"^\s*\[\s*\d+\]\s*\+base\[\s*(\d+)\]\s+[0-9a-fA-F]+\s+Export RVA\b",
                line,
            )
            if m_rva:
                eat_by_ord[int(m_rva.group(1))] = ("rva", None)
        elif mode == "names":
            # [   0] +base[   1]  0000 GameInputCreate
            m_name = re.match(
                r"^\s*\[\s*\d+\]\s*\+base\[\s*(\d+)\]\s+[0-9a-fA-F]+\s+(\S+)",
                line,
            )
            if m_name:
                name_to_ord[m_name.group(2).strip()] = int(m_name.group(1))

    return is_pe32_plus, edata_name, eat_by_ord, name_to_ord, import_dlls


errors = 0
checked_dlls = 0
total_exports = 0

for fname in sorted(os.listdir(defs_dir)):
    if not fname.endswith(".def"):
        continue
    def_path = os.path.join(defs_dir, fname)
    dll_name, expected = parse_def(def_path)
    if not dll_name:
        print(f"ERROR: {fname} missing LIBRARY directive")
        errors += 1
        continue
    dll_path = os.path.join(bin_dir, dll_name)
    if not os.path.exists(dll_path):
        print(f"ERROR: missing built DLL {dll_path}")
        errors += 1
        continue

    out = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", dll_path], text=True)
    is_pe32_plus, edata_name, eat_by_ord, name_to_ord, import_dlls = parse_objdump_pe(out)

    if not is_pe32_plus:
        print(f"ERROR: {dll_name} is not PE32+ (pei-x86-64)")
        errors += 1
        continue

    if not edata_name or edata_name.lower() != dll_name.lower():
        print(f"ERROR: {dll_name} .edata internal name mismatch: {edata_name!r}")
        errors += 1
        continue

    forbidden = sorted(FORBIDDEN_IMPORTS & import_dlls)
    if forbidden:
        print(f"ERROR: {dll_name} dynamically imports forbidden MinGW runtime DLLs: {forbidden}")
        errors += 1
        continue

    expected_names = set(expected.keys())
    actual_names = set(name_to_ord.keys())
    missing = sorted(expected_names - actual_names)
    extra = sorted(actual_names - expected_names)
    if missing or extra:
        print(f"ERROR: {dll_name} export mismatch: missing={missing} extra={extra}")
        errors += 1
        continue

    dll_err = False
    for sym, (exp_ord, exp_fwd) in expected.items():
        actual_ord = name_to_ord[sym]
        if exp_ord is not None and actual_ord != exp_ord:
            print(f"ERROR: {dll_name}!{sym} ordinal mismatch: expected @{exp_ord}, got @{actual_ord}")
            dll_err = True
        eat_entry = eat_by_ord.get(actual_ord)
        if eat_entry is None:
            print(f"ERROR: {dll_name}!{sym} @{actual_ord} missing from Export Address Table")
            dll_err = True
            continue
        kind, actual_fwd = eat_entry
        if exp_fwd is not None:
            if kind != "forwarder" or actual_fwd.lower() != exp_fwd.lower():
                print(
                    f"ERROR: {dll_name}!{sym} expected forwarder {exp_fwd!r}, got {kind}:{actual_fwd!r}"
                )
                dll_err = True
        else:
            if kind != "rva":
                print(f"ERROR: {dll_name}!{sym} expected Export RVA, got {kind}:{actual_fwd!r}")
                dll_err = True

    if dll_err:
        errors += 1
        continue

    checked_dlls += 1
    total_exports += len(expected)
    print(f"  OK {dll_name} ({len(expected)} exact exports verified)")

if errors:
    sys.exit(1)
print(f"Verified {checked_dlls} PE32+ shim DLLs ({total_exports} exact exports, 0 extra, 0 forbidden MinGW imports) in {bin_dir}")
PY
