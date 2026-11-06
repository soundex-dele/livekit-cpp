# AV control-plane synthetic media smoke

`av_media_control_smoke.mjs` uses the actual Go API, worker and C++ SFU. It obtains
all RTC credentials through the Go APIs and connects headless Chrome clients.
Synthetic sources are a changing 320×240 canvas encoded as VP8 and a 440 Hz
oscillator encoded as Opus. Every audio/video path must decode more than five
frames and receive non-silent audio; both counters must continue increasing.

| Business | Actual automated media path |
| --- | --- |
| Screen cast | Paired guest sender → signed-in receiver, screen video and audio |
| Conference | Waiting guest cannot connect; admitted guest ↔ host, camera video and microphone audio |
| Live studio | Program publisher → anonymous viewer; host ↔ approved member on a separate stage, with stage isolation from viewers |
| Remote assist | Approved host screen → controller; authenticated synthetic key message → recording-only host → acknowledgement |

Each scenario ends or revokes its resource through the business API, checks that
fresh unexpired media JWTs are rejected, and observes all participating media
clients disconnect. Assist renews its host lease every three seconds. Its input
fixture only records and echoes protocol bytes; it has no OS input sink.

Install the existing `tests/package.json` dependencies and provide Chrome (or set
`LK_CHROME` to a Chromium executable). The process harness must supply:

| Environment variable | Meaning |
| --- | --- |
| `AV_SMOKE_API` | First running Go API URL |
| `AV_SMOKE_API2` | Second replica URL; defaults to the first |
| `AV_SMOKE_TOKEN` | Authenticated Foundation bearer session |
| `AV_SMOKE_ORGANIZATION` | Initialized AV organization ID |
| `AV_SMOKE_MEMBER_TOKEN` | Second signed-in organization member for the interactive live stage |
| `AV_SMOKE_ADMIN_TOKEN` | Optional platform-admin session for explicit non-participant governance |
| `AV_SMOKE_OUTPUT` | JSON report path; defaults to `tests/results/av-control-media.json` |

Run `node livekit-cpp/tests/av_media_control_smoke.mjs` from the workspace, or use
`livekit-server/tests/control_plane_integration.py --media-script` to manage the
real services and pass these variables without command-line credentials. The
harness provisions the second member. Without `AV_SMOKE_MEMBER_TOKEN`, the report
explicitly marks interactive stage coverage absent; the four core paths still run.

Add `--admin-governance` to the process harness to exercise the administrative
end/revoke routes. It creates resources under a separate organization and supplies
a platform administrator who never joins that organization or its media sessions.
Without the optional admin token, the original participant actions remain in use.
The administrative run is recorded in `livekit-server/out/admin-media-integration.json`;
all six media assertions and owned-process cleanup passed with Chromium headless
shell 153.0.8010.12 on 2026-11-06.

API requests run in Node; the browser only serves the local SDK page and connects
to the API-provided media URL. No camera, microphone, desktop automation or capture
permission is used. The test heartbeats the business participants until stop,
cleans up the resource and synthetic tracks, and redacts credentials from reports.
Reports are checkpointed after each assertion and cleanup stage. Browser contexts,
the owned browser process, pending heartbeats and the local HTTP server have bounded
cleanup; an unrecovered timeout records the failing step and makes the fixture fail. Media
assertion success is recorded separately from successful overall teardown.
For the owned Chromium process, cleanup observes the actual process `exit` event
or exit status, allows a bounded native-kill fallback, and then releases inherited
stdio streams only after exit. This handles Windows cases where Playwright waits
for a delayed `close` event. The report records PID, exit code/signal, recovery
method and driver cleanup completion; an unrecovered process or cleanup timeout
still fails the fixture.
This verifies automated local media paths. It does not constitute actual-device,
public TURN, remote-input injection, or capacity acceptance.

Verified through `control_plane_integration.py` with two real Go API processes,
the worker, Redis, temporary SQLite, the AV-mode C++ SFU and Chrome 156.0.8078.81:

| Scenario | Decoded video progression | Stop/revoke to all clients disconnected |
| --- | --- | --- |
| Cast | 7 → 12 frames | 565 ms |
| Conference | Host 6 → 11; guest 21 → 27 | 525 ms |
| Live program and stage | Viewer 6 → 11; stage host 8 → 13; stage member 23 → 28 | 933 ms |
| Assist | Controller 6 → 11; authenticated input recorded and echoed | 392 ms |

All audio paths had increasing sample counts and nonzero energy. All four
scenarios rejected credential reissue with HTTP 403 and unexpired JWT validation
with HTTP 401. These are local test observations, not latency service guarantees.
The detailed report is `livekit-server/out/control-plane-integration.json`.
The final run also completed every fixture cleanup step, observed the owned
browser exit with code 0, and removed the harness temporary directory without
process cleanup errors.
