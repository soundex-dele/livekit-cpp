# Private RTC platform implementation

## Current status: paused (2026-10-26)

The user requested a status handoff and a pause before further development.
See [功能现状与暂停交接](STATUS_AND_HANDOFF.md) for implemented capabilities,
open regressions, unfinished deliverables, workspace state and resume steps.
The goal is not complete. Historical "active" and "next" statements below describe
their respective increments; they do not authorize continued work during this pause.

This tracks the accepted platform plan, not just the current patch. A checked
item requires build/runtime evidence; declarations and API stubs do not count.

## Required deliverables

- [ ] P0: pinned protocol/dependencies and compatibility matrix; Linux server,
  Android arm64 and iOS arm64 native builds and real-device interoperability.
- [ ] P0: packet-level SFU with no decode/re-encode in the forwarding path,
  independent subscriber RTP state, NACK/PLI and repeatable media validation.
- [ ] P1: SFU simulcast selection, congestion control/pacing, audio priority,
  TURN deployment and network recovery.
- [ ] P1: Windows/Android/iOS C++ SDK, JNI/Kotlin and Objective-C++/Swift wrappers,
  Web SDK interoperability, camera/microphone/screen sources, preview, device
  management, custom PCM/I420/NV12 I/O, bounded media queues and quality APIs.
- [ ] P1: project isolation, tokens/key rotation, quotas, control APIs, admin UI,
  diagnostics, metrics, Linux installation/containers/offline deployment.
- [ ] P1: 2–16 participants per room and 100 concurrent real-media sessions;
  cross-network, weak-network, recovery and 24-hour soak acceptance.
- [ ] P2: RU separate-track recording, timeline manifest, storage and playback;
  MCU audio mixing (mix-minus for interactive audio), video composition, three
  layouts, clock synchronization and H.264/AAC output; composite MP4 recording.
- [ ] P2: LU RTMP/WHIP ingress, RTMP/RTMPS egress and reconnect; independent
  processes, resource quotas, persistent idempotent jobs, leases and webhooks.
- [ ] P3: multi-node room scheduling, drain/rolling upgrades, control HA, task
  rescheduling and independent media-worker scaling.
- [ ] P3: dynacast/SVC, additional codecs, fine-grained policies, AI transcription
  and dialogue, SIP, SRT, E2EE and multi-region interconnection; further desktops
  and cross-platform wrappers.

## Architecture decisions

WebRTC mechanisms must reuse libwebrtc: RTP/RTCP parsing and serialization,
NACK detection/retry, retransmission history, RTX encapsulation/decapsulation,
PLI/FIR, FEC, congestion control and pacing. Application code owns routing,
subscription/layer policy and negotiated SSRC/PT/header-extension mappings.
Do not add parallel handwritten protocol or retransmission implementations.

`livekit-cpp` owns the C++ control plane and SFU/MCU/RU/LU processes.
`livekit-sdk-cpp` owns the common native SDK and platform adapters. Reuse coturn,
FFmpeg, PostgreSQL, S3-compatible storage and established monitoring components.
Production targets Linux; Windows remains supported for development/regression.
Preserve verified LiveKit APIs; use a separate versioned HTTP namespace for
platform management and protobuf/gRPC for internal job control. Initially one
room belongs to one SFU. Workers initially join the SFU over internal WebRTC.

The optional decoded relay is neither an SFU nor an MCU. Native SDK service
clients alone do not implement media workers. Do not mark mobile support based
on CMake presets without compiling and testing the actual platform bindings.

## Acceptance gates

- Normal network: RTT <=50 ms, loss <=0.1%, unconstrained bandwidth; join success
  >=99.5%, join-to-remote-first-frame P95 <=2 s, capture-to-playout P95 <=300 ms.
- Impairment: random 5/10% and burst loss, 150 ms RTT, jitter, bandwidth collapse;
  verify layer reduction/audio priority and no >2 s continuous audio stall in
  the 10% random-loss case.
- Recovery: Wi-Fi/cellular change, blocked UDP, TURN-only and 5 s outage;
  media recovers within 10 s P95 after connectivity returns, no reconnect loop
  after removal/revocation.
- Capacity: 100 real media sessions, including 16-person rooms; each publisher
  supplies 720p plus 180p layers, each receiver selects up to four videos and
  all other audio. Record exact hardware, bitrate, CPU, memory and egress.
