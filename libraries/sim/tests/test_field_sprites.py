"""Exercise native and widened OBJ rasterization with the configured build flags."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
entries = json.loads((build / "compile_commands.json").read_text())
raster = next(item for item in entries if item["file"].endswith("/sim_g2.cpp"))
variables = next(item for item in entries if item["file"].endswith("/sim_vars.c"))
source = Path(__file__).with_name("field_sprites.cpp").resolve()

with tempfile.TemporaryDirectory(prefix="field-sprites-test-") as directory:
    output = Path(directory)
    args = shlex.split(raster["command"])
    filtered = []
    i = 0
    while i < len(args):
        if args[i] in ("-o", "-MF", "-MQ"):
            i += 2
        elif args[i] == "-MD":
            i += 1
        else:
            filtered.append(args[i])
            i += 1
    subprocess.run(filtered + ["-ffunction-sections", "-fdata-sections",
        "-o", str(output / "raster.o")], cwd=raster["directory"], check=True)
    subprocess.run(["g++", "-Wl,--gc-sections", str(source),
        str(output / "raster.o"), str(Path(variables["directory"]) / variables["output"]),
        "-o", str(output / "test")], check=True)
    subprocess.run([str(output / "test")], check=True)
