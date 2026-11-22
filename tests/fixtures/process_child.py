import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


mode = sys.argv[1]
if mode == "arguments":
    print(json.dumps({
        "argv": sys.argv[2:],
        "cwd": str(Path.cwd()),
        "secret": os.environ.get("ANTHROPIC_API_KEY"),
        "stdin_closed": sys.stdin.read() == "",
    }))
elif mode == "flood":
    out = sys.stdout.buffer
    err = sys.stderr.buffer
    out.write(b"STDOUT-START\n")
    err.write(b"STDERR-START\n")
    for i in range(800):
        out.write(b"x" * 65536)
        err.write(b"y" * 65536)
    out.write(b"\x1b[31mSTDOUT-END\xff\n")
    err.write(b"STDERR-END\n")
elif mode == "linger":
    subprocess.Popen([sys.executable, __file__, "hold"])
    print("leader exited")
elif mode == "hold":
    if os.name != "nt":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    print("child started", flush=True)
    time.sleep(30)
elif mode == "exit127":
    sys.exit(127)
else:
    raise RuntimeError(mode)
