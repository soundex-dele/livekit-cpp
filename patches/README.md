# Native WebRTC patches

Base: M137, commit `aad183504be13d5a9c529581a27c53767570b5d6` in the adjacent
WebRTC checkout. These are small corrections to the native implementation,
not alternate RTP/RTCP, congestion-control or pacing implementations.

## Pacer TTL after pause

`webrtc-m137-pacer-pause-ttl.patch` fixes a mixed clock domain in the native
`PrioritizedPacketQueue`. Its internal `enqueue_time` subtracts cumulative
paused time for average queue statistics. Its leading enqueue time is used
by scheduling and TTL code that compares against the actual clock. Returning
the adjusted value makes even a 1 ms old packet expire after a 1 s empty pause.
Repeated signaling detach/rebind can accumulate the same failure.

The patch returns the original enqueue timestamp already stored by the native
queue and corrects the TTL log to use that timestamp. Average queue time still
excludes pauses. TTL still expires packets by actual residence time, including
time spent paused; video/RTX limits and native queue ordering are unchanged.

Apply explicitly, with the path to this patch made absolute:

```powershell
git -C D:/workspace/webrtc apply --check --ignore-space-change D:/workspace/livekit-cpp/patches/webrtc-m137-pacer-pause-ttl.patch
git -C D:/workspace/webrtc apply --ignore-space-change D:/workspace/livekit-cpp/patches/webrtc-m137-pacer-pause-ttl.patch
cmake --build D:/workspace/webrtc/out/windows-x64 --config Release --target webrtc -j 8
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
```

For Linux, run the same `git -C <webrtc-root> apply` commands with Linux paths,
then `scripts/build-linux.sh`; its source build rebuilds the native library.
`--ignore-space-change` accommodates a source snapshot with CRLF. Do not apply
twice; `git apply --reverse --check` can verify an already applied patch.

CMake checks the normalized source SHA-256 and fails on a different version.
It never patches the dependency automatically. For prebuilt imports this cannot
prove the library was rebuilt: the `native-send` CTest exercises the linked
native queue and must pass before delivery. Its regression covers an empty
pause, fresh video, audio priority, real TTL expiration, average queue time and
eight repeated pauses. Unpatched Windows and Linux libraries failed the fresh
packet assertion; `out/pacer-pause-before.log` retains the Windows failure.

Any WebRTC upgrade must re-audit this patch and these tests. Do not silently
remove the guard, increase TTLs, or replace the native pacer to make tests pass.

## Refresh congestion cause at constant bitrate

`webrtc-m137-probe-cause-refresh.patch` includes the native bandwidth-limited
cause in `GoogCcNetworkController`'s change detection. In an application-limited
region (ALR), the delay detector can recover while the target bitrate, loss,
RTT and stable rate remain unchanged. Previously the probe controller retained
the old delay-limited cause and rejected periodic ALR probes indefinitely.

The patch forwards actual cause changes through the existing native update
path. It does not manufacture bandwidth, force probes, reset the estimator or
bypass delay/loss/RTT safeguards. Apply and rebuild as above, substituting this
patch filename. Both patches are required; CMake checks both normalized source
files, and `native-send` checks the linked implementation.

The SFU supplies at most half the native target as padding demand when all
media is paused and more video is wanted. This keeps feedback flowing while
leaving room for ALR detection. Actual sending and probing remain controlled
by GoogCC and the native pacer; completed subscriptions request no padding.

The regression sends real MTU-sized RTP over a simulated FIFO link that falls
from 1.5 Mbps to 100 kbps at 10 seconds and recovers at 25 seconds. It checks
video pause, highest-layer recovery, actual video delivery, cold-start time,
probe feedback and audio priority. It also runs without any audio sender.
Oversized synthetic packets are not accepted as a substitute for media load.

Windows validation on 2026-10-27: `out/remote-video-minimal-test.log` (aggregate
workspace) passes all three shaped-link cases. An isolated executable compiled
the same controller with only the new cause-change condition removed, leaving
the shared patched library and source intact. With identical MTU-sized traffic,
the cold-start case stalled at 144155 bps and failed actual-video recovery:
`out/remote-video-legacy-cause-test.log`. This is a simulated network regression,
not physical-device or production-network acceptance.
