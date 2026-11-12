"""Black-box CLI check using fixed Animus machine words, with no C++ fixture."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)
# Parent: WSPAWN(4,32), then WSPAWN(1,0), then TMC(0).
# Each child executes x3=7 with lane 0 only, then TMC(0).
WORDS = [0x00400093, 0x02000113, 0x0020800B, 0x00100093, 0x0000800B,
         0x0200000B, 0x00000013, 0x00000013, 0x00700193, 0x0200000B]
image = BUILD / "management.bin"
image.write_bytes(struct.pack("<10I", *WORDS))
executable = BUILD / "animus_model_cli.exe"
if not executable.exists():
    executable = BUILD / "animus_model_cli"
environment = os.environ.copy()
compiler = shutil.which("g++")
fallback = Path("D:/mingw-19.0/MinGW/bin/g++.exe")
if compiler or fallback.exists():
    directory = Path(compiler).parent if compiler else fallback.parent
    environment["PATH"] = str(directory) + os.pathsep + environment.get("PATH", "")
# Keep the guest image argument relative, as in the existing Windows oracles:
# MinGW narrow argv/filesystem conversion cannot reliably consume this Chinese
# workspace's absolute path. Python itself still writes files via Unicode paths.
result = subprocess.run([str(executable), str(image.relative_to(ROOT)), "--run"], cwd=ROOT,
                        env=environment, text=True, capture_output=True, timeout=30)
if result.returncode:
    raise RuntimeError(f"CLI exit {result.returncode}: {result.stderr}")
records = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
assert len(records) == 12, "Six parent instructions plus two per child"
assert [r["step"] for r in records] == list(range(1, 13))
assert sum(r["op"] == "gpu.wspawn" for r in records) == 2
assert sum(r["op"] == "gpu.tmc" for r in records) == 4
for warp in (1, 2, 3):
    child = [r for r in records if r["warp"] == warp]
    assert len(child) == 2 and child[0]["pc_before"] == 32
    assert child[0]["exec_before"] == 1
    assert [(w["lane"], w["reg"], w["after"]) for w in child[0]["writes"]] == [(0, 3, 7)]
    assert child[1]["ended"] and child[1]["exec_after"] == 0
assert "Completed management program after 12 instructions." in result.stderr
(BUILD / "management_trace.jsonl").write_text(result.stdout, encoding="utf-8")
print("PASS management CLI: 12 instructions, 3 fresh children, lane-0 writes and 4 TMC exits; JSONL parsed")
