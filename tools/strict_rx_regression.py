"""Generate HPM TX WAVs and require the ENTIRE payload byte-for-byte in S3 RX.

This is noiseless host interoperability, not a board BER or CPU measurement.
The receiver tool is compiled with the same K<=7 profile as the S3 firmware.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import wave

MODES = """BPSK31 BPSK63 BPSK63F BPSK125 BPSK250 BPSK500 BPSK1000
12xPSK125 6xPSK250 2xPSK500 4xPSK500 2xPSK800 2xPSK1000
QPSK31 QPSK63 QPSK125 QPSK250 QPSK500 8PSK125 8PSK250 8PSK500
8PSK1000 8PSK1000F 8PSK1200F 2x8PSK1200F""".split()
EXPERIMENTAL = {"8PSK1000", "2x8PSK1200F"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hpm-project", type=Path, default=Path("C:/projects/hpm6750-psk-modem"))
    parser.add_argument("--mode", action="append", choices=MODES)
    parser.add_argument("--include-experimental", action="store_true")
    parser.add_argument("--input", type=Path, default=Path(__file__).with_name("short_regression_input.txt"))
    parser.add_argument("--out", type=Path, default=Path("build/strict-rx"))
    parser.add_argument("--tone", type=int, default=1500)
    parser.add_argument("--guard-text", action="store_true", help="Add explicit acquisition/drain text outside measured payload")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    tx = args.hpm_project / "build/host-tools/host_tx_wav.exe"
    rx = root / "build/host-tools/host_psk_rtty_wav.exe"
    env = os.environ.copy()
    env["PATH"] = "C:/cygwin64/bin;" + env.get("PATH", "")
    env.pop("PSK_HOST_NATIVE", None)
    args.out.mkdir(parents=True, exist_ok=True)
    # Normalize the fixture once, before TX; the received bytes are never normalized.
    expected = args.input.read_text(encoding="utf-8").strip().encode("ascii")
    fixture = args.out / "payload.txt"
    fixture.write_bytes(expected)
    stimulus = args.out / "transmit.txt"
    transmitted = (b"VVV " * 8 + b"\n" + expected + b"\n" + b"VVV " * 8) if args.guard_text else expected
    if len(transmitted) > 255:
        raise ValueError("HPM finite TX text buffer accepts at most 255 bytes; shorten the fixture")
    stimulus.write_bytes(transmitted)
    results = []
    for mode in args.mode or [m for m in MODES if args.include_experimental or m not in EXPERIMENTAL]:
        wav, decoded = args.out / (mode + ".wav"), args.out / (mode + ".bin")
        generated = subprocess.run([str(tx), mode, str(wav), "--text-file", str(stimulus),
            "--sample-rate", "16000", "--tone-hz", str(args.tone), "--channels", "1", "--max-seconds", "180"],
            capture_output=True, env=env, timeout=120)
        if generated.returncode:
            raise RuntimeError(generated.stderr.decode(errors="replace"))
        env["PSK_HOST_TONE"] = str(args.tone)
        run = subprocess.run([str(rx), mode, str(wav), str(fixture), "30", str(decoded)],
                             capture_output=True, env=env, timeout=120)
        (args.out / (mode + ".log")).write_bytes(run.stdout + run.stderr)
        actual = decoded.read_bytes() if decoded.exists() else b""
        offset = actual.find(expected)
        with wave.open(str(wav)) as audio:
            duration = audio.getnframes() / audio.getframerate()
        result = {"mode": mode, "pass": run.returncode == 0 and offset >= 0,
                  "payload_bytes": len(expected), "received_bytes": len(actual),
                  "payload_offset": offset, "wav_seconds": round(duration, 3),
                  "payload_sha256": hashlib.sha256(expected).hexdigest()}
        results.append(result)
        print(f"{mode:14s} {'PASS' if result['pass'] else 'FAIL'} full={len(expected)} B received={len(actual)} B offset={offset}", flush=True)
    report = {"scope": "Noiseless HPM TX -> 16 kHz PCM -> S3 FIR/DC -> 64-sample RX, K<=7; host only",
              "tone_hz": args.tone, "guard_text": args.guard_text,
              "tx_exe_sha256": hashlib.sha256(tx.read_bytes()).hexdigest(),
              "rx_exe_sha256": hashlib.sha256(rx.read_bytes()).hexdigest(), "results": results}
    (args.out / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"PASS={sum(r['pass'] for r in results)}/{len(results)}")
    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