- Soak: 24 h at target load, no crashes/deadlocks/growing resource leaks, cleanup.
- Workers: timestamped AV within 100 ms over 2 h; layout/track changes, disk and
  upload failures, process loss; restarting creates marked discontinuities.
- Security: cross-project access, expired tokens, unauthorized subscription,
  duplicate jobs, webhook replay, secret-redacted logs.
- Delivery: clean Linux install, actionable errors, backup restore, upgrade
  rehearsal; SDK artifacts, examples, documentation and measured reports.

## Initial inspection (2026-10-23)

Server worktree clean at start. SDK has pre-existing edits to demo/media code;
preserve them. Adjacent WebRTC CMake manifest currently accepts only Windows and
Linux x64; arm64/mobile support is an outstanding implementation dependency.

## Implemented increment: native packet forwarding (2026-10-23)

- Default `sfu` mode uses native RTP demux and SRTP transports, with no video
  Encode/Decode calls during the full browser regression; `relay` is optional.
- Uses RTPSender/RtpPacketHistory/PacketSequencer, RTCPReceiver,
  ModuleRtpRtcpImpl2, ReceiveStatistics, NackRequester and RtxReceiveStream.
  Sender reports preserve upstream RTP/NTP association with subscriber counters.
- One ReceiveSideCongestionController per publisher transport supplies native
  TWCC/REMB feedback for all its audio/video/RTX; extensions are identified by
  the negotiated source map. Subscriber transport sequence IDs are not copied.
- First codec contract is Opus/VP8/RTX, set with native CodecPreferences.
  RED/ULPFEC and additional codecs are excluded until native adapters are added.
- Windows Release build, 2 CTests, full browser integration and both existing
  native SDK CLI integrations passed. See VALIDATION.md for measured evidence.
- Remaining after that increment: native subscriber pacing/congestion control, downstream bitrate
  allocation, simulcast selection, larger-room capacity/weak-network/soak,
  Linux and mobile builds, and every other P0–P3 gate above. The retransmission
  limiter is currently a fixed 2 Mbps per sender, not adaptive congestion control.
  Native packet-history retention uses its culling policy with target size 512;
  this is not a hard 512-packet memory limit. Full platform goal remains open.

## Implemented increment: native downstream control (2026-10-23)

- A NativeSendTransport per subscriber transport shares PacingController,
  GoogCcNetworkControllerFactory and TransportFeedbackAdapter across audio,
  video, RTX and probes. Socket sent notifications supply actual send timestamps.
- Native audio priority, initial/repeated/ALR probes and receiver TWCC/REMB
  update the pacer. New per-hop transport sequence numbers and absolute-send-time
  extensions are generated at send; RTX sequences are assigned at actual send.
- ICE/SRTP route changes update native transport history and network constraints;
  unavailable transports pause, mute/removal cancel queued packets, teardown
  unsubscribes callbacks and stops repeating tasks on the owning network thread.
- The native retransmission limiter follows the estimated transport target.
  Native queue TTL is 500 ms for video, 250 ms for video retransmission and 150 ms
  for audio retransmission. Application media admission is bounded to 4 MiB per
  transport. Overflow packets are accounted separately; native padding/probing
  and native retransmission history have their own lifetime policies.
- Deterministic native tests cover pacing over time, audio priority, shared hop
  sequence space, real RTCP feedback reaching GoogCC, bitrate reduction, paced
  RTX, queue admission, mute and destruction. Browser regression requires actual
  downstream feedback and zero queued data/controllers after deleting rooms.
- Outstanding: subscriber bandwidth-to-layer allocation, simulcast/SVC,
  congestion-driven frame admission/recovery, meaningful real weak-network and
  long-disconnection audio freshness acceptance, capacity/soak, Linux/mobile and
  all other platform deliverables. Initial/min/max rates are currently internal
  defaults (300 kbps / 30 kbps / 20 Mbps); deployment configuration is still needed.

## Implemented increment: native encoded-frame forwarding (2026-10-23)

- VP8 uses the native RtpVideoFrameAssembler (including native PacketBuffer and
  reference finder) after native receive/NACK/RTX, then RtpPacketizerVp8 for each
  subscriber. Encoded frame bytes remain unchanged; there is no decoder/encoder.
  Audio retains the packet forwarding path. RTP padding reaches native assembly.
