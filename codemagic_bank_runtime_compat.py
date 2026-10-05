#!/usr/bin/env python3
from pathlib import Path
import re
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

# The final SV v2 patch intentionally upgrades an existing v1 bank-reader test.
# Earlier source patches can reformat or partially touch those one-line records,
# which made exact string replacement brittle. Canonicalize the six test anchors
# back to their v1 forms here; codemagic_bank_runtime_v2.py then performs the
# actual v2 conversion and verifies the resulting stable IDs.
test_candidates = [
    ROOT / "core/tests/CoreTests.cpp",
    ROOT / "core/tests/SamplerCoreTests.cpp",
]
test_path = next((p for p in test_candidates if p.is_file()), None)
if test_path is None:
    raise SystemExit("missing sampler core test source")

t = test_path.read_text(encoding="utf-8")

def normalize_record(type_name: str, var_name: str, canonical_line: str) -> None:
    global t
    pattern = rf"(?m)^[ \t]*{re.escape(type_name)}[ \t]+{re.escape(var_name)}\{{\}};[^\n]*$"
    matches = list(re.finditer(pattern, t))
    if len(matches) != 1:
        raise SystemExit(f"cannot uniquely normalize {type_name} {var_name}: found {len(matches)}")
    t = t[:matches[0].start()] + canonical_line + t[matches[0].end():]

normalize_record(
    "SvSoundRecord", "sound",
    "    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.bank=2U; sound.page=3U; sound.slot=4U;"
)
normalize_record(
    "SvSoundLayerRecord", "layer",
    "    SvSoundLayerRecord layer{}; layer.multisampleIndex=0U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;"
)
normalize_record(
    "SvMultisampleRecord", "ms",
    "    SvMultisampleRecord ms{}; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;"
)
normalize_record(
    "SvSampleRecord", "sample",
    "    SvSampleRecord sample{}; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;"
)
normalize_record(
    "SvZoneRecord", "zone",
    "    SvZoneRecord zone{}; zone.sampleIndex=0U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;"
)

assert_pattern = r"(?m)^[ \t]*assert\(loaded\.data\.programs\[0\]\.address\.bank==[^\n]*$"
assert_matches = list(re.finditer(assert_pattern, t))
if len(assert_matches) != 1:
    raise SystemExit(f"cannot uniquely normalize SV bank address assertion: found {len(assert_matches)}")
t = (
    t[:assert_matches[0].start()]
    + "    assert(loaded.data.programs[0].address.bank==2U && loaded.data.programs[0].address.page==3U && loaded.data.programs[0].address.slot==4U);"
    + t[assert_matches[0].end():]
)

# Remove a stable-ID assertion line left by an earlier partial application, if any;
# the final v2 patch will add the canonical pair exactly once.
t = re.sub(
    r"(?m)^[ \t]*assert\(loaded\.data\.samples\[0\]\.id==3000001U\);[ \t]*assert\(loaded\.data\.multisamples\[0\]\.id==4000001U\);[ \t]*\n?",
    "",
    t,
)

test_path.write_text(t, encoding="utf-8")

required_test_anchors = [
    "SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.bank=2U; sound.page=3U; sound.slot=4U;",
    "SvSoundLayerRecord layer{}; layer.multisampleIndex=0U;",
    "SvMultisampleRecord ms{}; ms.nameOffset=msName;",
    "SvSampleRecord sample{}; sample.nameOffset=sampleName;",
    "SvZoneRecord zone{}; zone.sampleIndex=0U;",
    "loaded.data.programs[0].address.bank==2U",
]
check = test_path.read_text(encoding="utf-8")
for token in required_test_anchors:
    if token not in check:
        raise SystemExit(f"SV v2 test-anchor normalization failed: {token}")

print("PASS: startVoice + SV v2 test anchors normalized for final bank runtime patch")
