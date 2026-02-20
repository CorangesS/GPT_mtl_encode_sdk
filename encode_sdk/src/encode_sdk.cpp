#include "encode_sdk/encode_sdk.hpp"
#include "ffmpeg_utils.hpp"

#include <stdexcept>
#include <iostream>
#include <cstring>
#include <limits>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/timestamp.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace encode_sdk {

static void ff_check(int err, const char* what) {
  if (err < 0) throw std::runtime_error(std::string(what) + ": " + ff_err2str(err));
}

static const char* container_name(Container c) {
  switch (c) {
    case Container::MP4: return "mp4";
    case Container::MXF: return "mxf";
  }
  return "mp4";
}

static const char* audio_codec_name(AudioCodec c) {
  switch (c) {
    case AudioCodec::AAC: return "aac";
    case AudioCodec::MP2: return "mp2";
    case AudioCodec::PCM: return "pcm_s16le";
    case AudioCodec::AC3: return "ac3";
  }
  return "aac";
}

static const char* pick_video_encoder(VideoCodec c, HwAccel hw) {
  // Prefer explicit request; Auto tries nvenc then qsv then software.
  if (hw == HwAccel::NVENC) return (c == VideoCodec::H264) ? "h264_nvenc" : "hevc_nvenc";
  if (hw == HwAccel::QSV)   return (c == VideoCodec::H264) ? "h264_qsv"   : "hevc_qsv";
  if (hw == HwAccel::Software) return (c == VideoCodec::H264) ? "libx264" : "libx265";
  // Auto
  return (c == VideoCodec::H264) ? "h264_nvenc" : "hevc_nvenc";
}

static const char* sw_fallback_encoder(VideoCodec c) {
  return (c == VideoCodec::H264) ? "libx264" : "libx265";
}

struct StreamCtx {
  AVStream* st = nullptr;
  AVCodecContext* enc = nullptr;
  int64_t first_ts_ns = AV_NOPTS_VALUE;
};

class SessionImpl final : public Session {
public:
  static std::unique_ptr<Session> open(const EncodeParams& params) {
    return std::unique_ptr<Session>(new SessionImpl(params));
  }

  ~SessionImpl() override { try { close(); } catch (...) {} }

  bool push_video(const mtl_sdk::VideoFrame& frame) override {
    if (closed_) return false;
    if (!v_.enc) return false;

    // Convert timestamp to PTS in video timebase
    if (v_.first_ts_ns == AV_NOPTS_VALUE) v_.first_ts_ns = frame.timestamp_ns;
    int64_t rel_ns = frame.timestamp_ns - v_.first_ts_ns;
    int64_t pts = av_rescale_q(rel_ns, AVRational{1, 1000000000}, v_.enc->time_base);

    AVFrame* src = wrap_video_frame(frame);
    AVFrame* in = src;

    // If input pix_fmt doesn't match encoder pix_fmt, convert via swscale to encoder_fmt_ (NV12/P010)
    if (src->format != v_.enc->pix_fmt) {
      AVFrame* conv = alloc_video_frame(v_.enc->pix_fmt, v_.enc->width, v_.enc->height);
      sws_scale(sws_.get(),
                src->data, src->linesize, 0, v_.enc->height,
                conv->data, conv->linesize);
      conv->pts = pts;
      in = conv;
      av_frame_free(&src);
      src = nullptr;
      encode_and_write(v_, in);
      av_frame_free(&conv);
    } else {
      in->pts = pts;
      encode_and_write(v_, in);
      av_frame_free(&in);
    }
    return true;
  }

  bool push_audio(const mtl_sdk::AudioFrame& frame) override {
    if (closed_) return false;
    if (!a_.enc) return false;

    if (a_.first_ts_ns == AV_NOPTS_VALUE) a_.first_ts_ns = frame.timestamp_ns;
    int64_t rel_ns = frame.timestamp_ns - a_.first_ts_ns;

    // input: interleaved S16LE in frame.pcm (mock); real ST2110 may be 24-bit
    const int bytes_per_sample = 2;
    const int nb_samples = (int)(frame.pcm.size() / (bytes_per_sample * frame.fmt.channels));
    if (nb_samples <= 0) return false;

    AVFrame* af = av_frame_alloc();
    af->format = a_.enc->sample_fmt;
    av_channel_layout_default(&af->ch_layout, a_.enc->ch_layout.nb_channels);
    af->sample_rate = a_.enc->sample_rate;
    af->nb_samples = nb_samples;
    ff_check(av_frame_get_buffer(af, 0), "av_frame_get_buffer(audio)");

    // resample if needed
    const uint8_t* in_data[1] = { frame.pcm.data() };
    int in_linesize = 0;

    if (swr_) {
      // swr converts from S16 to encoder fmt
      ff_check(swr_convert(swr_.get(),
                           af->data, af->nb_samples,
                           in_data, nb_samples), "swr_convert");
    } else {
      // direct copy (S16LE)
      std::memcpy(af->data[0], frame.pcm.data(), frame.pcm.size());
    }

    int64_t pts = av_rescale_q(rel_ns, AVRational{1, 1000000000}, a_.enc->time_base);
    af->pts = pts;

    encode_and_write(a_, af);
    av_frame_free(&af);
    return true;
  }

