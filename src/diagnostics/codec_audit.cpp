#include "lk/codec_audit.hpp"
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <modules/video_coding/include/video_error_codes.h>

namespace lk {
namespace {
// An SFU negotiates encoded transport capabilities independently of installed
// software encoders. The media channel is disabled before forwarding begins.
// These endpoints never produce media: any accidental encode/decode call fails
// and increments the same audit counters checked by real-client integration.
class ForwardingEncoder : public webrtc::VideoEncoder {
 public:
  explicit ForwardingEncoder(std::shared_ptr<CodecAudit> a):audit_(std::move(a)) {}
  int InitEncode(const webrtc::VideoCodec*,const Settings&) override { return WEBRTC_VIDEO_CODEC_OK; }
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback*) override {return WEBRTC_VIDEO_CODEC_OK;}
  int32_t Release() override {return WEBRTC_VIDEO_CODEC_OK;}
  int32_t Encode(const webrtc::VideoFrame&,const std::vector<webrtc::VideoFrameType>*) override {++audit_->video_encode_calls;return WEBRTC_VIDEO_CODEC_ERROR;}
  void SetRates(const RateControlParameters&) override {}
  EncoderInfo GetEncoderInfo() const override {EncoderInfo info;info.implementation_name="SFU encoded forwarding";return info;}
 private: std::shared_ptr<CodecAudit> audit_;
};
class ForwardingDecoder : public webrtc::VideoDecoder {
 public:
  explicit ForwardingDecoder(std::shared_ptr<CodecAudit> a):audit_(std::move(a)) {}
  bool Configure(const Settings&) override {return true;}
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback*) override {return WEBRTC_VIDEO_CODEC_OK;}
  int32_t Release() override {return WEBRTC_VIDEO_CODEC_OK;}
  int32_t Decode(const webrtc::EncodedImage&,bool,int64_t) override {++audit_->video_decode_calls;return WEBRTC_VIDEO_CODEC_ERROR;}
 private: std::shared_ptr<CodecAudit> audit_;
};
std::vector<webrtc::SdpVideoFormat> forwarding_formats() {
  std::vector<webrtc::SdpVideoFormat> formats{webrtc::SdpVideoFormat("VP8")};
  for (const char* profile:{"42e01f","42001f","4d001f","640c1f"})
    formats.emplace_back("H264",webrtc::CodecParameterMap{{"profile-level-id",profile},{"level-asymmetry-allowed","1"},{"packetization-mode","1"}});
  return formats;
}
class ForwardingEncoders : public webrtc::VideoEncoderFactory {
 public:
  explicit ForwardingEncoders(std::shared_ptr<CodecAudit> a):audit_(std::move(a)) {}
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {return forwarding_formats();}
  std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&,const webrtc::SdpVideoFormat&) override {return std::make_unique<ForwardingEncoder>(audit_);}
 private: std::shared_ptr<CodecAudit> audit_;
};
class ForwardingDecoders : public webrtc::VideoDecoderFactory {
 public:
  explicit ForwardingDecoders(std::shared_ptr<CodecAudit> a):audit_(std::move(a)) {}
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {return forwarding_formats();}
  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment&,const webrtc::SdpVideoFormat&) override {return std::make_unique<ForwardingDecoder>(audit_);}
 private: std::shared_ptr<CodecAudit> audit_;
};
class Encoder : public webrtc::VideoEncoder {
 public:
  Encoder(std::unique_ptr<webrtc::VideoEncoder> inner,std::shared_ptr<CodecAudit> audit)
      : inner_(std::move(inner)),audit_(std::move(audit)) {}
  int InitEncode(const webrtc::VideoCodec* codec,const Settings& settings) override { return inner_->InitEncode(codec,settings); }
  int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) override { return inner_->RegisterEncodeCompleteCallback(cb); }
  int32_t Release() override { return inner_->Release(); }
  int32_t Encode(const webrtc::VideoFrame& frame,const std::vector<webrtc::VideoFrameType>* types) override {
    ++audit_->video_encode_calls; return inner_->Encode(frame,types);
  }
  void SetRates(const RateControlParameters& params) override { inner_->SetRates(params); }
  EncoderInfo GetEncoderInfo() const override { return inner_->GetEncoderInfo(); }
  void SetFecControllerOverride(webrtc::FecControllerOverride* c) override { inner_->SetFecControllerOverride(c); }
  void OnPacketLossRateUpdate(float loss) override { inner_->OnPacketLossRateUpdate(loss); }
  void OnRttUpdate(int64_t rtt) override { inner_->OnRttUpdate(rtt); }
  void OnLossNotification(const LossNotification& loss) override { inner_->OnLossNotification(loss); }
 private:
  std::unique_ptr<webrtc::VideoEncoder> inner_;
  std::shared_ptr<CodecAudit> audit_;
};
class Decoder : public webrtc::VideoDecoder {
 public:
  Decoder(std::unique_ptr<webrtc::VideoDecoder> inner,std::shared_ptr<CodecAudit> audit)
      : inner_(std::move(inner)),audit_(std::move(audit)) {}
  bool Configure(const Settings& settings) override { return inner_->Configure(settings); }
  int32_t Decode(const webrtc::EncodedImage& frame,int64_t time) override {
    ++audit_->video_decode_calls; return inner_->Decode(frame,time);
  }
  int32_t Decode(const webrtc::EncodedImage& frame,bool missing,int64_t time) override {
    ++audit_->video_decode_calls; return inner_->Decode(frame,missing,time);
  }
  int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* cb) override { return inner_->RegisterDecodeCompleteCallback(cb); }
  int32_t Release() override { return inner_->Release(); }
  DecoderInfo GetDecoderInfo() const override { return inner_->GetDecoderInfo(); }
 private:
  std::unique_ptr<webrtc::VideoDecoder> inner_;
  std::shared_ptr<CodecAudit> audit_;
};
class Encoders : public webrtc::VideoEncoderFactory {
 public:
  explicit Encoders(std::shared_ptr<CodecAudit> a) : audit_(std::move(a)) {}
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return inner_->GetSupportedFormats(); }
  std::vector<webrtc::SdpVideoFormat> GetImplementations() const override { return inner_->GetImplementations(); }
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,std::optional<std::string> scalability) const override {
    return inner_->QueryCodecSupport(format,scalability);
  }
  std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,const webrtc::SdpVideoFormat& format) override {
    auto codec=inner_->Create(env,format);
    if (!codec) return nullptr;
    return std::make_unique<Encoder>(std::move(codec),audit_);
  }
 private:
  std::unique_ptr<webrtc::VideoEncoderFactory> inner_=webrtc::CreateBuiltinVideoEncoderFactory();
  std::shared_ptr<CodecAudit> audit_;
};
class Decoders : public webrtc::VideoDecoderFactory {
 public:
  explicit Decoders(std::shared_ptr<CodecAudit> a) : audit_(std::move(a)) {}
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override { return inner_->GetSupportedFormats(); }
  CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format,bool scaling) const override { return inner_->QueryCodecSupport(format,scaling); }
  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,const webrtc::SdpVideoFormat& format) override {
    auto codec=inner_->Create(env,format);
    if (!codec) return nullptr;
    return std::make_unique<Decoder>(std::move(codec),audit_);
  }
 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> inner_=webrtc::CreateBuiltinVideoDecoderFactory();
  std::shared_ptr<CodecAudit> audit_;
};
}
std::unique_ptr<webrtc::VideoEncoderFactory> audited_video_encoders(std::shared_ptr<CodecAudit> a,bool packet_forwarding) {
  if (packet_forwarding) return std::make_unique<ForwardingEncoders>(std::move(a));
  return std::make_unique<Encoders>(std::move(a));
}
std::unique_ptr<webrtc::VideoDecoderFactory> audited_video_decoders(std::shared_ptr<CodecAudit> a,bool packet_forwarding) {
  if (packet_forwarding) return std::make_unique<ForwardingDecoders>(std::move(a));
  return std::make_unique<Decoders>(std::move(a));
}
}
