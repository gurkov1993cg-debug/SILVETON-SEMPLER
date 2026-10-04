from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
audit = root / 'ci/audit_gui_contract.py'
for p in (view, controller, audit):
    if not p.is_file():
        raise SystemExit(f'missing required GUI source: {p}')

# Repair the diagnostics patch if MidiStatus was inserted into the constructor's
# statusChanged default lambda. That placement is invalid Kotlin and caused
# compileReleaseKotlin to fail even though all source audits passed.
s = controller.read_text(encoding='utf-8')
ctor_start = s.find('class SamplerAppController(')
body_anchor = ') : AutoCloseable {'
body_pos = s.find(body_anchor, ctor_start)
if ctor_start < 0 or body_pos < 0:
    raise SystemExit('SamplerAppController class header missing')
pre_body = s[ctor_start:body_pos]
if 'data class MidiStatus(' in pre_body:
    m = re.search(
        r'\n\s*data class MidiStatus\(\n'
        r'\s*val deviceCount: Int,\n'
        r'\s*val lastError: Int,\n'
        r'\s*val activitySerial: Long,\n'
        r'\s*val status: Int,\n'
        r'\s*val data1: Int,\n'
        r'\s*val data2: Int\n'
        r'\s*\)\n',
        pre_body,
    )
    if not m:
        raise SystemExit('malformed MidiStatus block not recognized')
    midi_block = '''\n    data class MidiStatus(\n        val deviceCount: Int,\n        val lastError: Int,\n        val activitySerial: Long,\n        val status: Int,\n        val data1: Int,\n        val data2: Int\n    )\n'''
    pre_body = pre_body[:m.start()] + pre_body[m.end():]
    pre_body = pre_body.replace(
        'private val statusChanged: (String?) -> Unit = {\n},',
        'private val statusChanged: (String?) -> Unit = {},'
    )
    s = s[:ctor_start] + pre_body + s[body_pos:]
    body_pos = s.find(body_anchor, ctor_start)
    insert_at = body_pos + len(body_anchor)
    s = s[:insert_at] + midi_block + s[insert_at:]

body_pos = s.find(body_anchor, ctor_start)
pre_body = s[ctor_start:body_pos]
post_body = s[body_pos:]
if 'data class MidiStatus(' in pre_body:
    raise SystemExit('Kotlin structure invalid: MidiStatus still inside constructor parameters')
if 'data class MidiStatus(' not in post_body:
    raise SystemExit('Kotlin structure invalid: MidiStatus missing from class body')
if 'private val statusChanged: (String?) -> Unit = {},' not in pre_body:
    raise SystemExit('Kotlin structure invalid: statusChanged default lambda')
controller.write_text(s, encoding='utf-8')
print('PASS: Kotlin controller structure valid (MidiStatus in class body)')

# Semantic GUI contract audit. The user-facing hardware-test instrument is now
# labelled PIANO / STAGE PIANO, while the existing internal native wrapper names
# testEpNoteOn/testEpNoteOff remain implementation details.
content = r'''#!/usr/bin/env python3
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"

if not view_path.is_file():
    raise SystemExit(f"missing GUI view: {view_path}")
if not controller_path.is_file():
    raise SystemExit(f"missing controller: {controller_path}")

view = view_path.read_text(encoding="utf-8")
controller = controller_path.read_text(encoding="utf-8")

required_view = {
    "native Canvas renderer": "class SilvetonSamplerView",
    "LIST page": "Page.LIST",
    "COMBI page": "Page.COMBI",
    "EFFECTS page": "Page.EFFECTS",
    "SETTINGS page": "Page.SETTINGS",
    "LIST renderer": "drawList(",
    "COMBI renderer": "drawCombi(",
    "EFFECTS renderer": "drawEffects(",
    "SETTINGS renderer": "drawSettings(",
    "panic action": "sampler.panic()",
    "PIANO control": '"PIANO"',
    "STAGE PIANO set-list slot": '"STAGE PIANO"',
    "AUDIO status": '"AUDIO"',
    "XRUN status": '"XRUN"',
    "MIDI live status": "frameMidiStatus = sampler.midiStatus()",
    "MIDI RX note display": "midiNoteName(m.data1)",
}

for name, token in required_view.items():
    if token not in view:
        raise SystemExit(f"MISSING GUI contract: {name}: {token}")
    print(f"PASS - GUI {name}")

settings_hit = re.search(
    r"hit\s*\(\s*gear\s*,.*?Page\.SETTINGS",
    view,
    flags=re.DOTALL,
)
if settings_hit is None:
    nav_ok = (
        'Page.SETTINGS to "НАСТРОЙКИ"' in view
        and "hit(b" in view
        and "page = p" in view
    )
    if not nav_ok:
        raise SystemExit("MISSING GUI contract: SETTINGS has no real hit route")
print("PASS - GUI SETTINGS has real hit route")

if "sampler.testEpNoteOn" not in view or "sampler.testEpNoteOff" not in view:
    raise SystemExit("MISSING GUI contract: PIANO control is not wired to native note on/off")
print("PASS - GUI PIANO control is wired to native note on/off")

required_controller = {
    "MIDI status wrapper": "fun midiStatus(): MidiStatus",
    "audio running wrapper": "fun audioRunning(): Boolean",
    "audio error wrapper": "fun audioLastError(): Int",
    "PIANO note on": "fun testEpNoteOn(",
    "PIANO note off": "fun testEpNoteOff(",
}
for name, token in required_controller.items():
    if token not in controller:
        raise SystemExit(f"MISSING GUI/controller contract: {name}: {token}")
    print(f"PASS - controller {name}")

class_pos = controller.find("class SamplerAppController(")
body_pos = controller.find(") : AutoCloseable {", class_pos)
if class_pos < 0 or body_pos < 0:
    raise SystemExit("MISSING GUI/controller contract: SamplerAppController class header")
if "data class MidiStatus(" in controller[class_pos:body_pos]:
    raise SystemExit("INVALID KOTLIN: MidiStatus is inside constructor parameters")
print("PASS - controller Kotlin class structure")

print("GUI contract audit: PASS")
'''

audit.write_text(content, encoding='utf-8')
print('PASS: GUI audit aligned with PIANO/STAGE PIANO live-performance contract')
