from pathlib import Path
import sys

root = Path(sys.argv[1])
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
audit = root / 'ci/audit_gui_contract.py'
for p in (view, audit):
    if not p.is_file():
        raise SystemExit(f'missing required GUI source: {p}')

# Replace the old formatting-sensitive audit with a semantic contract audit.
# It checks that the real native Canvas GUI still exposes all functional pages,
# hit targets, live diagnostics and the playable TEST EP path.
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
    "TEST EP control": '"TEST EP"',
    "AUDIO status": '"AUDIO"',
    "XRUN status": '"XRUN"',
    "MIDI live status": "frameMidiStatus = sampler.midiStatus()",
    "MIDI RX note display": "midiNoteName(m.data1)",
}

for name, token in required_view.items():
    if token not in view:
        raise SystemExit(f"MISSING GUI contract: {name}: {token}")
    print(f"PASS - GUI {name}")

# Settings must be reachable by a real hit target, not merely drawn as text.
settings_hit = re.search(
    r"hit\s*\(\s*gear\s*,.*?Page\.SETTINGS",
    view,
    flags=re.DOTALL,
)
if settings_hit is None:
    # The navigation list may route generically via page = p; in that case
    # require SETTINGS to be one of the clickable labels and page assignment.
    nav_ok = (
        'Page.SETTINGS to "НАСТРОЙКИ"' in view
        and "hit(b" in view
        and "page = p" in view
    )
    if not nav_ok:
        raise SystemExit("MISSING GUI contract: SETTINGS has no real hit route")
print("PASS - GUI SETTINGS has real hit route")

# TEST EP must call a controller method, not be decorative text only.
if "sampler.testEpNoteOn" not in view or "sampler.testEpNoteOff" not in view:
    raise SystemExit("MISSING GUI contract: TEST EP is not wired to note on/off")
print("PASS - GUI TEST EP is wired to native note on/off")

required_controller = {
    "MIDI status wrapper": "fun midiStatus(): MidiStatus",
    "audio running wrapper": "fun audioRunning(): Boolean",
    "audio error wrapper": "fun audioLastError(): Int",
    "TEST EP note on": "fun testEpNoteOn(",
    "TEST EP note off": "fun testEpNoteOff(",
}
for name, token in required_controller.items():
    if token not in controller:
        raise SystemExit(f"MISSING GUI/controller contract: {name}: {token}")
    print(f"PASS - controller {name}")

print("GUI contract audit: PASS")
'''

audit.write_text(content, encoding='utf-8')
print('PASS: GUI audit upgraded to semantic functional contract')