- Subscriber picture IDs and sequence numbers are continuous; initial/resumed
  subscriptions and source changes require keyframes. Dependencies must have
  been admitted for this subscriber. The bounded dependency set is not proof
  of delivery; downstream native NACK/PLI still handles delivery failures.
- Native RTP accessors preserve orientation across assembly. Source timestamp
  mapping handles backwards/large clock changes and wrap; sender reports apply
  the same mapping. Real cross-layer AV synchronization is not yet validated.
- The 4 MiB application admission budget accepts/rejects a whole frame batch.
  Rejection consumes no outgoing frame sequence/picture IDs, requests a keyframe
  and gates dependent frames. Native pacer TTL/socket losses can still happen
  after admission, so this is not complete congestion-driven recovery.
- Unit coverage includes missing fragment/RTX, out-of-order assembly, byte-for-
  byte preservation, rotation/dimensions, source switch keyframe gating, missing
  dependencies, clock wrap/SR mapping, mute/resume and whole-frame admission.
- Outstanding: source-layer/RID discovery, subscriber quality/size/priority
  signaling, bandwidth allocation/hysteresis and actual simulcast switching.
  The frame sender is ready for multiple input SSRCs, but the live SFU still
  binds one source SSRC per track. No simulcast completion claim is made.
- Full-frame buffering adds latency and copies; measure capture-to-playout,
  memory and CPU at the acceptance load before considering this production-ready.
- Functional regressions pass, but the latest four-track CLI run has a sampled
  expected queue-time peak of 1568 ms and only 72/72 decoded video frames (prior
  packet-path baseline 108/109). Investigate native probing/demand signaling and
  allocation before treating this as a performance-complete increment. Evidence:
  tests/results/sdk-cli-integration/20261023-190201/metrics.jsonl.

## Implemented increment: deterministic native probing initialization (2026-10-23)

- Root cause of the preceding regression: M137 applies its initial stream
  configuration inside the first OnProcessInterval, after ResetConstraints.
  If ICE is already writable, initial probing starts before repeated-probing
  is enabled, leaving no retry window after a failed first probe. Native info
  logs reproduced this path; the diagnostic run also failed the existing file
  recording threshold (20261023-190656), without weakening that assertion.
- The adapter now consumes the controller's initial configuration while its
  network availability is still false, before scheduling tasks or binding ICE.
  All probing, estimation and pacing remain native; no WebRTC source change or
  forced target bitrate was introduced. A simulated-clock regression makes the
  transport writable before the first scheduled task, withholds receiver
  feedback, and requires the native controller to issue another probe cluster.
- New metrics distinguish created probe clusters, sent probe packets and probe
  packets reported received. LK_WEBRTC_LOG=info enables native diagnosis; normal
  operation keeps error-level logging.
- CTest 3/3, browser 8 groups and both SDK CLI integrations pass. Final normal-
  logging four-track run: sampled queue peak 30 ms / 8484 bytes, video 99/108
  frames, 2322 probe packets matched with received feedback. The diagnostic
  run also passed (10 ms, 102/104 video frames). These replace the preceding
  1568 ms/72-frame result for this fixture, not the platform-wide latency gate.
- CLI regression now requires at least 80 decoded frames per video in its
  120-frame local fixture, sampled expected queue time below 500 ms, actual probe
  feedback and zero server video Encode/Decode calls. It writes media-evidence.json
  with the tested binary hash, decoded track counts and sampled peaks.
- Next: negotiated simulcast/RID discovery, per-subscription quality settings,
  shared bandwidth allocation with audio reservation and priority/hysteresis;
  all remaining P0–P3 acceptance items still apply.

## Implemented increment: negotiated VP8 simulcast selection (2026-10-23)

- Sources now own separate native receive/NACK/RTX/assembly state per primary
  SSRC, with FID or negotiated RID/repaired-RID association, capped at three
  layers. Dynamic SSRCs bind only to negotiated RIDs; RTX before a primary RID
  is known is ignored rather than guessed. Ordinary renegotiation preserves
  layer state when the negotiated configuration is unchanged.
