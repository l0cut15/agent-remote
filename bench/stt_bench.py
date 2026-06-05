#!/usr/bin/env python3
"""
STT server latency benchmark.

Generates speech audio with macOS `say`, sends identical WAV files to each
server N times, and prints latency stats + transcription quality side-by-side.

Usage:
    python3 bench/stt_bench.py               # 5 runs, default phrases
    python3 bench/stt_bench.py --runs 10 -v  # 10 runs, verbose per-run output
    python3 bench/stt_bench.py --phrase "Hello world"   # single custom phrase

Requirements:
    pip install requests
    macOS only (uses `say` + `afconvert` for audio generation)
"""

import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")

# ── Server definitions ────────────────────────────────────────────────────────

SERVERS = {
    "mac  /inference         (10.10.11.111:7124)": {
        "url":   "http://10.10.11.111:7124/inference",
        "field": "file",
        "extra": {},  # native whisper.cpp — no extra fields needed
    },
    "linux /v1/audio/transcriptions (10.10.11.11:7124)": {
        "url":   "http://10.10.11.11:7124/v1/audio/transcriptions",
        "field": "file",
        "extra": {"model": "whisper-1"},  # OpenAI-compat requires model field
    },
}

# ── Test phrases ──────────────────────────────────────────────────────────────

DEFAULT_PHRASES = [
    "What is the weather like today?",
    "Set a timer for ten minutes.",
    "Turn off the kitchen lights.",
    "The quick brown fox jumps over the lazy dog.",
    "Play some jazz music in the living room.",
]

# ── Audio generation ──────────────────────────────────────────────────────────

def make_wav(text: str, out_path: str) -> int:
    """
    Generate a 16 kHz 16-bit mono WAV via macOS `say` + `afconvert`.
    Returns file size in bytes.
    """
    aiff = out_path + ".aiff"
    try:
        subprocess.run(
            ["say", "-o", aiff, text],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, out_path],
            check=True, capture_output=True,
        )
    finally:
        if os.path.exists(aiff):
            os.unlink(aiff)
    return os.path.getsize(out_path)


# ── Transcription request ─────────────────────────────────────────────────────

def transcribe(url: str, wav_path: str, extra: dict, timeout: int = 60) -> tuple:
    """
    POST WAV to url as multipart/form-data.
    Returns (latency_seconds: float, transcript: str).
    Raises on HTTP error or non-200 status.
    """
    with open(wav_path, "rb") as f:
        wav_bytes = f.read()

    t0 = time.monotonic()
    resp = requests.post(
        url,
        files={"file": ("audio.wav", wav_bytes, "audio/wav")},
        data=extra,
        timeout=timeout,
    )
    latency = time.monotonic() - t0

    if resp.status_code != 200:
        raise RuntimeError(f"HTTP {resp.status_code}: {resp.text[:200]}")

    j = resp.json()
    text = j.get("text", "").strip()
    return latency, text


# ── Benchmark runner ──────────────────────────────────────────────────────────

def run_bench(phrases: list, runs: int, verbose: bool) -> None:
    sep = "─" * 70

    for phrase in phrases:
        print(f"\n{sep}")
        print(f'Phrase  : "{phrase}"')

        with tempfile.TemporaryDirectory() as tmpdir:
            wav = os.path.join(tmpdir, "test.wav")
            size = make_wav(phrase, wav)
            duration = (size - 44) / (16000 * 2)  # bytes → seconds (16kHz 16-bit mono)
            print(f"Audio   : {size:,} bytes  ({duration:.2f}s @ 16 kHz 16-bit mono)\n")

            results = {}
            for name, cfg in SERVERS.items():
                latencies = []
                transcripts = []
                errors = 0

                for i in range(runs):
                    try:
                        lat, txt = transcribe(cfg["url"], wav, cfg["extra"])
                        latencies.append(lat)
                        transcripts.append(txt)
                        if verbose:
                            print(f"  [{i+1}/{runs}] {name.split()[0]:5s}  {lat:.2f}s  → {txt!r}")
                    except Exception as e:
                        errors += 1
                        if verbose:
                            print(f"  [{i+1}/{runs}] {name.split()[0]:5s}  ERROR: {e}")

                results[name] = (latencies, transcripts, errors)

            # Print summary table for this phrase
            for name, (latencies, transcripts, errors) in results.items():
                print(f"  {name}")
                if not latencies:
                    print(f"    ALL {runs} RUNS FAILED")
                    continue

                mean = statistics.mean(latencies)
                med  = statistics.median(latencies)
                mn   = min(latencies)
                mx   = max(latencies)
                p95  = sorted(latencies)[int(len(latencies) * 0.95)]
                best = max(set(transcripts), key=transcripts.count)

                print(f"    transcript : {best!r}")
                print(f"    mean {mean:.2f}s   median {med:.2f}s   "
                      f"min {mn:.2f}s   max {mx:.2f}s   p95 {p95:.2f}s"
                      + (f"   errors {errors}" if errors else ""))
                print()

    print(sep)


# ── Connectivity pre-check ────────────────────────────────────────────────────

def check_servers() -> bool:
    ok = True
    for name, cfg in SERVERS.items():
        host_port = cfg["url"].split("/")[2]
        try:
            r = requests.get(f"http://{host_port}/", timeout=3)
            # Any response (even 404) means the server is reachable
            print(f"  ✓ {name.strip()}")
        except requests.exceptions.ConnectionError:
            print(f"  ✗ {name.strip()}  — UNREACHABLE")
            ok = False
        except Exception:
            print(f"  ✓ {name.strip()}  (responded)")
    return ok


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="STT server latency benchmark")
    parser.add_argument("--runs",   type=int, default=5,
                        help="Requests per phrase per server (default 5)")
    parser.add_argument("--phrase", type=str, default=None,
                        help="Single custom phrase instead of the default set")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Print result of every individual request")
    args = parser.parse_args()

    phrases = [args.phrase] if args.phrase else DEFAULT_PHRASES

    print("STT Benchmark")
    print(f"  {args.runs} runs × {len(phrases)} phrase(s) × {len(SERVERS)} servers\n")
    print("Checking connectivity …")
    check_servers()

    run_bench(phrases, args.runs, args.verbose)


if __name__ == "__main__":
    main()
