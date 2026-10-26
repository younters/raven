# Remaining Windows audio implementation

Upstream inspected 2026-10-25: `git fetch origin`, HEAD `51f2554`, origin/main `fb18bca`.
`git grep` of DirectSoundCreate, IAudioClient, XAudio2Create, PlaySound and waveInOpen found only the existing timing-only x86
DirectSound, x64 no-driver handlers, WinMM playback and no-input stubs. PR and issue searches (all states) for DirectSound,
WASAPI, XAudio2, PlaySound and capture returned no entries.

## DirectSound milestone

Darwin uses width-aware guest vtables in a private `raven-audio.dll` trap provider. COM state and sample copies live in the arm64
host, matching x86 DirectSound/D3D9's existing object/trap pattern. Linux retains its existing paths.

`AudioMixer` supplies float stereo at the default device's nominal rate to the existing `AudioOutput`. It linearly resamples
PCM voices using the same sample decoder as WinMM. A single process mixer combines independent voices before clipping. Guest memory
is copied only on the scheduler thread at Unlock; the mixer/audio threads never read guest memory or execute guest callbacks.
Mixing is bounded to 50 ms ahead so AudioOutput's 40 ms queue stays supplied. Controls and Stop can therefore take up to the queued
latency to become audible. Positions and notification markers follow AudioOutput's presentation clock, in 10 ms blocks.

Primary and secondary allocations, wrapped locks, shared-memory duplicates, looping, independent frequency/volume/pan, stop/offset
notifications, DirectSoundCreate8 and enumeration are implemented for x86 and x64. 3D SetPosition approximates horizontal placement
with stereo pan; other setters are accepted, getters/effects are unsupported. Hardware acceleration is not advertised.

Locked validation: all seven `darwin_(audio|wave|dsound)|native_x86_dsound` tests passed. The expanded DirectSound fixtures also passed
at both widths: enumeration callback, wrapped lock, mute/pan readback, stop notification, simultaneous looping duplicate with half-rate
playback, and primary SetFormat. x86 recorded 46 CoreAudio callbacks, 11,111 presented frames and 13,440 submitted frames in one run;
these include mixed silence and must not be interpreted as source-buffer frame counts. No audible samples were sent.

Baseline full serial run: 317 listed, three failures (`native_layout_failure_component`, `native_dll_lifecycle`,
`darwin_dxmt_tessellation`), four disabled/skipped, 530.97 seconds. Logs: `/tmp/raven-audio2-results/baseline-complete.log`.
A duplicate baseline was inadvertently queued during initial configuration and was stopped after the complete run was preserved.