- Native M137 needs two SFU adaptations: default parameters for an unsignaled
  receiver omit the extension map, so use the accepted native JSEP content;
  ordinary PeerConnection answers only negotiate send simulcast, so explicitly
  accept offered receive RIDs in native JSEP objects before serialization.
  Neither adapter parses/writes RTP/SDP syntax by hand or changes WebRTC sources.
- TrackInfo retains validated VP8 layer metadata. TrackSettings quality and
  dimensions reach SFU outputs and persist across unsubscribe/resubscribe;
  departure/unpublish removes stored policies. Unknown track settings do not
  grow state. Nonzero fps/priority requests receive UNSUPPORTED_TYPE without
  partially applying the request. Metadata update signals remain outstanding.
- Each output chooses independently, continues the previous layer while
  waiting for the selected layer's keyframe, and forwards SR only from its
  active layer with the same timestamp mapping used for RTP. Native frame IDs,
  dependencies and packetization preserve subscriber sequence continuity.
- Dimension selection uses native VP8 frame-header dimensions, choosing the
  smallest sufficient available layer. Quality follows negotiated low-to-high
  order (official LiveKit SDK q/h/f); policy includes one/two-layer fallback.
  Layers with no complete frame for two seconds become unavailable, independent
  of RTX padding; recovery can switch back on a keyframe.
- Browser integration now has nine groups and actually publishes 180p/360p/720p
  encodings. It verifies independent LOW/HIGH subscriptions, decoded dimensions,
  unchanged subscriber SSRC, continued audio, dimension settings surviving
  resubscription and high-layer pause/fallback/resume. Encode/Decode audit stays
  zero. Native/core tests cover policy limits, metadata and settings cleanup.
- Still outstanding: measured-rate/GoogCC budget allocation, audio reservations,
  priority/hysteresis, temporal/SVC selection, dynacast, source SSRC restart
  within the same RID without renegotiation, metadata updates, weak-network,
  capture-to-playout and AV-sync acceptance. Native SDK multi-layer publishing
  APIs/mobile platform work remain separate deliverables, not inferred from
  the browser interoperability test. All remaining P0–P3 gates still apply.
- Final evidence: CTest 3/3, browser 9 groups, SDK CLI media-evidence.json under
  20261023-193451 (98/102 video frames, 16 ms sampled queue peak), stream result
  20261023-193515 (246 full/160 selected video frames). Manual LOW-to-HIGH and
  HIGH-to-dimension observations were 120/351 ms in this browser run; initial
  high-layer observation took 2145 ms. No P95 latency gate is claimed.

## Implemented increment: shared subscriber media allocation (2026-10-23)

- One network-thread allocation task per subscriber transport uses the native
  GoogCC target every 200 ms. It assigns 85% to media, reserves at least 64 kbps
  per enabled audio track (or its higher observed demand), admits video base
  layers in priority order, then upgrades. Explicit TrackSettings priority 1 is
  highest; unset tracks follow explicit priorities in subscription order. An
  explicit priority is now supported; nonzero fps is still rejected atomically.
- Source rates use native RateStatistics with a 2-second window, unique complete
  encoded frames and estimated per-packet overhead. Initial demand comes from
  dimensions/quality. Measurement does not count duplicate assembled RTX/probe
  payload as new encoded video. It is source demand, not network estimation.
- The allocator reduces layers without an extra delay and requires 15% extra
  headroom sustained for one second before upgrades or congestion recovery.
  First admission of an affordable base layer is immediate; waiting for source
  discovery must not trigger recovery hysteresis. Quality and dimensions remain
  caps; the allocator can choose a lower layer or pause an unaffordable video.
  The first discovered keyframe can trigger allocation immediately, rather than
  losing it to the periodic tick and incurring another PLI round trip. A newly
  configured sender already knows its source generation; do not reset its
  native queue again on the first frame. Resume retains pending native padding.
- Bandwidth pause clears native queued media/history and requires a keyframe on
  resume, while retaining native padding. During a downshift, the old layer can
  continue only if its demand fits the new allocation. User mute also stops
  padding. All packet construction, feedback, pacing and probing remain native.
