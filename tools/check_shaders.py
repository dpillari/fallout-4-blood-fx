"""Compile every HLSL shader embedded in src/Shaders.h with fxc, so shader
errors fail the CI build instead of showing up in-game."""
import pathlib, re, subprocess, sys, tempfile

fxc = sys.argv[1]
src = (pathlib.Path(__file__).resolve().parent.parent / "src" / "Shaders.h").read_text()
blocks = dict(re.findall(r'inline constexpr const char\* (\w+) = R"HLSL\((.*?)\)HLSL";', src, re.S))
common = blocks.pop("kCommon")
profiles = {"kEmitCS": "cs_5_0", "kProbeCS": "cs_5_0", "kUpdateCS": "cs_5_0", "kRenderVS": "vs_5_0", "kRenderPS": "ps_5_0"}

failed = False
with tempfile.TemporaryDirectory() as tmp:
    for name, body in blocks.items():
        path = pathlib.Path(tmp) / f"{name}.hlsl"
        path.write_text(common + body)
        r = subprocess.run([fxc, "/nologo", "/T", profiles[name], "/E", "main", "/O3", "/Fo", str(path) + ".cso", str(path)],
                           capture_output=True, text=True)
        print(f"== {name} ({profiles[name]}): {'OK' if r.returncode == 0 else 'FAILED'}")
        if r.stdout.strip(): print(r.stdout)
        if r.stderr.strip(): print(r.stderr)
        failed |= r.returncode != 0
sys.exit(1 if failed else 0)
