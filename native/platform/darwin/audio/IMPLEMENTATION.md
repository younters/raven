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

## Additional Windows interfaces

MMDevice and WASAPI objects are guest vtables in `raven-audio.dll`; their calls enter the same arm64 host `State` as DirectSound.
The enumerator exposes the default CoreAudio render and capture endpoints. Session manager activation supplies basic session control
and per-session volume objects for applications that inspect the endpoint before opening a stream. Each render client copies released guest frames into a
bounded mixer voice. `GetCurrentPadding` counts submitted frames that have not been presented, while `IAudioClock` uses the mixer's
continuous device timeline. Event clients receive scheduler-driven event signals. The default-output notification callback is
delivered on a scheduler service call after the CoreAudio default device ID changes. The host does not change the system device.

For capture, `AudioInput` owns a CoreAudio input queue and bounded host storage. Enumeration and the authorization probe only read
the microphone authorization state. Constructing `AudioInput` when a Windows program actually opens input may request permission;
automated capture opening is skipped unless authorization was already granted. The Raven executable and generated app bundles
carry `NSMicrophoneUsageDescription` so an actual user open can present the macOS prompt. `IAudioCaptureClient` and `waveIn*` consume the
same queue. The queue's CoreAudio callback never touches guest memory. The recording path needs validation on a previously
authorized host; the current machine's authorized capture test skipped.

XAudio2 2.9 (`XAudio2Create`) and 2.7 (`CoCreateInstance`) are small native implementations over `AudioMixer`. Source voices copy
PCM into host storage on `SubmitSourceBuffer`, expose queue state and delivered-frame positions, and send buffer/stream completion
callbacks on the scheduler thread. The mastering voice names the shared output. FAudio was considered: the provisioned Wine
source contains a zlib-licensed copy, but importing its guest DLL and Win32 backend would add a second mixing engine and media
dependencies to a port whose output already has one native mixer. No FAudio component or other binary was added. Submix voices
can be created, but graph routing is approximated by direct output. Effects and compressed input are still unsupported.

PlaySound accepts RIFF PCM/float WAV from Windows paths, memory, or WAVE resources. Aliases enqueue a short, quiet generated tone
through the same WinMM/CoreAudio queue path. `MessageBeep` asks macOS to play the selected alert sound. The PlaySound tone and
MessageBeep are not exercised by automated tests, which submit silence.