- The padding demand limit reaches native StreamsConfig. Merely retaining a
  padding callback was insufficient: in a serial-link test, audio stayed too
  close to the collapsed estimate for ALR probing, and video failed to recover.
  Native max_padding_rate supplies traffic up to the native target while higher
  layers are requested. There is no application bandwidth setter/probe scheduler.
  Padding is requested only when admitted demand is at least 60% of the native
  target; otherwise leave spare capacity visible to native ALR detection. Filling
  all spare capacity suppressed ALR and regressed the short SDK four-track test.
  Do not populate max_total_allocated_bitrate from unadmitted source demand:
  M137 treats nonzero allocation as the end of repeated initial probing. A
  regression sets a padding demand before network availability and requires
  repeated native initial probes with no receiver feedback.
  The simulated test also aligns global TimeMillis with Environment::clock:
  M137 AlrDetector uses the former while ProbeController uses the latter.
- Metrics expose media/audio/video allocation, current unallocated video count
  and cumulative bandwidth pauses. Tasks/weak outputs are removed on teardown.
- CTest covers conservation with changing rates/priorities, audio reservation,
  initialization, downgrade/resume and oscillation; native tests check queue
  cancellation, padding-only probing, keyframe resume and real REMB/TWCC effects.
  A 60-second simulated FIFO link changes 1.5 Mbps -> 100 kbps -> 1.5 Mbps,
  including 250 ms tail-drop buffering and 25 ms propagation. Native TWCC drives
  high-layer allocation, pause and recovery. See VALIDATION.md for the latest
  target, packet loss and audio packet reception-gap measurements. No decoded
  video/acoustic quality is claimed by this test.
- Full P0-P3 gates remain open. Outstanding include physical/browser weak-network
  decoding and AV-sync acceptance, TURN/recovery, capacity/soak, deployment rate
  configuration, mobile/Linux builds and the other platform/worker deliverables.
- Final binary 87692c3c0fe3a51a8a3e1cb582a7f174cc4611d2a1f089ffdde59c1f146f2482:
  CTest 3/3, browser 9 groups, SDK four-track 103/102 video frames with sampled
  queue peak 5 ms (20261023-200645), stream 140 full/104 selected video frames
  (20261023-200744). The stream frame count is below the prior 246-frame run;
  cold-start/allocation performance remains follow-up work. Browser manual
  LOW-to-HIGH took 1226 ms and inactive-high fallback observation took 3755 ms;
  these individual observations do not establish a P95 latency gate.

## Implemented increment: native scheduling and startup diagnostics (2026-10-23)

- Windows server and SDK explicitly inject the existing libwebrtc Windows task
  queue factory. The pinned external CMake manifest selected stdlib queues;
  probe timelines showed coarse wakeups and incomplete probe clusters. Compile
  the unchanged native task_queue_win.cc in the application build. One shared
  SFU timer queue wakes the owning network queue; native task-safety flags guard
  delayed callbacks. RTP/pacing/probe decisions remain entirely native.
- Defer transport availability until an RTP sender is attached. Starting native
  padding probes without any sender latched the pacer's pre-first-packet failure
  state, deadlocking video-only subscriptions waiting for bandwidth. A native
  regression connects first, waits 600 ms, then attaches a paused video sender
  and requires successful padding-only probing. SRTP readiness is also checked
  independently of whether video has been allocated.
- Direct fallback when a selected layer disappears. Budget-paused video can
  resume after 200 ms only if half the available video budget can still satisfy
  its target. Ordinary recovery/upgrades retain 1 s hysteresis; changing either
  candidate or headroom class restarts the timer. Conservation and oscillation
  tests cover both paths. A separate simulated cold-start case admits a base
  video above the initial estimate using native padding/feedback.
- SDK optionally emits asynchronous native RTCStats reports at 200-60000 ms,
  disabled by default, at most one request outstanding per connection. Reports
  retain native fields and run through the session event dispatcher. No raw
  candidate or certificate records are exposed. Integration evidence records
  source encoding, receiver decoding and native quality limitations.
- Windows CTest 3/3, SDK CTest 7/7, browser 9 groups, and both CLI integrations
  passed with the original thresholds. Final four-track video 101/104 frames;
  stream full/selected 300/204 frames. SFU video encode/decode calls remain zero.
  See VALIDATION.md for binary hashes, failures encountered and exact evidence.
- Linux configuration/build support now includes distribution Boost/libvpx,
  the PFFFT libc feature macro and pinned dav1d 1.5.3 bootstrap with SHA-512
  validation. Ubuntu 22.04's dav1d 0.9.2 lacks required native API; fail early
  during CMake configuration when those headers are selected. Native compilation
  is continuing in WSL's Linux filesystem. Linux build/runtime/interoperability
  gates remain open, as do all other unfinished P0-P3 deliverables above.