  bool close() override {
    if (closed_) return true;
    // flush
    if (v_.enc) flush_stream(v_);
    if (a_.enc) flush_stream(a_);

    if (ofmt_) {
      av_write_trailer(ofmt_);
      if (!(ofmt_->oformat->flags & AVFMT_NOFILE) && ofmt_->pb) {
        avio_closep(&ofmt_->pb);
      }
      avformat_free_context(ofmt_);
      ofmt_ = nullptr;
    }

    if (v_.enc) { avcodec_free_context(&v_.enc); v_.enc = nullptr; }
    if (a_.enc) { avcodec_free_context(&a_.enc); a_.enc = nullptr; }

    if (sws_) sws_.reset();
    if (swr_) swr_.reset();

    closed_ = true;
    return true;
  }

  void set_video_bitrate_kbps(int kbps) override {
    params_.video.bitrate_kbps = kbps;
    need_reconfig_ = true;
  }

  void set_video_gop(int gop) override {
    params_.video.gop = gop;
    need_reconfig_ = true;
  }

  bool apply_reconfigure() override {
    if (!need_reconfig_) return true;
    // Portable implementation: close and reopen output file.
    // For long-running systems you can extend to segment files or in-place reinit.
    auto saved = params_;
    std::string out = params_.mux.output_path;

    close();
    // reopen to a new file name to avoid overwriting partially written file
    saved.mux.output_path = out + ".reconfig";
    auto reopened = SessionImpl(saved); // uses move-assignment pattern
    *this = std::move(reopened);
    need_reconfig_ = false;
    return true;
  }

private:
  struct SwsDeleter { void operator()(SwsContext* p) const { sws_freeContext(p); } };
  struct SwrDeleter { void operator()(SwrContext* p) const { swr_free(&p); } };

  SessionImpl(const EncodeParams& params) : params_(params) {
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    open_output();
    open_video();
    if (params_.audio) open_audio();

    ff_check(avformat_write_header(ofmt_, nullptr), "avformat_write_header");
  }

  // move-only reopen helper
  SessionImpl(SessionImpl&& other) noexcept { *this = std::move(other); }
  SessionImpl& operator=(SessionImpl&& other) noexcept {
    if (this == &other) return *this;
    params_ = other.params_;
    ofmt_ = other.ofmt_; other.ofmt_ = nullptr;
    v_ = other.v_; other.v_ = {};
    a_ = other.a_; other.a_ = {};
    sws_ = std::move(other.sws_);
    swr_ = std::move(other.swr_);
    encoder_fmt_ = other.encoder_fmt_;
    need_reconfig_ = other.need_reconfig_;
    closed_ = other.closed_;
    return *this;
  }
  SessionImpl(const SessionImpl&) = delete;
  SessionImpl& operator=(const SessionImpl&) = delete;

  void open_output() {
    const char* fmt = container_name(params_.mux.container);
    ff_check(avformat_alloc_output_context2(&ofmt_, nullptr, fmt, params_.mux.output_path.c_str()),
             "avformat_alloc_output_context2");
    if (!(ofmt_->oformat->flags & AVFMT_NOFILE)) {
      ff_check(avio_open(&ofmt_->pb, params_.mux.output_path.c_str(), AVIO_FLAG_WRITE), "avio_open");
    }
  }

