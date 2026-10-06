#!/usr/bin/env python3
"""SILVETON PRO SOUND (must run LAST, after codemagic_crackle_fix.py).

1. PORTAMENTO exactly like the SILVETON portamento page (Silveton-Portamento-Keys): while
   portamento is ON an Upper plays one line (last key wins, back to a key still held). While a
   key is held the line is one continuous sound - no new strike; the pitch glides from the pitch
   sounding now to the new key in exactly TIME (1..100 ms), evenly in semitones, the same time
   for every interval. With no key held the next note is struck normally without a glide; the
   note that already sounds pressed again changes nothing. PORTA BALANCE is gone (it only chose
   the CC#5 number for a PA600 and never changed the sound).
2. Professional exponential ADSR and a 12 dB low-pass CUTOFF / RESONANCE per Upper (TONE),
   live while keys are held, stored in SET v11.
3. Audio start exactly as in the working crackle-fix version; if the PCM reports EINVAL
   (no DL1 -> DAC mixer route) the app sets the proven SM-X130 route itself and opens again.
4. Headphone lock: SILVETON plays only through the headphone jack (DL1 only into the ADDA
   headphone DAC, line-out/speaker mux open, Headset Volume 12); re-checked while playing.
   Tablet volume keys change SILVETON's MASTER and are kept away from Android's audio system.
6. The SOUND list of an Upper opens on that Upper's own sound (e.g. position 53) and the
   BLENDS list on the loaded blend, instead of position 1.
5. Render speed: every voice renders its block at once; controller, bend, filter base, pan and
   level are computed once per block, filter coefficients only when they change. Same sound
   (bit-exact against the previous engine), about 2.7x less CPU per voice.
7. New look (the approved design): dark side menu, amber LCD screens, seven UPPER strips with
   real faders, PAN knobs and L/R meters, knob boards for TUNE / TONE / FX / EFFECTS / SETTINGS.
   Every key, fader and knob drives the real engine. Drawn at exactly 1340x800 and scaled
   evenly on other screens; touches are mapped back the same way.
8. WRITE like Korg: rename, then pick any LIST position (1..200) or BLEND (1..500); a used
   position is overwritten after a confirmation, without touching any other entry or blend.
   The LIST shows a saved, renamed, moved, copied or deleted entry at once.
9. Every setting is heard at once on the notes you hold: Upper level, pan, ON / MUTE / SOLO
   (smoothed, no clicks), octave / transpose / fine / master tune, TONE, LFO, EQ, FX,
   controllers - as long as the Upper keeps the same Sound (a replaced Sound's notes keep
   their own settings). The number box changes the sound while its slider or - / + is
   moved; CANCEL puts the old value back.
10. LIST positions are fixed like Korg Performances: WRITE to any position 1..200 (also after
   empty ones), COPY and MOVE to a position chosen on the same screen (a used one is
   overwritten after a confirmation), DELETE leaves the place empty - nothing else moves.
11. Effects: FX1 / FX2 knobs now reach FX1 / FX2 (they were one slot off), FX1 and FX2 have
   their own engines, DELAY with working TONE and click-free TIME, a new studio plate REVERB
   (DECAY 0.6..12 s, TONE), and every effect shows only the knobs it really uses.
12. Studio interpolation (live default): Kaiser-windowed sinc chosen by playback ratio
   (24..64 points, 12 kernels per octave). 0..20 kHz flat, every image/alias that could
   land in 0..20 kHz rejected by 110 dB or more; after the first loop pass the frames
   before the loop start are taken from the loop end. The old 4-point cubic stays available.
13. Zero-latency smooth limiter on MASTER (no added delay): the gain drops to exactly
   what a peak needs and comes back in 80 ms, so loud playing is not cut flat.
14. TPDF dither before the 16-bit output: quiet tails fade smoothly, digital silence
   stays silent.
15. MIDI: every input port of each USB MIDI device is read (a Korg sends what enters its
   MIDI IN, e.g. a breath controller, on its second port).
16. Soft portamento: every glide follows an S-curve in TIME, and PORTA BALANCE (3..40 ms,
   default 11, tap the PORTA field on the status screen) rounds it further.

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
  "android/app/src/main/java/com/silveton/sampler/MainActivity.kt",
  "02187e015b8bd50af51a95b5332f049e19e7fe6039e2968e48edf6e13e46fea4"
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
  "core/include/silveton/sampler/SincInterpolator.h",
  "558a9d614599cc653f61ac9776ab38292befd651fa79e896304c697a7af3d719"
 ],
 [
  "core/src/sampler/SincInterpolator.cpp",
  "d0ec1d739fe2a342925a471c92340c6277e534e82ddb9a94e7856d1067173d9a"
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
