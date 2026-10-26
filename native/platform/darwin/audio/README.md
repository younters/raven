# macOS audio

## Upstream and existing interfaces

On 2026-10-25, `git fetch origin` succeeded. Both `gh pr list --repo mirkobrombin/raven --state all --search audio` and
`gh issue list --repo mirkobrombin/raven --state all --search audio` returned no entries. Additional searches for `sound` also returned
none. This is a search result, not a claim that upstream has no related work under other names.
The fetched `origin/main` was `fb18bca`. Its existing audio history includes `d31a764` (wave playback), `1a2ffde` (waveform-device queries),
and `2a5d981` (32-bit DirectSound without a device). There were no audio-file commits in `HEAD..origin/main`; these implementations were
already present in this checkout. No upstream CoreAudio backend was found.

The host implements WinMM `waveOut*` in `native/nt/wave_api.cpp`, registered in both the x64 providers and the x86 shared API registry.
Windows sample buffers are copied into host memory when submitted. Scheduler services return completed headers and signal event
callbacks. Null and event callbacks are supported; guest function/window/thread callbacks remain unsupported.

On Linux, `WaveOut` dynamically loads the five PulseAudio simple functions in that same file. One worker writes buffers to PulseAudio;
without PulseAudio it sleeps and discards samples. Its position counts completed writes. There was no separate host audio interface.
The Darwin implementation supplies the same queue-level methods on `WaveOut` with a small `__APPLE__` adapter; Linux retains its
existing implementation.

The original `nt/x86_dsound.cpp` maps buffers and computes timing without sending samples to a device. On macOS, the host now serves
DirectSound through a shared mixer. MMDevice/WASAPI, basic XAudio2, PlaySound and waveIn use the same native audio layer. See
[IMPLEMENTATION.md](IMPLEMENTATION.md) for the newer interfaces and their verified coverage. Linux keeps its existing behavior.
Microphone authorization is queried without prompting during enumeration and automated tests; only opening an input requests access.

## Backend

`output.h` exposes the queue-level contract: enqueue host-owned data, completed header tokens, pause/restart, reset, busy, position,
padding, and volume. `output.cpp` owns CoreAudio; it has no dependency on Raven or guest pointers.

AudioQueue was chosen over AVAudioEngine or a raw AUHAL unit because its built-in converter handles differing guest/device sample rates
and channel counts, provides a presentation timeline, tracks the system default output, and supports deterministic offline rendering
through the same converter. The host remains arm64; no audio library is loaded into the Rosetta guest.

- Accepts unsigned PCM8, signed little-endian PCM16/24/32, and IEEE float32; 8–384 kHz; one through eight channels. Non-finite floats become
  silence, and floats outside the PCM range are clamped. Rates below 8 kHz are rejected in both WinMM queries and opens: AudioQueue's
  converter rejected these at start on the test host. Guest extensible-format channel masks are not interpreted; conventional CoreAudio
  channel ordering/downmix applies. There is no bit-perfect, exclusive, compressed, or spatial-audio path.
- Converts to interleaved float at the guest sample rate, then lets AudioQueue convert to the device format. Four preallocated 10 ms
  buffers bound queued host audio to about 40 ms, in addition to the device's own latency. Large Windows headers are split across them.
  A worker converts/refills buffers; the CoreAudio callback only publishes a free slot, increments an atomic counter, and wakes the worker.
  No guest access, allocation, mutex acquisition, or AudioQueue operation occurs in that callback.
- Enqueues silence during underruns to keep the timeline continuous. Scheduled buffer sample times map presentation time to guest frames;
  inserted silence does not advance guest position. Header completion waits for presentation, rather than buffer reuse. `position()` reports
  bytes; `padding()` reports submitted guest frames not yet presented, including pending data. WinMM exposes position as bytes/samples/ms.
  There is no `IAudioClient::GetCurrentPadding` or `IAudioClock` export because WASAPI is not implemented.
- Pause retains pending audio and freezes the queue. Reset synchronously stops callbacks, returns unfinished headers once, clears scheduling
  state, and restarts at position zero. Synchronous disposal keeps callback context alive until callbacks stop.
- Atomic left/right WinMM gains scale all supported sample formats as chunks are prepared; zero mutes. Already prepared buffers may retain
  the previous gain for up to 40 ms. Channels other than the second use the left gain, matching the prior WinMM convention.
