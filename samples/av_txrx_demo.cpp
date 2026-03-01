// 音视频收发统一示例：对应 docs/SDK_USAGE.md，支持 MtlSdkConfig 全部参数（PTP、lcores、tasklets 等）。
// 用法：--mode send 或 --mode recv，DPDK 发送默认使用 build/yuv420p10le_1080p.yuv。
// 示例（发送，DPDK 模式，16 核）：av_txrx_demo --mode send --port 0000:04:00.0 --sip 192.168.10.1 --url build/yuv420p10le_1080p.yuv --lcores 0-15 --tasklets 16

#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"

#include <iostream>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <cmath>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " --mode <send|recv> [options]\n"
            << "  MtlSdkConfig（见 docs/SDK_USAGE.md）：\n"
            << "  --port <name>           MTL 端口，DPDK 用 BDF 如 0000:04:00.0，Kernel 用 kernel:enp4s0 或 kernel:lo\n"
            << "  --sip <ip>              本机源 IP（默认 127.0.0.1）\n"
            << "  --tx-queues <n>         发送队列数（send 默认 1，recv 默认 0）\n"
            << "  --rx-queues <n>         接收队列数（send 默认 0，recv 默认 1）\n"
            << "  --lcores <list>         DPDK lcore 列表，如 0-15 或 2,3,4,5（跑满网卡/多用 CPU）\n"
            << "  --main-lcore <id>       主 lcore id，<0 为 MTL 自动\n"
            << "  --tasklets <n>          每 scheduler tasklet 数，0=自动（可试 16）\n"
            << "  --data-quota-mbs <n>    每 lcore 数据配额 MB/s，0=自动\n"
            << "  --no-ptp                禁用 PTP，用合成时间戳\n"
            << "  --ptp-system            以系统时间为 PTP 源（先 ptp4l+phc2sys 同步）；不走网卡 PTP\n"
            << "  --bind-numa <0|1>       是否绑定 NUMA，默认 1\n"
            << "  流参数（收发一致）：\n"
            << "  --ip <multicast_ip>      组播 IP（默认 239.0.0.1）\n"
            << "  --video-port <port>      视频端口（默认 5004）\n"
            << "  --audio-port <port>      音频端口，0=无（默认 0）\n"
            << "  --width <w> --height <h> --fps <fps>  分辨率与帧率（默认 1920 1080 59.94）\n"
            << "  仅 send：\n"
            << "  --url <yuv_file>         YUV 文件路径，默认 build/yuv420p10le_1080p.yuv\n"
            << "  --fmt <format>           yuv420p10le | yuv422p10le（默认 yuv420p10le）\n"
            << "  --duration <sec>         发送秒数（默认 30）\n"
            << "  --sdp-out <file>         导出 SDP 到文件\n"
            << "  --put-retry <n>          put_video 失败重试次数（默认 150）\n"
            << "  --prefill-frames <n>     启动预填帧数（默认 4）\n"
            << "  仅 recv：\n"
            << "  --max-frames <n>         接收帧数后退出（默认 1800）\n"
            << "  --sdp <file>             从 SDP 加载 IP/端口/格式\n"
            << "  [output.mp4]             输出文件（默认 out.mp4）\n";
}

static void yuv420p10le_to_yuv422p10le(int w, int h,
    const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in,
    uint8_t* y_out, uint8_t* u_out, uint8_t* v_out) {
  const size_t y_sz = (size_t)w * h * 2;
  memcpy(y_out, y_in, y_sz);
  const int u_w = w / 2, u_h = h / 2;
  const int out_u_stride = u_w * 2;
  for (int row = 0; row < u_h; row++) {
    const uint8_t* u_row = u_in + row * u_w * 2;
    const uint8_t* v_row = v_in + row * u_w * 2;
    memcpy(u_out + (row * 2) * out_u_stride, u_row, (size_t)out_u_stride);
    memcpy(u_out + (row * 2 + 1) * out_u_stride, u_row, (size_t)out_u_stride);
    memcpy(v_out + (row * 2) * out_u_stride, v_row, (size_t)out_u_stride);
    memcpy(v_out + (row * 2 + 1) * out_u_stride, v_row, (size_t)out_u_stride);
  }
}