## Implemented increment: Linux build and cross-system evidence (2026-10-23)

- Ubuntu 22.04 x64 native ELF build and CTest 3/3 now pass with Clang/libc++ 15,
  pinned dav1d 1.5.3, distribution Boost/libvpx and the unchanged WebRTC sources.
  Application CMake scopes HAVE_UNISTD_H away from AOM NASM, retains PFFFT's
  glibc feature macro, and selects Asio's invoke_result for Boost 1.74/C++20.
  One shared_ptr lambda uses an init-capture for Clang 15 compatibility.
- Added commit-checked Linux dependency bootstrap with offline mirror support,
  hashed dav1d bootstrap, compiler/header checks and the compatibility matrix.
- External-server modes retain browser and native CLI media assertions. The WSL
  driver owns its server process, records the ELF/hash/runtime libraries and
  shutdown status, and collects all three suites even if an earlier suite fails.
- Linux cross-system acceptance is NOT complete. Run 20261023-215424 passed the
  native stream suite (168 full / 92 selected video frames), but browser HIGH
  simulcast and four-track recording failed. Linux exited with status zero.
  Earlier LOW simulcast failures and native info logs are retained as evidence.
- Read-only guest/host clock diagnostics measured 10.017359 s guest versus
  10.926949 s host (9.08%). Kernel logs reported a >11% TSC adjustment; native
  GoogCC logs reported delay growth limiting probes. This is a diagnostic lead,
  not proof that every media failure is environmental. Temporary clocksource
  comparisons did not solve the discrepancy; original tsc was restored. No
  protocol algorithm, production clock implementation or media gate was changed.
- A temporary tick=10000 comparison also failed all three suites, including the
  previously passing stream suite (20261023-220100). Original tick=9167 was
  restored. RAW time matched the host while MONOTONIC differed; clock calibration
  is not yet a proven fix. Do not claim stable Linux media based on one pass.
- See VALIDATION.md for Windows regression and Linux evidence. Stable-clock
  Linux interoperability, mobile, TURN/recovery, capacity/soak and all remaining
  platform/worker requirements remain open. The full P0-P3 goal is not complete.

## Implemented increment: Linux-local and TURN recovery gates (2026-10-23)

- The unchanged Linux ELF now passes all nine browser integration groups with
  Linux Chrome 156.0.8078.47/Node 22.16.0, including real audio/video, independent
  VP8 simulcast subscriptions, permission changes and teardown. Added a Linux
  browser entry script, explicit Chrome override, platform/version evidence and
  owned-server shutdown verification. Windows browser regression still passes.
- Added a Linux coturn driver with separate UDP and TCP processes/configurations,
  random test credentials, binary hashes and terminal status. Both modes run
  all media assertions with relay-only policy and verify actual selected relay
  candidates on five active connections. TCP disables the TURN UDP listener.
- Added optional five-second opaque UDP dropping ahead of TURN. No RTP parsing,
  repair, buffering or retransmission exists in this test proxy. The test proves
  packets were dropped in both directions, video stalled, then decoded video and
  non-silent audio recovered within ten seconds. Raw monotonic test time avoids
  the known guest clock adjustment; production libwebrtc clocks are unchanged.
- Final raw-clock UDP sample recovered in 1301 ms after 594 upstream and 661
  downstream packets were dropped. Earlier local recovery samples are retained.
  This does not satisfy the complete P95, random/burst loss, handover, public NAT,
  TURN/TLS, credential rotation, capacity or soak requirements. Cross-system
  Windows/WSL failures remain open despite successful Linux-local tests.
- See docs/NETWORK_VALIDATION.md and VALIDATION.md. This increment provides
  reproducible local acceptance tooling, not production coturn deployment or
  completion of any composite P0-P3 checklist item above.

## Implemented increment: coturn REST session credentials (2026-10-23)

- Optional rtc.turn_rest configuration issues expiry:ParticipantSID usernames
  and coturn-compatible Base64 HMAC-SHA1 passwords. Secret sources are mutually
  exclusive inline/environment configuration; TTL/type/overflow checks reject
  invalid configuration. Native WebRTC validates TURN URLs, and OpenSSL signs.
  Static ICE entries remain supported. Both native peers and JoinResponse use
  the session credentials; the shared signing secret is not serialized.
