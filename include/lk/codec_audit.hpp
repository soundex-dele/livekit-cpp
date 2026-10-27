#pragma once
#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <atomic>
#include <memory>

namespace lk {
struct CodecAudit {
  std::atomic<uint64_t> video_encode_calls{0},video_decode_calls{0};
};
std::unique_ptr<webrtc::VideoEncoderFactory> audited_video_encoders(std::shared_ptr<CodecAudit>,bool packet_forwarding=false);
std::unique_ptr<webrtc::VideoDecoderFactory> audited_video_decoders(std::shared_ptr<CodecAudit>,bool packet_forwarding=false);
}
