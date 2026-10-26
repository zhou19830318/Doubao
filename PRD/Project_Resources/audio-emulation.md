# Audio Emulation via PC Speakers

## Purpose
Emulate voice input to the SenseCAP Watcher device by playing audio through PC speakers, which the device mic picks up.

## Method: Windows System.Speech (TTS)
```powershell
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = -1    # Slightly slower for better mic pickup
$synth.Volume = 100
$synth.Speak("What is your name?")
```

## Coordination with Device Recording
1. Send `talk` command to device serial monitor
2. Wait ~300ms for the start sound to play
3. Play audio from PC speakers
4. Device mic picks up the audio
5. Silence detection stops recording after ~1.2s of silence

## Important Notes
- The start recording beep may be picked up by the mic — there's a 100ms delay after the beep before recording starts to minimize this
- Background noise can trigger speech detection (avg_abs=150+ is common in noisy environments)
- The silence threshold is 80 RMS — speech should be significantly louder
- STT may misinterpret background noise as speech — this is expected
- For reliable testing, use `say <message>` serial command which bypasses audio entirely

## Available Audio Devices on This PC
- Audio Kontrol 1 WDM Audio
- Realtek High Definition Audio  
- USB Audio Device (x2)
- NVIDIA Virtual/HD Audio
- Oculus Virtual Audio Device
