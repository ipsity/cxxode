"""Measure the Windows process up to its first flushed prompt."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time


PROMPT = b"lay-code> "
WARM_RUNS = 100


class MemoryCounters(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("page_faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t)
        for name in (
            "peak_working_set", "working_set", "peak_paged_pool", "paged_pool",
            "peak_nonpaged_pool", "nonpaged_pool", "pagefile", "peak_pagefile", "private_usage",
        )
    ]


def memory_reader():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.K32GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD,
    ]
    kernel.K32GetProcessMemoryInfo.restype = wintypes.BOOL

    def private_bytes(pid):
        process = kernel.OpenProcess(0x0410, False, pid)
        if not process:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            size = ctypes.sizeof(MemoryCounters)
            counters = MemoryCounters(size)
            if not kernel.K32GetProcessMemoryInfo(process, ctypes.byref(counters), size):
                raise ctypes.WinError(ctypes.get_last_error())
            return counters.private_usage
        finally:
            kernel.CloseHandle(process)

    return private_bytes


def sample(executable, pool, private_bytes):
    started = time.perf_counter_ns()
    with subprocess.Popen([executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as process:
        try:
            prompt = pool.submit(process.stdout.read, len(PROMPT)).result(timeout=5)
            elapsed_ms = (time.perf_counter_ns() - started) / 1_000_000
            if prompt != PROMPT:
                raise RuntimeError(f"Unexpected CLI prompt: {prompt!r}")
            memory = private_bytes(process.pid)
            output, error = process.communicate(b"Startup benchmark\n", timeout=5)
            if process.returncode or error or b"end_turn requests=2 dispatches=1" not in output:
                raise RuntimeError(f"Unexpected CLI result: {process.returncode}, {output!r}, {error!r}")
            return round(elapsed_ms, 3), memory
        finally:
            if process.poll() is None:
                process.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    if platform.system() != "Windows":
        parser.error("The baseline samples Windows private bytes")
    executable = args.executable.resolve(strict=True)
    private_bytes = memory_reader()
    with ThreadPoolExecutor(max_workers=1) as pool:
        pool.submit(lambda: None).result()
        first = sample(executable, pool, private_bytes)
        warm = [sample(executable, pool, private_bytes) for _ in range(WARM_RUNS)]

    times, memory = zip(*warm)
    print(json.dumps({
        "os": platform.platform(),
        "arch": platform.machine(),
        "compiler": args.compiler,
        "build": "CMake Release",
        "stdout": "pipe",
        "executable_bytes": executable.stat().st_size,
        "first": {"ms": first[0], "private_bytes": first[1]},
        "warm": {
            "count": len(warm),
            "median_ms": statistics.median(times),
            "p95_ms": sorted(times)[94],
            "max_private_bytes": max(memory),
            "samples_ms": times,
            "samples_private_bytes": memory,
        },
    }, indent=2))


if __name__ == "__main__":
    main()
