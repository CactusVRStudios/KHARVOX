"""Compare cumulative observations from the latest captured SFS/AER processes.

No inference about GPU residency or shader-selected mip is made from appends.
"""
import argparse
import re
from pathlib import Path

RECORD = re.compile(r"\[(VT-STATS|TEXTURE-STATS|TEXTURE-CONTEXT)\]\s+(.*)")


def latest_capture(text, renderer):
    records = []
    for line in text.splitlines():
        match = RECORD.search(line)
        if match:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", match[2]))
            if fields.get("renderer") == renderer:
                records.append((match[1], fields))
    contexts = [fields for kind, fields in records if kind == "TEXTURE-CONTEXT"]
    if not contexts:
        raise ValueError(f"{renderer}: no TEXTURE-CONTEXT; enable Extended Logging and capture gameplay")
    pid = contexts[-1].get("pid")
    if not pid:
        raise ValueError(f"{renderer}: process id missing")
    capture = {kind: fields for kind, fields in records if fields.get("pid") == pid}
    missing = {"VT-STATS", "TEXTURE-STATS"} - capture.keys()
    if missing:
        raise ValueError(f"{renderer} pid {pid}: missing {', '.join(sorted(missing))}")
    return capture


def measure(capture, kind, field):
    return capture[kind].get(field, "unavailable")


def retry_mean(capture):
    fields = capture["VT-STATS"]
    count = int(fields.get("observed_retries", "0"))
    return f"{int(fields['retry_wait_ms_total']) / count:.2f}" if count else "unavailable"


def comparison(sfs, aer):
    rows = [
        ("Elapsed VT capture (ms)", "VT-STATS", "elapsed_ms"),
        ("Request observations (includes repeats)", "VT-STATS", "request_observations"),
        ("Deferred observations (includes repeats)", "VT-STATS", "deferred_observations"),
        ("Saturated append calls", "VT-STATS", "saturated_calls"),
        ("Observed retried tokens", "VT-STATS", "observed_retries"),
        ("Maximum observed retry wait (ms)", "VT-STATS", "retry_wait_ms_max"),
        ("Untracked deferrals", "VT-STATS", "untracked_deferrals"),
        ("Vulkan uploaded mips", "TEXTURE-STATS", "uploaded_mips"),
        ("Vulkan upload mip transitions", "TEXTURE-STATS", "upload_mip_transitions"),
        ("Vulkan view base mips", "TEXTURE-STATS", "view_base_mips"),
        ("Minimum sampler LOD bias", "TEXTURE-STATS", "lod_bias_min"),
        ("Maximum sampler LOD bias", "TEXTURE-STATS", "lod_bias_max"),
        ("Anisotropic samplers", "TEXTURE-STATS", "anisotropic"),
        ("Maximum anisotropy", "TEXTURE-STATS", "max_anisotropy"),
        ("Scene eye dimensions", "TEXTURE-CONTEXT", "scene_eye"),
        ("Submitted target dimensions", "TEXTURE-CONTEXT", "target_rect"),
        ("FSR used for observed eye", "TEXTURE-CONTEXT", "fsr_used"),
        ("AA disable requested", "TEXTURE-CONTEXT", "aa_disable_requested"),
        ("Final transfer", "TEXTURE-CONTEXT", "final_transfer"),
    ]
    lines = ["| Observation | SFS | AER |", "|---|---|---|"]
    for label, kind, field in rows:
        lines.append(f"| {label} | {measure(sfs, kind, field)} | {measure(aer, kind, field)} |")
    lines.append(f"| Mean observed retry wait (ms) | {retry_mean(sfs)} | {retry_mean(aer)} |")
    lines.append("\nGPU residency and shader-selected/virtual-page mips are unavailable. "
                 "Compare matched scenes, capture durations and settings. "
                 "Counts describe observed commands and repeated requests, not unique pages or completed GPU uploads.")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sfs", required=True, type=Path)
    parser.add_argument("--aer", required=True, type=Path)
    args = parser.parse_args()
    try:
        sfs = latest_capture(args.sfs.read_text(encoding="utf-8", errors="replace"), "SFS")
        aer = latest_capture(args.aer.read_text(encoding="utf-8", errors="replace"), "AER")
        print(comparison(sfs, aer))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
