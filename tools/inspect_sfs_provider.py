"""Static, version-checked disassembly of the locally supplied reference DLL.

Requires pefile and capstone. Does not load or execute the provider.
Outputs stay local; no DLL or game shader is copied into the repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

EXPECTED = "b2ec1ac73a4bdb679c5b7a32286c5acdf3bd84d52e416d98ba2965fcd8646aba"
# Explicit bounds include chained x64 unwind regions, unlike an initial
# RUNTIME_FUNCTION EndAddress which can describe only a function's prologue.
REGIONS = {
    "create_device_dispatch": (0x1a4380, 0x1550),
    "queue_submit": (0x1ade80, 0x65),
    "bind_descriptor_sets": (0x1a3690, 0x50),
    "acquire_next_image": (0x1a26c0, 0x330),
    "queue_present": (0x1ad640, 0x840),
    "stereo_keyed_mutex_copy": (0x18bdd0, 0x32d),
    "present_fence_helper": (0x18eef0, 0x442),
    "begin_render_pass": (0x1a31a0, 0x4e5),
    "render_pass_selection": (0x1cc940, 0x33d),
    "create_image": (0x1a62a0, 0x380),
    "create_image_view": (0x1a6620, 0x1100),
    "create_render_pass": (0x1a8540, 0x4b0),
    "create_framebuffer": (0x1a59c0, 0xf0),
    "dispatch": (0x1a37e0, 0x150),
    "dispatch_policy": (0x1cc6f0, 0x480),
    "profile_hash": (0x1cc5f0, 0x100),
    "pipeline_hash": (0x1ca2b0, 0x2c2),
    "graphics_stage_selection": (0x1e4060, 0x92a),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256(args.dll.read_bytes()).hexdigest()
    if digest != EXPECTED:
        parser.error("Unknown provider build; fixed RVAs must not be applied to it")
    pe = pefile.PE(str(args.dll))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.skipdata = True
    base = pe.OPTIONAL_HEADER.ImageBase
    args.output.mkdir(parents=True, exist_ok=True)
    for name, (rva, size) in REGIONS.items():
        lines = [f"{ins.address-base:08x}  {ins.mnemonic:9} {ins.op_str}"
                 for ins in decoder.disasm(pe.get_data(rva, size), base+rva)]
        (args.output / (name + ".asm")).write_text("\n".join(lines), encoding="utf-8")
    (args.output / "manifest.json").write_text(json.dumps(
        {"provider_sha256": digest, "regions": REGIONS}, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