  void open_video() {
    // Pick encoder. Auto tries NVENC then QSV then SW by probing.
    const char* enc_name = pick_video_encoder(params_.video.codec, params_.video.hw);
    const AVCodec* codec = avcodec_find_encoder_by_name(enc_name);
    if (!codec && params_.video.hw == HwAccel::Auto) {
      // try qsv
      enc_name = (params_.video.codec == VideoCodec::H264) ? "h264_qsv" : "hevc_qsv";
      codec = avcodec_find_encoder_by_name(enc_name);
    }
    if (!codec) {
      codec = avcodec_find_encoder_by_name(sw_fallback_encoder(params_.video.codec));
    }
    if (!codec) throw std::runtime_error("No suitable video encoder found in your FFmpeg build");

    v_.st = avformat_new_stream(ofmt_, nullptr);
    if (!v_.st) throw std::runtime_error("avformat_new_stream(video) failed");

    v_.enc = avcodec_alloc_context3(codec);
    if (!v_.enc) throw std::runtime_error("avcodec_alloc_context3(video) failed");

    // Expect to be fed width/height from first frame; but set safe defaults.
    v_.enc->width = 1920;
    v_.enc->height = 1080;

    v_.enc->bit_rate = (int64_t)params_.video.bitrate_kbps * 1000;
    v_.enc->gop_size = params_.video.gop;
    v_.enc->max_b_frames = 2;

    AVRational fr = AVRational{params_.video.fps_num, params_.video.fps_den};
    v_.enc->time_base = av_inv_q(fr);
    v_.enc->framerate = fr;

    // Choose encoder pixel format:
    // - Most hw encoders accept NV12 (8-bit) and/or P010 (10-bit).
    // - We default to NV12 for maximum compatibility.
    encoder_fmt_ = (params_.video.input_fmt == mtl_sdk::VideoPixFmt::P010) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    v_.enc->pix_fmt = encoder_fmt_;

    if (ofmt_->oformat->flags & AVFMT_GLOBALHEADER) v_.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Apply profile if provided (best-effort; encoder-dependent)
    if (!params_.video.profile.empty()) {
      av_opt_set(v_.enc->priv_data, "profile", params_.video.profile.c_str(), 0);
    }

    // Common rate-control knobs (encoder-dependent)
    av_opt_set(v_.enc->priv_data, "rc", "vbr", 0);

    ff_check(avcodec_open2(v_.enc, codec, nullptr), "avcodec_open2(video)");
    ff_check(avcodec_parameters_from_context(v_.st->codecpar, v_.enc), "avcodec_parameters_from_context(video)");
    v_.st->time_base = v_.enc->time_base;
  }

