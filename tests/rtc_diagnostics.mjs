// Self-contained for Playwright page.evaluate; no changes to the connection.
export async function captureRtcDiagnostics() {
  const manager = window.room?.engine?.pcManager;
  const connections = [];
  const seen = new Set();
  for (const role of ['publisher', 'subscriber']) {
    const pc = manager?.[role]?.pc;
    if (!pc || seen.has(pc)) continue;
    seen.add(pc);
    const connection = {role, connectionState:pc.connectionState,
      iceConnectionState:pc.iceConnectionState, iceGatheringState:pc.iceGatheringState,
      signalingState:pc.signalingState};
    try {
      const report = await pc.getStats();
      connection.stats = [...report.values()].filter(s => [
        'transport', 'candidate-pair', 'local-candidate', 'remote-candidate',
        'inbound-rtp', 'outbound-rtp', 'remote-inbound-rtp', 'remote-outbound-rtp',
        'media-source', 'codec',
      ].includes(s.type));
    } catch (error) { connection.error = String(error); }
    connections.push(connection);
  }
  return {capturedAt:Date.now(), identity:window.room?.localParticipant?.identity,
    mode:manager?.mode, roomState:window.room?.state, connections};
}
