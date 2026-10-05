#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(sys.argv[1]).resolve()
engine = ROOT / "core/src/sampler/SamplerEngine.cpp"
if not engine.is_file():
    raise SystemExit(f"missing {engine}")

s = engine.read_text(encoding="utf-8")
needle = "void startVoice(SamplerEngine::Impl& impl,"
start = s.find(needle)
if start < 0:
    raise SystemExit("startVoice function not found")
end_marker = ") noexcept {"
end = s.find(end_marker, start)
if end < 0:
    raise SystemExit("startVoice signature end not found")
end += len(end_marker)
sig = s[start:end]

required = [
    "SamplerEngine::Impl& impl",
    "SamplerEngine::Impl::Voice& v",
    "const UpperPart& upper",
    "const ProgramLayer& layer",
    "const SampleZone& zone",
    "int upperIndex",
    "std::uint8_t channel",
    "std::uint8_t key",
    "std::uint8_t velocity",
    "const GlobalState& global",
]
for token in required:
    if token not in sig:
        raise SystemExit(f"unexpected startVoice signature; missing {token}: {sig}")
if "ownerState" in sig:
    raise SystemExit("startVoice already has ownerState before final bank patch")

canonical = '''void startVoice(SamplerEngine::Impl& impl,
                 SamplerEngine::Impl::Voice& v,
                 const UpperPart& upper,
                 const ProgramLayer& layer,
                 const SampleZone& zone,
                 int upperIndex,
                 std::uint8_t channel,
                 std::uint8_t key,
                 std::uint8_t velocity,
                 const GlobalState& global) noexcept {'''

if sig != canonical:
    s = s[:start] + canonical + s[end:]
    engine.write_text(s, encoding="utf-8")

final = engine.read_text(encoding="utf-8")
if canonical not in final:
    raise SystemExit("startVoice signature normalization failed")
print("PASS: startVoice signature normalized for final bank runtime patch")
