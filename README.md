# moonlight-audio

`moonlight-audio` is a native macOS command-line Moonlight/GameStream client for
Sunshine that uses the Moonlight Qt identity already paired on the same Mac. It
has no Qt GUI, QML, SDL video initialization, video decoder, Metal, OpenGL, or
VideoToolbox dependency. Encoded video decode units are acknowledged and
discarded immediately; audio is decoded with libopus and played with CoreAudio.

This is an early implementation for Apple Silicon. It deliberately has no
pairing command: pair with the official Moonlight desktop app first.

## Build

Install the build dependencies:

```sh
brew install cmake qt opus openssl pkgconf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first CMake configure downloads the pinned
[`moonlight-common-c` revision `62e0663`](https://github.com/moonlight-stream/moonlight-common-c/tree/62e066388f1a1b133e0bee947b9a374311a3354b).
For an offline build, populate its source first and point CMake's FetchContent
source directory at it.

## Nightly releases

Every successful commit pushed to `main` produces an Apple Silicon release
asset at the GitHub prerelease tagged `nightly`. The tag and release asset are
replaced on the next successful `main` build. Each release contains
`moonlight-audio-macos-arm64.tar.gz` and a matching SHA-256 checksum.

## Run

```sh
build/moonlight-audio hosts
build/moonlight-audio apps Gaming-PC
build/moonlight-audio Gaming-PC --app Desktop
build/moonlight-audio 192.168.1.50 --app Steam --verbose
build/moonlight-audio Gaming-PC --attach
build/moonlight-audio Gaming-PC --attach --duration 15 --video-size 640x360 --fps 30 --bitrate 1000
```

Host names, UUIDs, and each saved local/manual/remote/IPv6 address are matched
against Moonlight's known-host records. An address which is absent from those
records is intentionally rejected because it cannot prove an existing pairing.
For an existing record, the CLI attempts every saved address until mutual TLS
and the pinned host certificate succeed. Supplying a saved address explicitly
tries it first.
`Desktop` is selected when `--app` is omitted, except when Sunshine already has
an active application: in that case the CLI resumes the active application.
Use `--attach` to make that choice explicit and ignore `--app`. This is how to
receive audio while another device is showing the active application’s video.

## Moonlight Qt data reuse

This project inspected Moonlight Qt at commit
[`e3fd29e`](https://github.com/moonlight-stream/moonlight-qt/tree/e3fd29e4d7dc5723d8d0da7d19e2698daec74456),
in particular:

- `app/main.cpp`: assigns organization `Moonlight Game Streaming Project`,
  domain `moonlight-stream.com`, and application name `Moonlight` before any
  `QSettings` construction.
- `app/backend/identitymanager.cpp`: stores the PEM client certificate as
  `certificate`, private key as `key`, and client ID as `uniqueid` in that
  settings domain. They are not separate files or Keychain items.
- `app/backend/computermanager.cpp`: loads `hostsbackup` if present, otherwise
  the `hosts` QSettings array.
- `app/backend/nvcomputer.cpp`: serializes `hostname`, `uuid`, addresses and
  ports, `srvcert` (the pinned host PEM certificate), `nvidiasw`, and the
  cached `apps` array. It marks a live server paired only when `/serverinfo`
  reports `PairStatus == 1`.
- `app/backend/nvhttp.cpp`: configures mutual TLS with the client certificate
  and private key, and ignores TLS errors only when the server certificate is
  exactly the stored pinned certificate. `moonlight-audio` follows that rule.

The compiled upstream dependency is all of `moonlight-common-c` (including its
ENet and nanors submodules), which provides GameStream launch/session, RTSP,
encrypted audio/video transport, RTP depacketization, and connection lifecycle.
No Moonlight Qt class is linked directly: `ComputerManager` starts polling and
can schedule host writes, while `IdentityManager` generates and persists
missing settings. That conflicts with the intentional read-only requirement.
The small `MoonlightConfig` reader reproduces only their documented serialized
keys and loading order above, then closes the settings object.

For a Sunshine request, the required authentication material is the Moonlight
client certificate and matching private key (mutual TLS), the saved pinned
Sunshine server certificate (server identity check), and the Moonlight client
ID in the request query. A host UUID is used to identify a saved record; it is
not the TLS credential. Application metadata is cached under
`hosts/<index>/apps`, but `apps` intentionally obtains a fresh authenticated
list rather than trusting that cache.

For a standard installed Moonlight bundle, its `Info.plist` bundle identifier
is `com.moonlight-stream.Moonlight`. On this macOS installation, Qt native
settings resolve to:

```text
~/Library/Preferences/com.moonlight-stream.Moonlight.plist
```

The CLI sets the same three `QCoreApplication` identifiers before constructing
its sole `QSettings` object; it never calls `setValue()`, `remove()`, or
`sync()`. It copies the necessary certificate/key/host data into memory during
startup, so reading while Moonlight is open does not hold a writable settings
store.

The current Qt source lazily generates and persists `uniqueid` if it is
missing. Some older Moonlight installations have certificate/key material but
no `uniqueid` yet. To preserve read-only operation, this client uses the stored
value when present and otherwise derives a deterministic, in-memory 16-hex
request ID from the existing certificate. Sunshine authorization is based on
the exact client certificate it receives in the TLS handshake, so this neither
creates nor pairs a new client identity.

`apps` first performs authenticated `/serverinfo`, checks the live
`PairStatus`, then retrieves authenticated `/applist`. Streaming repeats those
checks and performs `/launch` (or `/resume`), so a stale saved certificate or a
server-side unpair is reported before transport startup.

## Streaming design

The launch stream configuration requests 320×180, 30 FPS, 500 Kbps H.264 and
stereo audio. Current Sunshine source at commit
[`8cd8539`](https://github.com/LizardByte/Sunshine/tree/8cd8539a1518fd148c3b5520994e588ae264b96e)
parses the requested RTSP width, height, frame rate and bitrate without a
documented minimum-resolution validation. Therefore 320×180 is an
implementation choice, not a Sunshine guarantee; hosts/encoders that reject it
can use `--video-size`, `--fps`, and `--bitrate`. H.264 is selected because it is
the baseline GameStream codec.

Moonlight-common-c still receives the video transport, but its
`DECODER_RENDERER_CALLBACKS::submitDecodeUnit` returns `DR_OK` immediately with
`CAPABILITY_DIRECT_SUBMIT`. No encoded frame is decoded or rendered.

The audio callback receives Moonlight-common-c's full
`OPUS_MULTISTREAM_CONFIGURATION` and constructs an `OpusMSDecoder` with the
negotiated sample rate, channel count, streams, coupled-stream count, mapping,
and samples-per-frame. It writes signed 16-bit PCM into a lock-free SPSC ring.
Moonlight-common-c runs that callback on its dedicated audio decoder thread; it
does not run on the UDP receive thread. The CoreAudio `AudioQueue` callback only
copies available samples or writes silence. It allocates neither memory nor
blocks. CoreAudio starts only after a **20 ms** PCM prebuffer. The device queue
has three 480-frame buffers: **30 ms at 48 kHz**. The PCM jitter ring is capped
at **200 ms**, which can absorb short Wi-Fi or macOS scheduling gaps while still
preventing unbounded latency growth.

The 20 ms prebuffer and 30 ms queue are configured buffering, not end-to-end
latency measurements. A ten-second attach test against the paired Sunshine host
decoded 920 Opus packets, inserted 0 ms of silence, and dropped 30 ms at the
bounded ring limit. The CoreAudio device latency property reported 0 ms on that
Mac, so it must not be treated as a physical output-latency measurement. The
startup log reports configured values and final output reports decoded packets,
queued/dropped PCM, silence inserted for underruns, output callbacks, peak
jitter-buffer occupancy, and the CoreAudio device-reported latency.
`--duration SECONDS` provides a bounded attach run for gathering those
diagnostics.

## macOS Wi-Fi stutter

This client cannot prevent macOS from taking over the Wi-Fi radio. Moonlight's
own FAQ says that periodic macOS scans for Location Services and AirDrop delay
or drop real-time stream traffic. Disable AirDrop and Location Services while
testing; for the most reliable result, use Ethernet. Moonlight issue
[#753](https://github.com/moonlight-stream/moonlight-qt/issues/753) contains
Apple Silicon reports that also implicate the `awdl0` interface used by AirDrop
and other Continuity services. The CLI intentionally does not disable `awdl0`
or Bluetooth because that changes system-wide connectivity and requires
administrator privileges.

As a temporary diagnostic, the affected Moonlight reports use
`sudo /sbin/ifconfig awdl0 down` before streaming and
`sudo /sbin/ifconfig awdl0 up` afterwards. This disables AirDrop, Handoff, and
other Continuity functions that depend on AWDL for that period. It is a manual
system setting, not a behavior of this CLI.

For a reproducible network check, Sunshine recommends a 60-second reverse UDP
`iperf3` test from the Mac to the host: `iperf3 -c HOST -t 60 -u -R -b 50M`.
Packet loss should be below 5% and jitter below 1 ms. If the CLI's final
statistics report `silence inserted`, the stream arrived too late for playback;
if they report `dropped (ring full)`, reduce accumulated latency or investigate
the output device.

## Concurrent use and limitations

Moonlight Qt and this CLI can read the same identity concurrently; the CLI is
read-only. `--attach` does not tap into another client’s existing media packets:
it asks Sunshine to create a separate GameStream transport for the active app,
with this CLI's low-resolution video immediately discarded and its audio played.
This therefore still consumes a small extra capture/encoder session. Sunshine's
current source authenticates clients by exact certificate and has an
active-session set, but it has one pending launch event and does not document
two concurrent streams from the same certificate. Test this on the target host
before relying on simultaneous Moonlight Qt and CLI sessions.

Other current limitations are no pairing UI, no reconnect loop, no host
discovery, and no end-to-end host test in this
repository. The `hosts` pairing label means a stored Moonlight pinned-host
certificate exists; a live pairing check happens for `apps` and streaming.
