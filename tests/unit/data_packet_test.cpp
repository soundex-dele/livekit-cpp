#include "lk/data_packets.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

std::string nonce(int n) {
  std::string result(16, '\0');
  result[0] = char(n);
  result[1] = char(n >> 8);
  return result;
}

void run() {
  lk::UserPacketDeduper cache;
  check(cache.duplicate(nonce(0)), "Go's initial zero nonce sentinel was lost");
  for (int i = 1; i <= 100; ++i)
    check(!cache.duplicate(nonce(i)), "New nonce incorrectly rejected");
  check(cache.duplicate(nonce(1)), "Oldest nonce must remain in the 100-entry window");
  check(!cache.duplicate(nonce(101)), "New nonce failed at capacity");
  check(!cache.duplicate(nonce(1)), "Duplicate lookup incorrectly refreshed FIFO order");
  check(!cache.duplicate("bad") && !cache.duplicate("bad"), "Invalid UUID nonce was cached");
  check(!cache.duplicate(nonce(0)), "Zero sentinel was not evicted");
  uint32_t sequence = 0;
  livekit::DataPacket packet;
  packet.mutable_user();
  packet.set_sequence(12);
  check(lk::prepare_client_data(packet, "PA_a", "a", false, false, true, sequence),
        "First sequence rejected");
  check(!lk::prepare_client_data(packet, "PA_a", "a", false, false, true, sequence),
        "Duplicate sequence accepted");
  packet.set_sequence(11);
  check(!lk::prepare_client_data(packet, "PA_a", "a", false, false, true, sequence),
        "Older sequence accepted");
  packet.set_sequence(0);
  check(
      lk::prepare_client_data(packet, "PA_a", "a", false, false, true, sequence) && sequence == 12,
      "Legacy packet reset sequence");
  packet.set_sequence(2);
  check(
      lk::prepare_client_data(packet, "PA_a", "a", false, false, false, sequence) && sequence == 12,
      "Lossy packet changed reliable cursor");
  packet.set_sequence(13);
  packet.mutable_transcription();
  check(
      !lk::prepare_client_data(packet, "PA_a", "a", false, false, true, sequence) && sequence == 13,
      "Filtered packet did not advance receive cursor");
  packet.Clear();
  packet.mutable_user()->add_destination_sids("PA_c");
  packet.mutable_user()->add_destination_identities("stale");
  packet.add_destination_identities("b");
  lk::normalize_data_destinations(packet);
  check(lk::data_destination_matches(packet, "b", "PA_b") &&
            lk::data_destination_matches(packet, "c", "PA_c"),
        "SID/identity union broken");
  check(!lk::data_destination_matches(packet, "stale", "PA_stale"),
        "Stale legacy identity overrode envelope");
}
}  // namespace

int main() {
  try {
    run();
    std::cout << "PASS data sequence, routing and bounded nonce cache\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