  void open_audio() {
    const auto& ap = *params_.audio;
    const AVCodec* codec = avcodec_find_encoder_by_name(audio_codec_name(ap.codec));
    if (!codec) throw std::runtime_error("Requested audio encoder not found in FFmpeg build");

    a_.st = avformat_new_stream(ofmt_, nullptr);
    if (!a_.st) throw std::runtime_error("avformat_new_stream(audio) failed");

    a_.enc = avcodec_alloc_context3(codec);
    if (!a_.enc) throw std::runtime_error("avcodec_alloc_context3(audio) failed");

    a_.enc->bit_rate = (int64_t)ap.bitrate_kbps * 1000;
    a_.enc->sample_rate = ap.sample_rate;
    av_channel_layout_default(&a_.enc->ch_layout, ap.channels);
    a_.enc->time_base = AVRational{1, ap.sample_rate};

    // pick sample_fmt
    if (codec->sample_fmts && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
      a_.enc->sample_fmt = codec->sample_fmts[0];
    } else {
      a_.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    if (ofmt_->oformat->flags & AVFMT_GLOBALHEADER) a_.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ff_check(avcodec_open2(a_.enc, codec, nullptr), "avcodec_open2(audio)");
    ff_check(avcodec_parameters_from_context(a_.st->codecpar, a_.enc), "avcodec_parameters_from_context(audio)");
    a_.st->time_base = a_.enc->time_base;

    // If our input is S16 and encoder isn't S16, set up resampler.
    if (a_.enc->sample_fmt != AV_SAMPLE_FMT_S16) {
      SwrContext* swr = nullptr;
      ff_check(swr_alloc_set_opts2(&swr,
                                  &a_.enc->ch_layout, a_.enc->sample_fmt, a_.enc->sample_rate,
                                  &a_.enc->ch_layout, AV_SAMPLE_FMT_S16, a_.enc->sample_rate,
                                  0, nullptr),
               "swr_alloc_set_opts2");
      swr_.reset(swr);
      ff_check(swr_init(swr_.get()), "swr_init");
    }
  }

  AVFrame* wrap_video_frame(const mtl_sdk::VideoFrame& frame) {
    // Update encoder dimensions on first frame if they differ.
    if (v_.enc->width != frame.fmt.width || v_.enc->height != frame.fmt.height) {
      // NOTE: many encoders cannot change resolution mid-stream; in production, segment/reopen.
      v_.enc->width = frame.fmt.width;
      v_.enc->height = frame.fmt.height;
    }

    AVFrame* f = av_frame_alloc();
    // Map mtl_sdk::VideoPixFmt -> AVPixelFormat for the source
    AVPixelFormat src_fmt = AV_PIX_FMT_NV12;
    switch (frame.fmt.pix_fmt) {
      case mtl_sdk::VideoPixFmt::NV12: src_fmt = AV_PIX_FMT_NV12; break;
      case mtl_sdk::VideoPixFmt::P010: src_fmt = AV_PIX_FMT_P010LE; break;
      case mtl_sdk::VideoPixFmt::YUV422_10BIT:
        // No single perfect mapping for RFC4175 packed; you likely convert earlier.
        // Treat as YUV422P10LE placeholder.
        src_fmt = AV_PIX_FMT_YUV422P10LE;
        break;
    }
    f->format = src_fmt;
    f->width = frame.fmt.width;
    f->height = frame.fmt.height;

    // For HostPtr, we point AVFrame planes at user memory (no copy).
    if (frame.mem_type != mtl_sdk::MemoryType::HostPtr) {
      // Placeholder: implement DMABUF/CUDA import here when available.
      throw std::runtime_error("Non-host video memory type not implemented in default encoder");
    }

    f->data[0] = frame.planes[0].data;
    f->linesize[0] = frame.planes[0].linesize;
    f->data[1] = frame.planes[1].data;
    f->linesize[1] = frame.planes[1].linesize;

    // Init sws if conversion needed
    if (!sws_ || sws_src_fmt_ != src_fmt || sws_dst_fmt_ != v_.enc->pix_fmt ||
        sws_w_ != frame.fmt.width || sws_h_ != frame.fmt.height) {
      sws_src_fmt_ = src_fmt;
      sws_dst_fmt_ = (AVPixelFormat)v_.enc->pix_fmt;
      sws_w_ = frame.fmt.width;
      sws_h_ = frame.fmt.height;

      SwsContext* sc = sws_getContext(sws_w_, sws_h_, sws_src_fmt_,
                                      sws_w_, sws_h_, sws_dst_fmt_,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sc) throw std::runtime_error("sws_getContext failed");
      sws_.reset(sc);
    }

    return f;
  }

  AVFrame* alloc_video_frame(AVPixelFormat fmt, int w, int h) {
    AVFrame* f = av_frame_alloc();
    f->format = fmt;
    f->width = w;
    f->height = h;
    ff_check(av_frame_get_buffer(f, 32), "av_frame_get_buffer(video)");
    ff_check(av_frame_make_writable(f), "av_frame_make_writable(video)");
    return f;
  }

  void encode_and_write(StreamCtx& s, AVFrame* frame) {
    ff_check(avcodec_send_frame(s.enc, frame), "avcodec_send_frame");
    while (true) {
      AVPacket* pkt = av_packet_alloc();
      int ret = avcodec_receive_packet(s.enc, pkt);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { av_packet_free(&pkt); break; }
      ff_check(ret, "avcodec_receive_packet");

      // rescale packet timestamps to stream timebase
      av_packet_rescale_ts(pkt, s.enc->time_base, s.st->time_base);
      pkt->stream_index = s.st->index;

      ff_check(av_interleaved_write_frame(ofmt_, pkt), "av_interleaved_write_frame");
      av_packet_free(&pkt);
    }
  }

  void flush_stream(StreamCtx& s) {
    ff_check(avcodec_send_frame(s.enc, nullptr), "avcodec_send_frame(flush)");
    while (true) {
      AVPacket* pkt = av_packet_alloc();
      int ret = avcodec_receive_packet(s.enc, pkt);
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) { av_packet_free(&pkt); break; }
      ff_check(ret, "avcodec_receive_packet(flush)");
      av_packet_rescale_ts(pkt, s.enc->time_base, s.st->time_base);
      pkt->stream_index = s.st->index;
      ff_check(av_interleaved_write_frame(ofmt_, pkt), "av_interleaved_write_frame(flush)");
      av_packet_free(&pkt);
    }
  }

private:
  EncodeParams params_;
  AVFormatContext* ofmt_ = nullptr;
  StreamCtx v_{}, a_{};

  std::unique_ptr<SwsContext, SwsDeleter> sws_{};
  AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
  AVPixelFormat sws_dst_fmt_ = AV_PIX_FMT_NONE;
  int sws_w_ = 0, sws_h_ = 0;

  std::unique_ptr<SwrContext, SwrDeleter> swr_{};

  int encoder_fmt_ = AV_PIX_FMT_NV12;
  bool need_reconfig_ = false;
  bool closed_ = false;
};

std::unique_ptr<Session> Session::open(const EncodeParams& params) {
  return SessionImpl::open(params);
}

} // namespace encode_sdk
