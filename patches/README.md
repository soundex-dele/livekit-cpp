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
