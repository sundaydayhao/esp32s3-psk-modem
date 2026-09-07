"""Reproducible content-sensitivity probes for the 8PSK1200F rate candidate."""
import json
from pathlib import Path
import random
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / "build/rate-probes"
out.mkdir(parents=True, exist_ok=True)
results = []
for seed in range(10):
    rng = random.Random(seed)
    fixture = out / f"payload-{seed}.txt"
    # Non-space endpoints avoid accidental fixture trimming.
    fixture.write_text("[" + "".join(rng.choice(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,!?/-") for _ in range(166)) + "]",
        encoding="ascii")
    run = subprocess.run([sys.executable, str(root / "tools/strict_rx_regression.py"),
        "--mode", "8PSK1200F", "--guard-text", "--input", str(fixture), "--out", str(out / f"seed-{seed}")],
        cwd=root, check=False)
    report = json.loads((out / f"seed-{seed}/results.json").read_text())
    results.append(dict(seed=seed, **report["results"][0]))
(out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
print(f"8PSK1200F: {sum(r['pass'] for r in results)}/10 payloads; host only")
raise SystemExit(0 if all(r["pass"] for r in results) else 1)
