#!/usr/bin/env python3
from pathlib import Path
import atexit
import re
import sys

ROOT = Path(sys.argv[1]).resolve()
engine = ROOT / "core/src/sampler/SamplerEngine.cpp"
if not engine.is_file():
    raise SystemExit(f"missing {engine}")

# -----------------------------------------------------------------------------
# Normalize the real startVoice signature after the earlier DSP patches.
# -----------------------------------------------------------------------------
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

if canonical not in engine.read_text(encoding="utf-8"):
    raise SystemExit("startVoice signature normalization failed")

# -----------------------------------------------------------------------------
# Select the test file that ACTUALLY owns the SV-bank test.
# The source has existed with three test filenames over the project history.
# Detect by content instead of assuming one fixed filename.
# -----------------------------------------------------------------------------
test_candidates = [
    ROOT / "core/tests/CoreTests.cpp",
    ROOT / "core/tests/SamplerCoreTests.cpp",
    ROOT / "core/tests/SamplerTests.cpp",
]

def owns_sv_test(path: Path) -> bool:
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    return (
        "SvSoundRecord sound" in text
        and "SvSoundLayerRecord layer" in text
        and "SvMultisampleRecord ms" in text
        and "SvSampleRecord sample" in text
        and "SvZoneRecord zone" in text
    )

test_path = next((p for p in test_candidates if owns_sv_test(p)), None)
if test_path is None:
    present = []
    for p in test_candidates:
        if p.is_file():
            present.append(f"{p.name}:present-no-sv-test")
        else:
            present.append(f"{p.name}:missing")
    raise SystemExit("cannot locate real SV bank core test: " + ", ".join(present))

# codemagic_bank_runtime_v2.py historically looks only at CoreTests.cpp and
# SamplerCoreTests.cpp. Keep that patch untouched by temporarily presenting the
# real SamplerTests.cpp under the expected SamplerCoreTests.cpp name. All renames
# are restored automatically before this Python process exits, even on failure.
primary = test_candidates[0]
secondary = test_candidates[1]
primary_hold = primary.with_name(primary.name + ".silveton_hold")
secondary_hold = secondary.with_name(secondary.name + ".silveton_hold")
original_test_path = test_path
renamed_primary = False
renamed_secondary = False
aliased_sampler_tests = False

if test_path != primary and primary.is_file():
    if primary_hold.exists():
        raise SystemExit(f"stale temporary test hold exists: {primary_hold}")
    primary.rename(primary_hold)
    renamed_primary = True

if test_path == test_candidates[2]:
    if secondary.exists():
        if secondary_hold.exists():
            raise SystemExit(f"stale temporary test hold exists: {secondary_hold}")
        secondary.rename(secondary_hold)
        renamed_secondary = True
    test_path.rename(secondary)
    test_path = secondary
    aliased_sampler_tests = True


def restore_test_files() -> None:
    try:
        if aliased_sampler_tests and secondary.exists() and not original_test_path.exists():
            secondary.rename(original_test_path)
        if renamed_secondary and secondary_hold.exists() and not secondary.exists():
            secondary_hold.rename(secondary)
        if renamed_primary and primary_hold.exists() and not primary.exists():
            primary_hold.rename(primary)
    except Exception as exc:
        print(f"WARNING: could not restore sampler test filenames: {exc}", file=sys.stderr)

atexit.register(restore_test_files)

# -----------------------------------------------------------------------------
# Canonicalize only the v1 fixture lines that the final v2 patch intentionally
# upgrades. This is test-fixture normalization, not runtime behavior.
# -----------------------------------------------------------------------------
t = test_path.read_text(encoding="utf-8")

def normalize_record(type_name: str, var_name: str, canonical_line: str) -> None:
    global t
    pattern = rf"(?m)^[ \t]*{re.escape(type_name)}[ \t]+{re.escape(var_name)}\{{\}};[^\n]*$"
    matches = list(re.finditer(pattern, t))
    if len(matches) != 1:
        raise SystemExit(
            f"cannot uniquely normalize {type_name} {var_name} in {test_path.name}: found {len(matches)}"
        )
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
    raise SystemExit(
        f"cannot uniquely normalize SV bank address assertion in {test_path.name}: found {len(assert_matches)}"
    )
t = (
    t[:assert_matches[0].start()]
    + "    assert(loaded.data.programs[0].address.bank==2U && loaded.data.programs[0].address.page==3U && loaded.data.programs[0].address.slot==4U);"
    + t[assert_matches[0].end():]
)

# Remove a stable-ID assertion line from a partial previous application, if one
# exists. The final bank-v2 patch adds the canonical assertions exactly once.
t = re.sub(
    r"(?m)^[ \t]*assert\(loaded\.data\.samples\[0\]\.id==3000001U\);[ \t]*assert\(loaded\.data\.multisamples\[0\]\.id==4000001U\);[ \t]*\n?",
    "",
    t,
)

# The final bank runtime performs state replacement at the realtime block boundary
# and deliberately keeps already-playing voices alive through SOUND/COMBI/global
# state changes. The legacy test expected setState() to kill them immediately.
# Update only that one assertion inside testGlobalAdvancedControls so CI verifies
# the new seamless-switch contract instead of the removed click-producing behavior.
fn_start = t.find("void testGlobalAdvancedControls()")
if fn_start < 0:
    raise SystemExit("testGlobalAdvancedControls not found in sampler tests")
fn_end = t.find("\nvoid ", fn_start + 1)
if fn_end < 0:
    fn_end = len(t)
fn_block = t[fn_start:fn_end]
legacy_voice_assert = "assert(e.activeVoiceCount()==0U);"
if fn_block.count(legacy_voice_assert) != 1:
    raise SystemExit(
        "cannot uniquely locate legacy activeVoiceCount assertion in testGlobalAdvancedControls: "
        + str(fn_block.count(legacy_voice_assert))
    )
fn_block = fn_block.replace(
    legacy_voice_assert,
    "assert(e.activeVoiceCount()>0U); // seamless state swap must preserve the held voice",
    1,
)
t = t[:fn_start] + fn_block + t[fn_end:]

test_path.write_text(t, encoding="utf-8")

required_test_anchors = [
    "SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.bank=2U; sound.page=3U; sound.slot=4U;",
    "SvSoundLayerRecord layer{}; layer.multisampleIndex=0U;",
    "SvMultisampleRecord ms{}; ms.nameOffset=msName;",
    "SvSampleRecord sample{}; sample.nameOffset=sampleName;",
    "SvZoneRecord zone{}; zone.sampleIndex=0U;",
    "loaded.data.programs[0].address.bank==2U",
    "assert(e.activeVoiceCount()>0U); // seamless state swap must preserve the held voice",
]
check = test_path.read_text(encoding="utf-8")
for token in required_test_anchors:
    if token not in check:
        raise SystemExit(f"SV v2 test-anchor normalization failed in {test_path.name}: {token}")

mode = ""
if aliased_sampler_tests:
    mode = " (SamplerTests.cpp presented as SamplerCoreTests.cpp for final v2 patch)"
elif renamed_primary:
    mode = " (unrelated CoreTests.cpp temporarily hidden)"
print("PASS: startVoice + SV v2 test anchors + seamless voice regression normalized in " + original_test_path.name + mode)
