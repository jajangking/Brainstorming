"""Driver hermes-reinstall: node-deps.mjs --reuse di env Hermes yang benar.

Dipanggil dengan venv python Hermes di dalam wadah:
    <venv-python> -I -S /tmp/hermes-nodeps-driver.py

Memakai activation_environment (cara venv_sync), supaya receipt key identik
dengan yang dihitung tail build Hermes. Tanpa ini receipt tak pernah cocok
dan npm ci jalan terus -> lefthook crash terus (device 2026-10-09).
"""
import glob
import os
import sys

ROOT = "/root/.hermes/hermes-agent"
sys.path.insert(0, ROOT)
os.chdir(ROOT)

from pathlib import Path  # noqa: E402
from pm.environments import activation_environment  # noqa: E402

env = dict(activation_environment(Path(ROOT)))

nodes = sorted(glob.glob("/root/.hermes/tools/node-*/bin/node"))
if not nodes:
    print("node Hermes tak ada", flush=True)
    raise SystemExit(1)
node = nodes[0]
print(f"driver: node={node}", flush=True)
os.execvpe(node, [node, f"{ROOT}/scripts/build/node-deps.mjs",
                  "--source", ROOT,
                  "--reuse", "--workspace", "ui-tui", "--workspace", "web"],
           env)
