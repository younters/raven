# Audio on macOS

WinMM playback uses CoreAudio Audio Queue Services. The host copies samples from Windows buffers on submission and
converts them to float. AudioQueue converts to the system's output format. Completion and position follow the
presentation clock. Silence inserted during underruns leaves the Windows position unchanged.

DirectSound, shared-mode WASAPI and XAudio2 use a host mixer on the same output backend. Raven exposes width-specific
guest vtables through `raven-audio.dll`. The host stores voice state and mixes copies of submitted samples. AudioQueue
callbacks work with host buffers. Windows completion callbacks are delivered through the scheduler.

MMDevice exposes the default render and capture endpoints. WASAPI capture and WinMM `waveIn` use a CoreAudio input queue.
Opening input can request microphone permission. Endpoint enumeration checks authorization without prompting.

## Tests

```sh
ctest --test-dir build-macos -j1 --output-on-failure -R '^native_darwin_(audio|wave|dsound|wasapi|xaudio)'
```

The offline output test checks conversion and the presentation clock. Device tests need the default device. The capture
fixtures require existing microphone permission.
