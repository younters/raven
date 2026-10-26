# macOS audio

## Upstream and existing interfaces

On 2026-10-25, `git fetch origin` succeeded. Both `gh pr list --repo mirkobrombin/raven --state all --search audio` and
`gh issue list --repo mirkobrombin/raven --state all --search audio` returned no entries. Additional searches for `sound` also returned
none. This is a search result, not a claim that upstream has no related work under other names.

The host implements WinMM `waveOut*` in `native/nt/wave_api.cpp`, registered in both the x64 providers and the x86 shared API registry.
Windows sample buffers are copied into host memory when submitted. Scheduler services return completed headers and signal event
callbacks. Null and event callbacks are supported; guest function/window/thread callbacks remain unsupported.

On Linux, `WaveOut` dynamically loads the five PulseAudio simple functions in that same file. One worker writes buffers to PulseAudio;
without PulseAudio it sleeps and discards samples. Its position counts completed writes. There was no separate host audio interface.
The Darwin implementation supplies the same queue-level methods on `WaveOut` with a small `__APPLE__` adapter; Linux retains its
existing implementation.

`nt/x86_dsound.cpp` only maps buffer memory and computes cursors/status from elapsed wall time. It does not submit samples to WinMM
or any host audio device. No WASAPI (`IAudioClient`, `MMDevice`), XAudio2 engine, or working `PlaySound` implementation was found.
`MessageBeep` returns success without audio. Merely loading an XAudio2 DLL in the existing DirectX check is not playback coverage.
`waveInGetNumDevs` reports zero and `waveInOpen` rejects recording: there is no existing capture interface to implement. This change
never accesses a microphone or requests permission; future capture work needs a usage description and an explicit authorization check
before opening an input device, with unauthorized automated tests skipped.

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

`native_darwin_audio_offline` renders known nonzero samples without an output device. It checks all integer widths at six rates from
8 to 384 kHz and 1/2/6/8 channels, float conversion, independent stereo volume/mute, pause, reset/reuse, header completion, padding,
underrun position, recycling over a longer stream, and 44.1-to-48 kHz resampling.

`native_darwin_audio_device` sends half a second of silence to the real default device. It checks returning CoreAudio buffers and a
presentation clock reaching 22,050 frames. It skips with code 77 if no default device exists.

`native_darwin_wave_x64` and `native_darwin_wave_x32` are actual Windows programs (`wave_client.c`), run under Raven/Rosetta. They generate
half a second of quiet triangle PCM and explicitly mute the stream before submission, then check pause/restart, header completion,
position, underrun, and reset while in flight. Private build-local prefixes are used. `RAVEN_AUDIO_TRACE=1` prints callback and frame
counts at disposal, providing host-side evidence that Windows submissions reached CoreAudio. No speakers, microphone permission,
loopback driver, Firefox prefix, debugger, or system volume change is needed.

Default-device hot switching is delegated to AudioQueue and has not been exercised by changing the user's active output device. Real
DirectSound/WASAPI/XAudio2/PlaySound apps still need their missing Windows-facing implementations; this backend enables WinMM playback.