struct FrameCopy {
  mtl_sdk::VideoFormat fmt;
  int64_t timestamp_ns;
  std::vector<uint8_t> y, u, v;
  int linesize_y, linesize_uv;

  static FrameCopy from(const mtl_sdk::VideoFrame& f) {
    FrameCopy fc;
    fc.fmt = f.fmt;
    fc.timestamp_ns = f.timestamp_ns;
    fc.linesize_y = f.planes[0].linesize;
    fc.linesize_uv = f.planes[1].linesize;
    size_t y_sz = (size_t)f.fmt.height * f.planes[0].linesize;
    size_t uv_sz = (size_t)f.fmt.height * f.planes[1].linesize;
    if (f.num_planes >= 3 && f.planes[0].data && f.planes[1].data && f.planes[2].data && y_sz > 0 && uv_sz > 0) {
      fc.y.assign(f.planes[0].data, f.planes[0].data + y_sz);
      fc.u.assign(f.planes[1].data, f.planes[1].data + uv_sz);
      fc.v.assign(f.planes[2].data, f.planes[2].data + uv_sz);
    }
    return fc;
  }
  bool is_valid() const { return !y.empty() && !u.empty() && !v.empty(); }
  void to_video_frame(mtl_sdk::VideoFrame& out) const {
    if (!is_valid()) return;
    out = {};
    out.fmt = fmt;
    out.timestamp_ns = timestamp_ns;
    out.mem_type = mtl_sdk::MemoryType::HostPtr;
    out.num_planes = 3;
    out.planes[0].data = const_cast<uint8_t*>(y.data());
    out.planes[0].linesize = linesize_y;
    out.planes[1].data = const_cast<uint8_t*>(u.data());
    out.planes[1].linesize = linesize_uv;
    out.planes[2].data = const_cast<uint8_t*>(v.data());
    out.planes[2].linesize = linesize_uv;
    out.bytes_total = y.size() + u.size() + v.size();
  }
};

class FrameQueue {
public:
  static constexpr size_t MAX_SIZE = 64;
  void push(FrameCopy fc) {
    std::unique_lock<std::mutex> lock(m_);
    while (q_.size() >= MAX_SIZE) cv_full_.wait(lock);
    q_.push(std::move(fc));
    cv_empty_.notify_one();
  }
  bool pop(FrameCopy& out) {
    std::unique_lock<std::mutex> lock(m_);
    while (q_.empty() && !done_) cv_empty_.wait_for(lock, std::chrono::milliseconds(100));
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop();
    cv_full_.notify_one();
    return true;
  }
  void set_done() { std::lock_guard<std::mutex> lock(m_); done_ = true; cv_empty_.notify_all(); }
private:
  std::queue<FrameCopy> q_;
  std::mutex m_;
  std::condition_variable cv_empty_, cv_full_;
  std::atomic<bool> done_{false};
};

int run_send(mtl_sdk::Context* ctx, int argc, char** argv,
  const std::string& port_name, const std::string& sip, const std::string& lcores,
  int main_lcore, uint32_t tasklets_nb_per_sch, uint32_t data_quota_mbs_per_sch,
  bool use_ptp_timestamps, bool use_ptp_system, int tx_queues, int rx_queues, bool bind_numa);
int run_recv(mtl_sdk::Context* ctx, int argc, char** argv,
  const std::string& port_name, const std::string& sip, const std::string& lcores,
  int main_lcore, uint32_t tasklets_nb_per_sch, uint32_t data_quota_mbs_per_sch,
  bool use_ptp_timestamps, bool use_ptp_system, int tx_queues, int rx_queues, bool bind_numa);