- Enumerates one logical default output, or zero if CoreAudio has none. Leaving `kAudioQueueProperty_CurrentDevice` unset lets AudioQueue
  follow the system default, including device changes. Device-specific Windows IDs/names and endpoint notifications are not implemented.
  Failure to open/start CoreAudio returns a WinMM error rather than silently discarding audio.

Apple's [AudioQueueGetCurrentTime](https://developer.apple.com/documentation/audiotoolbox/audioqueuegetcurrenttime(_:_:_:_:)) documents
queue time in the queue's sample rate; the SDK's `AudioQueue.h` documents default-device tracking and actual scheduled start times.
[Audio Queue Services](https://developer.apple.com/documentation/audiotoolbox/audio-queue-services) supplies the offline renderer.

## Verification

All CTest invocations must use the shared machine lock, serially:

```sh
/tmp/raven-ctest-lock.sh audio-inventory ctest --test-dir build -j1 -N -R 'audio|sound|wave|dsound|wasapi|xaudio'
/tmp/raven-ctest-lock.sh audio-focused ctest --test-dir build -j1 --output-on-failure -R 'audio|sound|wave|dsound|wasapi|xaudio'
/tmp/raven-ctest-lock.sh audio-full ctest --test-dir build -j1 --timeout 120 --output-on-failure
```

`native_darwin_audio_offline` renders known nonzero samples without sending them to an output device. It checks all integer widths at six rates from
8 to 384 kHz and 1/2/6/8 channels, float conversion, independent stereo volume/mute, pause, reset/reuse, header completion, padding,
underrun position, recycling over a longer stream, and 44.1-to-48 kHz resampling.

`native_darwin_audio_device` sends half a second of silence to the real default device. It checks returning CoreAudio buffers and a
presentation clock reaching 22,050 frames. It skips with code 77 if no default device exists.

`native_darwin_wave_x64` and `native_darwin_wave_x32` are actual Windows programs (`wave_client.c`), run under Raven/Rosetta. They generate
half a second of quiet triangle PCM and explicitly mute the stream before submission, then check pause/restart, header completion,
position, underrun, and reset while in flight. Private build-local prefixes are used. `RAVEN_AUDIO_TRACE=1` prints callback and frame
counts at disposal, providing host-side evidence that Windows submissions reached CoreAudio. No speakers, microphone permission,
loopback driver, Firefox prefix, debugger, or system volume change is needed.

Default-device hot switching is delegated to AudioQueue. The host MMDevice provider also polls the CoreAudio default output and calls
registered `IMMNotificationClient` listeners when it changes. Physical hot switching has not been exercised in automated tests.


## Recorded macOS validation (2026-10-25)

Built with the supplied RelWithDebInfo/Ninja command on native arm64 Apple M4 Pro, macOS 27.2 (26B5091g). The focused locked suite
passed all five tests. The standalone offline contract also passed AddressSanitizer and UndefinedBehaviorSanitizer (system-framework
leak detection disabled).

The full serial comparison used a pristine `eaea484` baseline built in `/tmp/raven-print-baseline` by another delegate and executed
through the shared `printing-baseline` lock. Its ordered 308-test list and Raven/build/component CMake settings were verified identical
to this checkout's frozen baseline, avoiding a redundant full run. Baseline: 301 passed, 3 failed, 4 disabled/skipped (493 seconds).
The final locked run of this checkout: 306 passed, 2 failed, 4 disabled/skipped, 312 total (378 seconds). No previously passing test
regressed; all four added audio contracts passed. `native_layout_failure_component` and `darwin_dxmt_tessellation` failed in both runs.
`native_dll_lifecycle` failed in the baseline and passed in the final run; this unrelated result is not attributed to audio.

The final Windows x64 fixture returned 0 with 69 CoreAudio callbacks and 22,050 presented frames; the x86 fixture returned 0 with
70 callbacks and 22,050 presented frames. Each submitted another 22,050 frames and reset that in-flight header, correctly ending at
position zero. Source triangle PCM was explicitly muted, so verification stayed silent. The direct real-device test reported
56 callbacks, 88,200 presented bytes and zero remaining padding. The offline contract reported zero failures.

[verification.json](verification.json) records counts, exact frame evidence, provenance and limitations. Full logs/XML are retained in
`/tmp/raven-audio-results/`. The only changed upstream file is `native/nt/wave_api.cpp` (+44/-1); all other changes are under this Darwin
platform directory. Removing the Apple branches reproduces the original Linux path apart from formatting of the device-count lambda.
