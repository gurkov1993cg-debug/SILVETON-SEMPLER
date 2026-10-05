#!/usr/bin/env python3
"""SILVETON PRO SOUND (must run LAST, after codemagic_crackle_fix.py).

1. MONO / LEGATO: held keys are remembered; releasing the top key returns to the key
   that is still held; LEGATO keeps the same sound and only moves its pitch (true glissando);
   PORTAMENTO glides in pitch.
2. Professional exponential ADSR (ATTACK / DECAY / SUSTAIN / RELEASE in ms and percent) and a
   12 dB low-pass CUTOFF / RESONANCE per Upper, live while keys are held, stored in SET v11.
3. Quick leave / re-enter of the app: the native sampler (bank, BLENDS, audio) is no longer
   replaced, the old Activity can no longer switch off the new one, the bank is loaded
   even when audio needs a second try.

The files are complete replacements. Each one is checked against the exact generated
file it was made from; if anything differs the build stops instead of mixing versions.
"""
import hashlib, os, sys

EXPECTED = [
 [
  "android/app/src/main/cpp/SamplerAudioBridge.cpp",
  "bb3e83f5e598c4c155f6e30dbfc3867dbe29cf5253f7468a800c0c1648d1f26f"
 ],
 [
  "android/app/src/main/java/com/silveton/sampler/NativeSamplerEngine.kt",
  "9058997f9569d604346add3aee88c20d280dd75027e7112a60a05795ee5d8a9d"
 ],
 [
  "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt",
  "e9b99e01a64de5124ed9973e74cd8de698beb6f9742f4fea2e120dcf4d6cd733"
 ],
 [
  "android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt",
  "12ba33d075b9247c7d645f4829363e1abf88b48e2eda2b7fbb989454a4dbf3a4"
 ],
 [
  "android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt",
  "321a403118af211535a1e68524c28e58c7c646f6eea8d276bae48b3ad26f97aa"
 ],
 [
  "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt",
  "8d8a026470966191867eff2c8edf8d418ff1b87310314c90f88d2274804a9e5d"
 ],
 [
  "core/include/silveton/sampler/SamplerEngine.h",
  "942427fd4468591ffa7e1b498d98e5657ae1819498642825e6040deb48056c76"
 ],
 [
  "core/include/silveton/sampler/SamplerTypes.h",
  "c33b56f1d12c0d239d865f9bff574853df206689fe5fa163d6eab1a9cc3b39cf"
 ],
 [
  "core/src/sampler/SamplerEngine.cpp",
  "c50f5541fd90cabfdc8ad6fa484d7d1abfc9cc3fbc55cc39ba61cbb7fb1e0c8a"
 ],
 [
  "core/src/sampler/SetPackage.cpp",
  "79dca32b357d2238bd71c00ec55b097b413bde5f822d0baad8edb17ce7244037"
 ],
 [
  "core/tests/SamplerTests.cpp",
  "011290d64168a59d041c3e25cfc4ba058ec027df59d6851f18b8bcaac4842149"
 ]
]

def main():
    if len(sys.argv) != 2:
        print("usage: codemagic_pro_sound_fix.py PROJECT_DIR", file=sys.stderr); return 2
    project = sys.argv[1]
    payload = os.path.join(os.path.dirname(os.path.abspath(__file__)), "silveton_pro_payload")
    for rel, old_hash in EXPECTED:
        target = os.path.join(project, rel)
        source = os.path.join(payload, rel)
        if not os.path.isfile(target):
            print("ERROR: missing " + rel, file=sys.stderr); return 1
        if not os.path.isfile(source):
            print("ERROR: payload missing " + rel, file=sys.stderr); return 1
        current = hashlib.sha256(open(target, "rb").read()).hexdigest()
        new = open(source, "rb").read()
        if current == hashlib.sha256(new).hexdigest():
            print("already applied: " + rel); continue
        if current != old_hash:
            print("ERROR: " + rel + " is not the file this patch was made for (hash differs)", file=sys.stderr); return 1
        open(target, "wb").write(new)
        print("patched: " + rel)
    print("SILVETON_PRO_SOUND_V1 applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