- Windows and Linux builds/CTest 4/4 pass, including independent signing vectors
  and JoinResponse isolation. Windows browser nine groups and both native CLI
  suites pass on the new binary; see VALIDATION.md for hashes and media counts.
- Real coturn REST UDP 12 groups/TCP 11 groups pass in 20261023-224659. Actual
  browser configuration is independently verified in Node; wrong-password and
  expired credentials return 401 with no relay candidate. Master-secret scans
  pass for logs/browser artifacts. The five-second outage sample recovered in
  915 ms raw time; processes stopped without forced termination.
- Two earlier REST runs failed media assertions: TCP inactive-high fallback
  with publisher bandwidth limitation, and UDP resubscription with receiver
  packet loss/NACK despite all source layers encoding. These intermittent
  failures are retained and unresolved; the final pass is not a stability fix.
- Static-credential regression 20261023-224846 passes TCP ten groups but fails
  UDP 720p: publisher high-layer framesEncoded=0 with bandwidth limitation,
  while low/middle layers decode. No impairment proxy was active. The failure
  is not exclusive to REST authentication; this run's aggregate result is false.
- Credentials refresh only on initial/full join. Online refresh, immediate
  revocation, coordinated hot secret rotation, allocation refresh across expiry,
  TURN/TLS/public deployment and long-call acceptance remain open. This is one
  part of the network/control-plane scope, not completion of P1 or the full goal.

## Implemented increment: native pacer pause/TTL correction (2026-10-26)

- A deterministic regression reproduced on both original Windows/Linux native
  libraries: pause an empty queue for 1 s, resume, enqueue two packets 1 ms
  apart; the first fresh packet was discarded with a 500 ms TTL. Native M137
  returned an enqueue time adjusted by cumulative pauses to scheduling/TTL
  code comparing against the actual clock. Repeated signaling detach/rebind
  eventually expired almost every new video packet, independently of bandwidth.
- A minimal native patch returns the original timestamp already retained in
  the queue, also fixing its expiry log. Average queue statistics still exclude
  pauses; actual TTLs, priorities, NACK/RTX and congestion algorithms are retained.
  This is a correction inside libwebrtc, not a parallel pacing implementation.
  The explicit patch and build instructions live in patches/. CMake checks the
  patched source hash; native-send checks the linked binary, including prebuilt
  imports. Dependency upgrades require re-auditing the patch and the regression.
- The regression additionally verifies audio priority, real TTL expiration,
  average queue time and eight repeated pauses. Both platforms pass CTest 4/4.
  Before-patch failure is retained in out/pacer-pause-before.log.
- Separate tracing confirmed disabled default PeerConnection media channels
  still emitted periodic RTCP, including transport feedback. The SFU now removes
  their native send interfaces after negotiation, retaining BaseChannel's
  ICE/DTLS/SRTP transport. This ownership correction alone did not fix the
  stall: channel-ownership, nack-trace and pacer-trace failures remain recorded.
- Browser regression supports LK_RESUBSCRIBE_ROUNDS (1..20, default 1) and
  requires existing audio/video to progress after each resubscription. With the
  corrected production library, pacer-ttl-fixed/20261026-083512 passes TURN REST
  UDP 12 groups/TCP 11 groups, five resubscriptions per transport and cleanup.
  Five-second outage recovery is 977 ms raw time. Earlier separate publisher
  bandwidth-limitation failures and the full latency/weak-network/capacity/soak
  gates must not be declared solved solely from this local run.
- Windows browser also passes five resubscriptions. Cross-system retest
  pacer-ttl-fixed-cross/20261026-083730 still fails: LOW timeout, four-track video
  108/40 frames, and native stream join HTTP 401 before media validation. Keep
  cross-system, fast high-layer recovery and all broader acceptance gates open.
- Windows CLI stream regression passes, but the file suite now records 108/78
  video frames against the unchanged >=80 gate, with a sampled 6 ms queue peak
  and no socket/admission drops. Preserve pacer-ttl-fixed-cli/20261026-084007 as
  a failed regression; inspect remaining publication startup/allocation timing
  rather than lowering the gate. The full platform objective remains active.