int main(int argc, char** argv) {
  std::string mode;
  std::string port_name = "kernel:lo";
  std::string sip = "127.0.0.1";
  std::string lcores;
  int main_lcore = -1;
  uint32_t tasklets_nb_per_sch = 0;
  uint32_t data_quota_mbs_per_sch = 0;
  bool use_ptp = true;
  bool use_ptp_system = false;  // use system time (ptp4l+phc2sys) as PTP source
  int tx_queues = -1, rx_queues = -1;
  bool bind_numa = true;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--mode" && i + 1 < argc) { mode = argv[++i]; continue; }
    if (a == "--port" && i + 1 < argc) { port_name = argv[++i]; continue; }
    if (a == "--sip" && i + 1 < argc) { sip = argv[++i]; continue; }
    if (a == "--lcores" && i + 1 < argc) { lcores = argv[++i]; continue; }
    if (a == "--main-lcore" && i + 1 < argc) { main_lcore = atoi(argv[++i]); continue; }
    if (a == "--tasklets" && i + 1 < argc) { tasklets_nb_per_sch = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--data-quota-mbs" && i + 1 < argc) { data_quota_mbs_per_sch = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--no-ptp") { use_ptp = false; continue; }
    if (a == "--ptp-system") { use_ptp_system = true; continue; }
    if (a == "--tx-queues" && i + 1 < argc) { tx_queues = atoi(argv[++i]); continue; }
    if (a == "--rx-queues" && i + 1 < argc) { rx_queues = atoi(argv[++i]); continue; }
    if (a == "--bind-numa" && i + 1 < argc) { bind_numa = (atoi(argv[++i]) != 0); continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
  }

  if (mode != "send" && mode != "recv") {
    std::cerr << "Must specify --mode send or --mode recv\n";
    usage(argv[0]);
    return 1;
  }

  mtl_sdk::MtlSdkConfig cfg;
  cfg.ports.push_back({port_name, sip});
  cfg.tx_queues = tx_queues >= 0 ? tx_queues : (mode == "send" ? 1 : 0);
  cfg.rx_queues = rx_queues >= 0 ? rx_queues : (mode == "recv" ? 1 : 0);
  cfg.enable_builtin_ptp = use_ptp && !use_ptp_system;  // no NIC PTP when using system time
  cfg.bind_numa = bind_numa;
  cfg.lcores = lcores;
  cfg.main_lcore = main_lcore;
  cfg.tasklets_nb_per_sch = tasklets_nb_per_sch;
  cfg.data_quota_mbs_per_sch = data_quota_mbs_per_sch;

  if (use_ptp_system) {
    cfg.ptp_mode = mtl_sdk::PtpMode::ExternalFn;
    cfg.external_ptp_time_fn = []() -> mtl_sdk::TimestampNs {
      struct timespec ts;
      if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
      return (mtl_sdk::TimestampNs)ts.tv_sec * 1000000000LL + (mtl_sdk::TimestampNs)ts.tv_nsec;
    };
  }

  auto ctx = mtl_sdk::Context::create(cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  bool use_ptp_timestamps = use_ptp || use_ptp_system;
  int ret = 0;
  if (mode == "send")
    ret = run_send(ctx.get(), argc, argv, port_name, sip, lcores, main_lcore, tasklets_nb_per_sch, data_quota_mbs_per_sch, use_ptp_timestamps, use_ptp_system, cfg.tx_queues, cfg.rx_queues, bind_numa);
  else
    ret = run_recv(ctx.get(), argc, argv, port_name, sip, lcores, main_lcore, tasklets_nb_per_sch, data_quota_mbs_per_sch, use_ptp_timestamps, use_ptp_system, cfg.tx_queues, cfg.rx_queues, bind_numa);

  ctx->stop();
  return ret;
}

int run_send(mtl_sdk::Context* ctx, int argc, char** argv,
  const std::string& port_name, const std::string& sip, const std::string& lcores,
  int main_lcore, uint32_t tasklets_nb_per_sch, uint32_t data_quota_mbs_per_sch,
  bool use_ptp_timestamps, bool use_ptp_system, int tx_queues, int rx_queues, bool bind_numa)
{
  (void)port_name;(void)sip;(void)lcores;(void)main_lcore;(void)tasklets_nb_per_sch;(void)data_quota_mbs_per_sch;(void)tx_queues;(void)rx_queues;(void)bind_numa;(void)use_ptp_system;
  std::string ip = "239.0.0.1";
  std::string url = "build/yuv420p10le_1080p.yuv";
  std::string fmt_str = "yuv420p10le";
  std::string sdp_out;
  uint16_t video_port = 5004;
  uint16_t audio_port = 0;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  int duration_sec = 30;
  int put_retry = 150;
  int prefill_frames = 4;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--mode" && i + 1 < argc) { i++; continue; }
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--url" && i + 1 < argc) { url = argv[++i]; continue; }
    if (a == "--fmt" && i + 1 < argc) { fmt_str = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--width" && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (a == "--height" && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (a == "--fps" && i + 1 < argc) { fps = atof(argv[++i]); continue; }
    if (a == "--duration" && i + 1 < argc) { duration_sec = atoi(argv[++i]); continue; }
    if (a == "--sdp-out" && i + 1 < argc) { sdp_out = argv[++i]; continue; }
    if (a == "--put-retry" && i + 1 < argc) { put_retry = atoi(argv[++i]); continue; }
    if (a == "--prefill-frames" && i + 1 < argc) { prefill_frames = atoi(argv[++i]); continue; }
  }

  const bool from_file = true;
  const bool use_yuv420 = (fmt_str == "yuv420p10le");
  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  mtl_sdk::St2110Endpoint vep{ip, video_port, 96};
  auto v_tx = ctx->create_video_tx(vf, vep);
  if (!v_tx) { std::cerr << "Failed to create video TX\n"; return 1; }

  std::unique_ptr<mtl_sdk::Context::AudioTxSession> a_tx;
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = 48000;
    af.channels = 2;
    af.bits_per_sample = 16;
    a_tx = ctx->create_audio_tx(af, mtl_sdk::St2110Endpoint{ip, audio_port, 97});
  }

  if (!sdp_out.empty()) {
    mtl_sdk::SdpSession sdp;
    sdp.session_name = "av_txrx_demo";
    sdp.origin = "- 0 0 IN IP4 " + sip;
    sdp.connection = "IN IP4 " + ip + "/32";
    mtl_sdk::SdpMedia mv;
    mv.type = mtl_sdk::SdpMedia::Type::Video;
    mv.endpoint = vep;
    mv.rtpmap = "raw/90000";
    mv.fmtp_kv.push_back("sampling=YCbCr-4:2:2");
    mv.fmtp_kv.push_back("width=" + std::to_string(width));
    mv.fmtp_kv.push_back("height=" + std::to_string(height));
    int fps_num = static_cast<int>(fps * 1000.0 + 0.5);
    mv.fmtp_kv.push_back("exactframerate=" + std::to_string(fps_num) + "/1000");
    sdp.media.push_back(std::move(mv));
    try { mtl_sdk::save_sdp_file(sdp_out, sdp); std::cout << "Wrote SDP to " << sdp_out << "\n"; } catch (...) {}
  }

  const size_t y_sz = (size_t)width * height * 2;
  const size_t uv_sz = (size_t)(width / 2) * height * 2;
  std::vector<uint8_t> y_buf(y_sz), u_buf(uv_sz), v_buf(uv_sz);

  std::ifstream yuv_file(url, std::ios::binary);
  if (!yuv_file) {
    std::cerr << "Failed to open " << url << "\n";
    return 1;
  }
  std::cout << "Send: " << url << " " << width << "x" << height << " " << fmt_str << ", port=" << port_name << ", sip=" << sip << "\n";

  const int total_video_frames = (int)(fps * duration_sec);
  const int64_t frame_ns = (int64_t)(1e9 / fps);
  bool ptp_valid = use_ptp_timestamps;
  if (ptp_valid && !use_ptp_system && ctx->now_ptp_ns() == 0) {
    ptp_valid = false;
    std::cout << "PTP unavailable, using synthetic timestamps\n";
  } else if (use_ptp_system) std::cout << "PTP source: system time (ptp4l+phc2sys)\n";
  else if (!use_ptp_timestamps) std::cout << "PTP disabled\n";

  mtl_sdk::VideoFrame frame;
  frame.fmt = vf;
  frame.mem_type = mtl_sdk::MemoryType::HostPtr;
  frame.num_planes = 3;
  frame.planes[0].data = y_buf.data();
  frame.planes[0].linesize = width * 2;
  frame.planes[1].data = u_buf.data();
  frame.planes[1].linesize = width;
  frame.planes[2].data = v_buf.data();
  frame.planes[2].linesize = width;
  frame.bytes_total = y_sz + uv_sz * 2;

  auto start = std::chrono::steady_clock::now();
  double frame_interval = 1.0 / fps;

  const int prefill = std::min(prefill_frames, total_video_frames);
  for (int i = 0; i < prefill; i++) {
    frame.timestamp_ns = ptp_valid ? ctx->now_ptp_ns() : (int64_t)i * frame_ns;
    if (use_yuv420) {
      const size_t u420_sz = (size_t)(width/2)*(height/2)*2;
      std::vector<uint8_t> u420(u420_sz), v420(u420_sz);
      yuv_file.read((char*)y_buf.data(), y_sz);
      yuv_file.read((char*)u420.data(), u420_sz);
      yuv_file.read((char*)v420.data(), u420_sz);
      if (!yuv_file || (size_t)yuv_file.gcount() < u420_sz) break;
      yuv420p10le_to_yuv422p10le(width, height, y_buf.data(), u420.data(), v420.data(), y_buf.data(), u_buf.data(), v_buf.data());
    } else {
      yuv_file.read((char*)y_buf.data(), y_sz);
      yuv_file.read((char*)u_buf.data(), uv_sz);
      yuv_file.read((char*)v_buf.data(), uv_sz);
      if (!yuv_file) break;
    }
    if (!v_tx->put_video(frame)) break;
  }
  if (prefill > 0) std::cout << "Prefilled " << prefill << " frames\n";

  int last_put_frame = -1;
  int audio_chunks_sent = 0;
  const int samples_per_audio_chunk = 480;
  const int audio_chunk_bytes = samples_per_audio_chunk * 2 * 2;
  const int audio_chunks_total = (int)(48000 * duration_sec / samples_per_audio_chunk);

  for (int i = prefill; i < total_video_frames; i++) {
    frame.timestamp_ns = ptp_valid ? ctx->now_ptp_ns() : (int64_t)i * frame_ns;
    if (use_yuv420) {
      const size_t u420_sz = (size_t)(width/2)*(height/2)*2;
      std::vector<uint8_t> u420(u420_sz), v420(u420_sz);
      yuv_file.read((char*)y_buf.data(), y_sz);
      yuv_file.read((char*)u420.data(), u420_sz);
      yuv_file.read((char*)v420.data(), u420_sz);
      if (!yuv_file || (size_t)yuv_file.gcount() < u420_sz) break;
      yuv420p10le_to_yuv422p10le(width, height, y_buf.data(), u420.data(), v420.data(), y_buf.data(), u_buf.data(), v_buf.data());
    } else {
      yuv_file.read((char*)y_buf.data(), y_sz);
      yuv_file.read((char*)u_buf.data(), uv_sz);
      yuv_file.read((char*)v_buf.data(), uv_sz);
      if (!yuv_file) break;
    }

    bool put_ok = false;
    if (put_retry > 0) {
      for (int r = 0; r < put_retry; r++) {
        if (v_tx->put_video(frame)) { put_ok = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    } else put_ok = v_tx->put_video(frame);

    if (!put_ok) {
      std::cerr << "put_video failed at frame " << i << "\n";
      break;
    }
    last_put_frame = i;

    if (a_tx && audio_chunks_sent < audio_chunks_total) {
      int chunks_this_frame = (int)((i + 1) * samples_per_audio_chunk * 2 / 48000.0) - audio_chunks_sent;
      for (int c = 0; c < chunks_this_frame && audio_chunks_sent < audio_chunks_total; c++) {
        mtl_sdk::AudioFrame af;
        af.fmt.sample_rate = 48000;
        af.fmt.channels = 2;
        af.fmt.bits_per_sample = 16;
        af.timestamp_ns = (int64_t)audio_chunks_sent * (int64_t)(1e9 * samples_per_audio_chunk / 48000);
        af.pcm.resize(audio_chunk_bytes);
        int16_t* p = (int16_t*)af.pcm.data();
        for (int s = 0; s < samples_per_audio_chunk * 2; s++)
          p[s] = (int16_t)(32000 * std::sin(2.0 * M_PI * 440.0 * (audio_chunks_sent * samples_per_audio_chunk + s/2) / 48000.0));
        a_tx->put_audio(af);
        audio_chunks_sent++;
      }
    }

    auto next = start + std::chrono::duration<double>(frame_interval * (i + 1));
    std::this_thread::sleep_until(next);
  }

  int frames_sent = (last_put_frame >= 0) ? (last_put_frame + 1) : prefill;
  std::cout << "Sent " << frames_sent << " video frames to " << ip << ":" << video_port << "\n";
  return 0;
}

int run_recv(mtl_sdk::Context* ctx, int argc, char** argv,
  const std::string& port_name, const std::string& sip, const std::string& lcores,
  int main_lcore, uint32_t tasklets_nb_per_sch, uint32_t data_quota_mbs_per_sch,
  bool use_ptp_timestamps, bool use_ptp_system, int tx_queues, int rx_queues, bool bind_numa)
{
  (void)port_name;(void)sip;(void)lcores;(void)main_lcore;(void)tasklets_nb_per_sch;(void)data_quota_mbs_per_sch;(void)tx_queues;(void)rx_queues;(void)bind_numa;(void)use_ptp_system;
  std::string ip = "239.0.0.1";
  std::string sdp_path;
  std::string out = "out.mp4";
  uint16_t video_port = 5004;
  uint16_t audio_port = 0;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  int max_frames = 1800;
  int audio_sample_rate = 48000;
  int audio_channels = 2;
  int audio_bits = 16;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--mode" && i + 1 < argc) { i++; continue; }
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--width" && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (a == "--height" && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (a == "--fps" && i + 1 < argc) { fps = atof(argv[++i]); continue; }
    if (a == "--max-frames" && i + 1 < argc) { max_frames = atoi(argv[++i]); continue; }
    if (a == "--sdp" && i + 1 < argc) { sdp_path = argv[++i]; continue; }
    if (a.compare(0, 2, "--") != 0) { out = a; continue; }
  }

  if (!sdp_path.empty()) {
    try {
      mtl_sdk::SdpSession sdp = mtl_sdk::load_sdp_file(sdp_path);
      for (const auto& m : sdp.media) {
        if (m.type == mtl_sdk::SdpMedia::Type::Video) {
          ip = m.endpoint.ip;
          video_port = m.endpoint.udp_port;
          for (const auto& kv : m.fmtp_kv) {
            if (kv.rfind("width=", 0) == 0) width = std::stoi(kv.substr(6));
            else if (kv.rfind("height=", 0) == 0) height = std::stoi(kv.substr(7));
            else if (kv.rfind("exactframerate=", 0) == 0) {
              std::string fr = kv.substr(15);
              auto slash = fr.find('/');
              if (slash != std::string::npos) { int num = std::stoi(fr.substr(0, slash)); int den = std::stoi(fr.substr(slash + 1)); if (den) fps = (double)num / den; }
            }
          }
          break;
        } else if (m.type == mtl_sdk::SdpMedia::Type::Audio) {
          ip = m.endpoint.ip;
          audio_port = m.endpoint.udp_port;
          if (!m.rtpmap.empty()) {
            auto first = m.rtpmap.find('/');
            if (first != std::string::npos) {
              auto second = m.rtpmap.find('/', first + 1);
              if (second != std::string::npos) {
                audio_sample_rate = std::stoi(m.rtpmap.substr(first + 1, second - first - 1));
                audio_channels = std::stoi(m.rtpmap.substr(second + 1));
              } else audio_sample_rate = std::stoi(m.rtpmap.substr(first + 1));
            }
          }
          break;
        }
      }
      std::cout << "Loaded SDP: ip=" << ip << " video_port=" << video_port << " audio_port=" << audio_port << "\n";
    } catch (...) {
      std::cerr << "Failed to load SDP " << sdp_path << "\n";
      return 1;
    }
  }

  if (!use_ptp_timestamps) std::cout << "PTP disabled\n";
  else if (use_ptp_system) std::cout << "PTP source: system time (ptp4l+phc2sys)\n";
  else if (ctx->now_ptp_ns() == 0) std::cout << "PTP unavailable, receiver will use synthetic timestamps\n";

  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  mtl_sdk::St2110Endpoint vep{ip, video_port, 96};
  auto v_rx = ctx->create_video_rx(vf, vep);

  std::unique_ptr<mtl_sdk::Context::AudioRxSession> a_rx;
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = audio_sample_rate;
    af.channels = audio_channels;
    af.bits_per_sample = audio_bits;
    a_rx = ctx->create_audio_rx(af, mtl_sdk::St2110Endpoint{ip, audio_port, 97});
  }

  encode_sdk::EncodeParams ep;
  ep.mux.container = encode_sdk::Container::MP4;
  ep.mux.output_path = out;
  ep.video.codec = encode_sdk::VideoCodec::H264;
  ep.video.hw = encode_sdk::HwAccel::Auto;
  ep.video.bitrate_kbps = 2000;
  ep.video.gop = 120;
  ep.video.profile = "main";
  ep.video.fps_num = (int)(fps + 0.5);
  ep.video.fps_den = 1;
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
  if (audio_port != 0) {
    ep.audio = encode_sdk::AudioEncodeParams{};
    ep.audio->codec = encode_sdk::AudioCodec::AAC;
    ep.audio->bitrate_kbps = 128;
    ep.audio->sample_rate = 48000;
    ep.audio->channels = 2;
  } else ep.audio = std::nullopt;

  auto enc = encode_sdk::Session::open(ep);
  if (!enc) { std::cerr << "Failed to open encoder\n"; return 1; }

  FrameQueue enc_queue;
  std::thread enc_thread([&]() {
    FrameCopy fc;
    while (enc_queue.pop(fc)) {
      mtl_sdk::VideoFrame vf_wrap;
      fc.to_video_frame(vf_wrap);
      enc->push_video(vf_wrap);
    }
  });

  int got = 0;
  while (got < max_frames) {
    mtl_sdk::VideoFrame vf_out;
    if (v_rx->poll(vf_out, 0)) {
      auto fc = FrameCopy::from(vf_out);
      v_rx->release(vf_out);
      if (fc.is_valid()) { enc_queue.push(std::move(fc)); got++; }
    }
    mtl_sdk::AudioFrame af_out;
    if (a_rx && a_rx->poll(af_out, 0)) enc->push_audio(af_out);
  }

  enc_queue.set_done();
  enc_thread.join();
  enc->close();

  std::cout << "Wrote " << out << " (" << got << " frames)\n";
  return 0;
}
