#include "thumbnail.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#ifndef FF_QP2LAMBDA
#define FF_QP2LAMBDA 118
#endif

namespace thumbnailer {
namespace {

constexpr int kMaxEdge = 400;
// ffmpeg mjpeg qscale ~8 ≈ libjpeg quality 50 (1=best, 31=worst).
constexpr int kJpegQscale = 8;

std::string averr(int err) {
  char buf[256];
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

bool mkdir_parent(const std::string& path, std::string& err) {
  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) {
      err = "mkdir: " + ec.message();
      return false;
    }
  }
  return true;
}

struct FmtDeleter {
  void operator()(AVFormatContext* p) const {
    if (!p) return;
    AVFormatContext* tmp = p;
    avformat_close_input(&tmp);
  }
};
struct CodecDeleter {
  void operator()(AVCodecContext* p) const {
    if (!p) return;
    AVCodecContext* tmp = p;
    avcodec_free_context(&tmp);
  }
};
struct FrameDeleter {
  void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct PacketDeleter {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsDeleter {
  void operator()(SwsContext* p) const { sws_freeContext(p); }
};

int snap_rotation(double rot) {
  if (!std::isfinite(rot)) return 0;
  int snap = static_cast<int>(std::lround(rot / 90.0)) * 90;
  snap %= 360;
  if (snap < 0) snap += 360;
  return snap;
}

int stream_rotation_deg(const AVStream* st, const AVFrame* frame) {
  if (frame) {
    const AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (sd && sd->data && sd->size >= 9 * static_cast<int>(sizeof(int32_t))) {
      return snap_rotation(av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data)));
    }
  }
  if (st && st->codecpar) {
    const AVCodecParameters* p = st->codecpar;
    for (int i = 0; i < p->nb_coded_side_data; ++i) {
      const AVPacketSideData& sd = p->coded_side_data[i];
      if (sd.type == AV_PKT_DATA_DISPLAYMATRIX && sd.data &&
          sd.size >= 9 * static_cast<int>(sizeof(int32_t))) {
        return snap_rotation(av_display_rotation_get(reinterpret_cast<const int32_t*>(sd.data)));
      }
    }
  }
  return 0;
}

void rotate_rgb(const uint8_t* src, int w, int h, int deg, std::vector<uint8_t>& dst, int& ow, int& oh) {
  deg %= 360;
  if (deg < 0) deg += 360;
  if (deg == 0) {
    ow = w;
    oh = h;
    dst.assign(src, src + static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    return;
  }
  if (deg == 180) {
    ow = w;
    oh = h;
    dst.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int si = (y * w + x) * 3;
        const int di = ((h - 1 - y) * w + (w - 1 - x)) * 3;
        dst[static_cast<size_t>(di)] = src[si];
        dst[static_cast<size_t>(di) + 1] = src[si + 1];
        dst[static_cast<size_t>(di) + 2] = src[si + 2];
      }
    }
    return;
  }
  ow = h;
  oh = w;
  dst.resize(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int nx = 0, ny = 0;
      if (deg == 90) {
        nx = y;
        ny = w - 1 - x;
      } else {
        nx = h - 1 - y;
        ny = x;
      }
      const int si = (y * w + x) * 3;
      const int di = (ny * ow + nx) * 3;
      dst[static_cast<size_t>(di)] = src[si];
      dst[static_cast<size_t>(di) + 1] = src[si + 1];
      dst[static_cast<size_t>(di) + 2] = src[si + 2];
    }
  }
}

void fit_inside(int w, int h, int& ow, int& oh) {
  const int m = std::max(w, h);
  if (m > kMaxEdge) {
    ow = std::max(1, w * kMaxEdge / m);
    oh = std::max(1, h * kMaxEdge / m);
  } else {
    ow = w;
    oh = h;
  }
  ow = std::max(2, ow & ~1);
  oh = std::max(2, oh & ~1);
}

bool scale_to_rgb(const AVFrame* src, int dw, int dh, std::vector<uint8_t>& rgb, std::string& err) {
  std::unique_ptr<SwsContext, SwsDeleter> sws(sws_getContext(
      src->width, src->height, static_cast<AVPixelFormat>(src->format), dw, dh, AV_PIX_FMT_RGB24,
      SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) {
    err = "swscale rgb context failed";
    return false;
  }
  rgb.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
  uint8_t* dst_data[4] = {rgb.data(), nullptr, nullptr, nullptr};
  int dst_linesize[4] = {dw * 3, 0, 0, 0};
  sws_scale(sws.get(), src->data, src->linesize, 0, src->height, dst_data, dst_linesize);
  return true;
}

bool scale_rgb(const uint8_t* src, int sw, int sh, int dw, int dh, std::vector<uint8_t>& dst, std::string& err) {
  if (sw == dw && sh == dh) {
    dst.assign(src, src + static_cast<size_t>(sw) * static_cast<size_t>(sh) * 3);
    return true;
  }
  std::unique_ptr<SwsContext, SwsDeleter> sws(sws_getContext(
      sw, sh, AV_PIX_FMT_RGB24, dw, dh, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) {
    err = "swscale rgb context failed";
    return false;
  }
  dst.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3);
  const uint8_t* src_data[4] = {src, nullptr, nullptr, nullptr};
  int src_linesize[4] = {sw * 3, 0, 0, 0};
  uint8_t* dst_data[4] = {dst.data(), nullptr, nullptr, nullptr};
  int dst_linesize[4] = {dw * 3, 0, 0, 0};
  sws_scale(sws.get(), src_data, src_linesize, 0, sh, dst_data, dst_linesize);
  return true;
}

bool encode_jpeg(const uint8_t* rgb, int w, int h, const std::string& output_path, std::string& err) {
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!enc) {
    err = "mjpeg encoder missing";
    return false;
  }
  std::unique_ptr<AVCodecContext, CodecDeleter> ctx(avcodec_alloc_context3(enc));
  if (!ctx) {
    err = "mjpeg alloc failed";
    return false;
  }
  ctx->width = w;
  ctx->height = h;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->time_base = AVRational{1, 25};
  ctx->framerate = AVRational{25, 1};
  ctx->color_range = AVCOL_RANGE_JPEG;
  ctx->flags |= AV_CODEC_FLAG_QSCALE;
  ctx->global_quality = kJpegQscale * FF_QP2LAMBDA;

  int ret = avcodec_open2(ctx.get(), enc, nullptr);
  if (ret < 0) {
    err = std::string("mjpeg open: ") + averr(ret);
    return false;
  }

  std::unique_ptr<SwsContext, SwsDeleter> sws(sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P,
                                                            SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) {
    err = "swscale yuv context failed";
    return false;
  }

  std::unique_ptr<AVFrame, FrameDeleter> yuv(av_frame_alloc());
  if (!yuv) {
    err = "yuv frame alloc failed";
    return false;
  }
  yuv->format = AV_PIX_FMT_YUV420P;
  yuv->width = w;
  yuv->height = h;
  yuv->color_range = AVCOL_RANGE_JPEG;
  ret = av_frame_get_buffer(yuv.get(), 32);
  if (ret < 0) {
    err = std::string("yuv buffer: ") + averr(ret);
    return false;
  }
  const uint8_t* src_data[4] = {rgb, nullptr, nullptr, nullptr};
  int src_linesize[4] = {w * 3, 0, 0, 0};
  sws_scale(sws.get(), src_data, src_linesize, 0, h, yuv->data, yuv->linesize);
  yuv->pts = 0;

  std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
  if (!pkt) {
    err = "packet alloc failed";
    return false;
  }
  ret = avcodec_send_frame(ctx.get(), yuv.get());
  if (ret < 0) {
    err = std::string("mjpeg send: ") + averr(ret);
    return false;
  }
  ret = avcodec_send_frame(ctx.get(), nullptr);
  if (ret < 0) {
    err = std::string("mjpeg flush: ") + averr(ret);
    return false;
  }
  ret = avcodec_receive_packet(ctx.get(), pkt.get());
  if (ret < 0) {
    err = std::string("mjpeg receive: ") + averr(ret);
    return false;
  }

  if (!mkdir_parent(output_path, err)) return false;
  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    err = "failed to open output";
    return false;
  }
  out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
  if (!out) {
    err = "failed to write jpeg";
    return false;
  }
  return true;
}

bool decode_first_frame(AVFormatContext* fmt, int stream_idx, AVCodecContext* dec, AVFrame* frame,
                        bool keys_only, std::string& err) {
  std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
  if (!pkt) {
    err = "packet alloc failed";
    return false;
  }
  while (true) {
    int ret = av_read_frame(fmt, pkt.get());
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        err.clear();
        return false;
      }
      err = std::string("read frame: ") + averr(ret);
      return false;
    }
    if (pkt->stream_index != stream_idx) {
      av_packet_unref(pkt.get());
      continue;
    }
    if (keys_only && !(pkt->flags & AV_PKT_FLAG_KEY)) {
      av_packet_unref(pkt.get());
      continue;
    }
    ret = avcodec_send_packet(dec, pkt.get());
    av_packet_unref(pkt.get());
    if (ret < 0) {
      err = std::string("send packet: ") + averr(ret);
      return false;
    }
    ret = avcodec_receive_frame(dec, frame);
    if (ret == 0) return true;
    if (ret == AVERROR(EAGAIN)) continue;
    if (ret == AVERROR_EOF) {
      err.clear();
      return false;
    }
    err = std::string("receive frame: ") + averr(ret);
    return false;
  }
}

int packet_side_rotation(const AVPacketSideData* list, int count) {
  if (!list || count <= 0) return -1;
  for (int i = 0; i < count; ++i) {
    const AVPacketSideData& sd = list[i];
    if (sd.type == AV_PKT_DATA_DISPLAYMATRIX && sd.data &&
        sd.size >= 9 * static_cast<int>(sizeof(int32_t))) {
      return snap_rotation(av_display_rotation_get(reinterpret_cast<const int32_t*>(sd.data)));
    }
  }
  return -1;
}

void blit_rgb(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, int dx, int dy) {
  if (!dst || !src || sw < 1 || sh < 1 || dw < 1 || dh < 1) return;
  for (int y = 0; y < sh; ++y) {
    const int cy = dy + y;
    if (cy < 0 || cy >= dh) continue;
    int sx = 0;
    int dx0 = dx;
    int copy = sw;
    if (dx0 < 0) {
      sx = -dx0;
      copy -= sx;
      dx0 = 0;
    }
    if (dx0 >= dw || copy <= 0) continue;
    if (dx0 + copy > dw) copy = dw - dx0;
    std::memcpy(dst + (static_cast<size_t>(cy) * static_cast<size_t>(dw) + dx0) * 3,
                src + (static_cast<size_t>(y) * static_cast<size_t>(sw) + sx) * 3,
                static_cast<size_t>(copy) * 3);
  }
}

struct TileImage {
  int stream_index = -1;
  std::vector<uint8_t> rgb;
  int w = 0;
  int h = 0;
  bool got = false;
  std::unique_ptr<AVCodecContext, CodecDeleter> ctx;
};

TileImage* find_tile(std::vector<TileImage>& tiles, int stream_index) {
  for (auto& tile : tiles) {
    if (tile.stream_index == stream_index) return &tile;
  }
  return nullptr;
}

bool frame_to_rgb(AVFrame* frame, std::vector<uint8_t>& rgb, int& w, int& h, std::string& err) {
  w = frame->width;
  h = frame->height;
  if (w < 1 || h < 1) {
    err = "empty heif tile";
    return false;
  }
  return scale_to_rgb(frame, w, h, rgb, err);
}

bool receive_tile(TileImage& tile, std::string& err) {
  if (tile.got) return true;
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  if (!frame) {
    err = "frame alloc failed";
    return false;
  }
  const int ret = avcodec_receive_frame(tile.ctx.get(), frame.get());
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return true;
  if (ret < 0) {
    err = std::string("receive tile: ") + averr(ret);
    return false;
  }
  if (!frame_to_rgb(frame.get(), tile.rgb, tile.w, tile.h, err)) return false;
  tile.got = true;
  return true;
}

bool send_tile_packet(TileImage& tile, AVPacket* pkt, std::string& err) {
  if (tile.got) return true;
  while (true) {
    const int ret = avcodec_send_packet(tile.ctx.get(), pkt);
    if (ret == AVERROR(EAGAIN)) {
      if (!receive_tile(tile, err)) return false;
      continue;
    }
    if (ret < 0) {
      err = std::string("send tile: ") + averr(ret);
      return false;
    }
    return receive_tile(tile, err);
  }
}

bool flush_tile(TileImage& tile, std::string& err) {
  if (tile.got) return true;
  const int ret = avcodec_send_packet(tile.ctx.get(), nullptr);
  if (ret < 0 && ret != AVERROR_EOF) {
    err = std::string("flush tile: ") + averr(ret);
    return false;
  }
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  if (!frame) {
    err = "frame alloc failed";
    return false;
  }
  while (true) {
    const int got = avcodec_receive_frame(tile.ctx.get(), frame.get());
    if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
    if (got < 0) {
      err = std::string("receive tile: ") + averr(got);
      return false;
    }
    if (!frame_to_rgb(frame.get(), tile.rgb, tile.w, tile.h, err)) return false;
    tile.got = true;
    return true;
  }
}

bool open_tile_decoder(AVStream* st, TileImage& tile, std::string& err) {
  const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
  if (!decoder) {
    err = "no decoder for heif tile";
    return false;
  }
  tile.ctx.reset(avcodec_alloc_context3(decoder));
  if (!tile.ctx) {
    err = "decoder alloc failed";
    return false;
  }
  int ret = avcodec_parameters_to_context(tile.ctx.get(), st->codecpar);
  if (ret < 0) {
    err = std::string("codecpar: ") + averr(ret);
    return false;
  }
  ret = avcodec_open2(tile.ctx.get(), decoder, nullptr);
  if (ret < 0) {
    err = std::string("decoder open: ") + averr(ret);
    return false;
  }
  tile.stream_index = st->index;
  return true;
}

AVStreamGroup* largest_tile_grid(AVFormatContext* fmt) {
  AVStreamGroup* best = nullptr;
  int64_t best_area = -1;
  for (unsigned i = 0; i < fmt->nb_stream_groups; ++i) {
    AVStreamGroup* group = fmt->stream_groups[i];
    if (!group || group->type != AV_STREAM_GROUP_PARAMS_TILE_GRID || !group->params.tile_grid) continue;
    const AVStreamGroupTileGrid* grid = group->params.tile_grid;
    if (grid->nb_tiles == 0 || !grid->offsets || grid->width < 1 || grid->height < 1) continue;
    if (!group->streams || group->nb_streams == 0) continue;
    const int64_t area = static_cast<int64_t>(grid->width) * static_cast<int64_t>(grid->height);
    if (area > best_area) {
      best_area = area;
      best = group;
    }
  }
  return best;
}

// 1 = assembled, 0 = no tile grid, -1 = error.
int stitch_heif_grid(AVFormatContext* fmt, std::vector<uint8_t>& out, int& out_w, int& out_h, int& rot,
                     std::string& err) {
  AVStreamGroup* group = largest_tile_grid(fmt);
  if (!group) return 0;
  const AVStreamGroupTileGrid* grid = group->params.tile_grid;
  if (grid->coded_width < 1 || grid->coded_height < 1 ||
      static_cast<int64_t>(grid->coded_width) * static_cast<int64_t>(grid->coded_height) > 80000000) {
    err = "heif grid is too large";
    return -1;
  }

  std::vector<TileImage> tiles;
  for (unsigned i = 0; i < grid->nb_tiles; ++i) {
    const unsigned idx = grid->offsets[i].idx;
    if (idx >= group->nb_streams || !group->streams[idx]) {
      err = "heif grid references a missing tile";
      return -1;
    }
    AVStream* st = group->streams[idx];
    if (find_tile(tiles, st->index)) continue;
    TileImage tile;
    if (!open_tile_decoder(st, tile, err)) return -1;
    tiles.push_back(std::move(tile));
  }

  std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
  if (!pkt) {
    err = "packet alloc failed";
    return -1;
  }
  auto all_got = [&]() {
    for (const auto& tile : tiles) {
      if (!tile.got) return false;
    }
    return true;
  };
  while (!all_got()) {
    const int ret = av_read_frame(fmt, pkt.get());
    if (ret == AVERROR_EOF) break;
    if (ret < 0) {
      err = std::string("read frame: ") + averr(ret);
      return -1;
    }
    TileImage* tile = find_tile(tiles, pkt->stream_index);
    if (!tile || tile->got) {
      av_packet_unref(pkt.get());
      continue;
    }
    const bool ok = send_tile_packet(*tile, pkt.get(), err);
    av_packet_unref(pkt.get());
    if (!ok) return -1;
  }
  for (auto& tile : tiles) {
    if (!flush_tile(tile, err)) return -1;
    if (!tile.got) {
      err = "missing heif tile";
      return -1;
    }
  }

  const int cw = grid->coded_width;
  const int ch = grid->coded_height;
  std::vector<uint8_t> canvas(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 3);
  const uint8_t br = grid->background[0];
  const uint8_t bg = grid->background[1];
  const uint8_t bb = grid->background[2];
  if (br || bg || bb) {
    for (size_t i = 0; i < canvas.size(); i += 3) {
      canvas[i] = br;
      canvas[i + 1] = bg;
      canvas[i + 2] = bb;
    }
  }
  for (unsigned i = 0; i < grid->nb_tiles; ++i) {
    const auto& off = grid->offsets[i];
    AVStream* st = group->streams[off.idx];
    TileImage* tile = find_tile(tiles, st->index);
    blit_rgb(canvas.data(), cw, ch, tile->rgb.data(), tile->w, tile->h, off.horizontal, off.vertical);
  }

  const int ox = grid->horizontal_offset;
  const int oy = grid->vertical_offset;
  const int pw = grid->width;
  const int ph = grid->height;
  if (ox < 0 || oy < 0 || pw < 1 || ph < 1 || ox + pw > cw || oy + ph > ch) {
    err = "bad heif grid crop";
    return -1;
  }
  out.resize(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 3);
  for (int y = 0; y < ph; ++y) {
    std::memcpy(out.data() + static_cast<size_t>(y) * static_cast<size_t>(pw) * 3,
                canvas.data() + (static_cast<size_t>(oy + y) * static_cast<size_t>(cw) + ox) * 3,
                static_cast<size_t>(pw) * 3);
  }
  out_w = pw;
  out_h = ph;
  rot = packet_side_rotation(grid->coded_side_data, grid->nb_coded_side_data);
  if (rot < 0) rot = stream_rotation_deg(group->nb_streams > 0 ? group->streams[0] : nullptr, nullptr);
  return 1;
}

bool write_fitted_jpeg(const uint8_t* rgb, int w, int h, int rot, const std::string& output_path,
                       ProgressFn progress, std::string& err) {
  int disp_w = w;
  int disp_h = h;
  if (rot == 90 || rot == 270) std::swap(disp_w, disp_h);
  int out_w = 0, out_h = 0;
  fit_inside(disp_w, disp_h, out_w, out_h);
  int scale_w = out_w, scale_h = out_h;
  if (rot == 90 || rot == 270) std::swap(scale_w, scale_h);

  std::vector<uint8_t> scaled;
  if (!scale_rgb(rgb, w, h, scale_w, scale_h, scaled, err)) return false;
  std::vector<uint8_t> oriented;
  int rgb_w = 0, rgb_h = 0;
  rotate_rgb(scaled.data(), scale_w, scale_h, rot, oriented, rgb_w, rgb_h);
  if (progress) progress(80, "encoding");
  return encode_jpeg(oriented.data(), rgb_w, rgb_h, output_path, err);
}

}  // namespace

bool write_text_file(const std::string& path, const std::string& text, std::string& err) {
  if (!mkdir_parent(path, err)) return false;
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    err = "failed to open output";
    return false;
  }
  const std::string body = text.empty() ? "unknown error" : text;
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!out) {
    err = "failed to write error text";
    return false;
  }
  return true;
}

bool make_thumbnail(const std::string& input_path, const std::string& output_path, ProgressFn progress,
                    std::string& err) {
  err.clear();
  av_log_set_level(AV_LOG_ERROR);
  if (progress) progress(5, "opening");

  AVFormatContext* raw_fmt = nullptr;
  int ret = avformat_open_input(&raw_fmt, input_path.c_str(), nullptr, nullptr);
  if (ret < 0) {
    err = std::string("open: ") + averr(ret);
    return false;
  }
  std::unique_ptr<AVFormatContext, FmtDeleter> fmt(raw_fmt);

  ret = avformat_find_stream_info(fmt.get(), nullptr);
  if (ret < 0) {
    err = std::string("stream info: ") + averr(ret);
    return false;
  }

  if (progress) progress(25, "decoding");
  std::vector<uint8_t> grid_rgb;
  int grid_w = 0, grid_h = 0, grid_rot = 0;
  const int grid_rc = stitch_heif_grid(fmt.get(), grid_rgb, grid_w, grid_h, grid_rot, err);
  if (grid_rc < 0) return false;
  if (grid_rc > 0) {
    if (progress) progress(55, "scaling");
    if (!write_fitted_jpeg(grid_rgb.data(), grid_w, grid_h, grid_rot, output_path, progress, err)) return false;
    if (progress) progress(95, "writing output");
    return true;
  }

  const int stream_idx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_idx < 0) {
    err = "no video stream";
    return false;
  }
  AVStream* st = fmt->streams[stream_idx];
  const AVCodec* decoder = avcodec_find_decoder(st->codecpar->codec_id);
  if (!decoder) {
    err = "no decoder for codec";
    return false;
  }
  std::unique_ptr<AVCodecContext, CodecDeleter> dec(avcodec_alloc_context3(decoder));
  if (!dec) {
    err = "decoder alloc failed";
    return false;
  }
  ret = avcodec_parameters_to_context(dec.get(), st->codecpar);
  if (ret < 0) {
    err = std::string("codecpar: ") + averr(ret);
    return false;
  }
  ret = avcodec_open2(dec.get(), decoder, nullptr);
  if (ret < 0) {
    err = std::string("decoder open: ") + averr(ret);
    return false;
  }

  if (progress) progress(25, "decoding");
  std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
  if (!frame) {
    err = "frame alloc failed";
    return false;
  }

  std::string dec_err;
  bool got = decode_first_frame(fmt.get(), stream_idx, dec.get(), frame.get(), true, dec_err);
  if (!got) {
    av_seek_frame(fmt.get(), stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec.get());
    dec_err.clear();
    got = decode_first_frame(fmt.get(), stream_idx, dec.get(), frame.get(), false, dec_err);
  }
  if (!got) {
    err = dec_err.empty() ? "no video frame" : dec_err;
    return false;
  }

  const int rot = stream_rotation_deg(st, frame.get());
  int disp_w = frame->width;
  int disp_h = frame->height;
  if (rot == 90 || rot == 270) std::swap(disp_w, disp_h);
  int out_w = 0, out_h = 0;
  fit_inside(disp_w, disp_h, out_w, out_h);
  int scale_w = out_w, scale_h = out_h;
  if (rot == 90 || rot == 270) std::swap(scale_w, scale_h);

  if (progress) progress(55, "scaling");
  std::vector<uint8_t> rgb;
  if (!scale_to_rgb(frame.get(), scale_w, scale_h, rgb, err)) return false;

  std::vector<uint8_t> oriented;
  int rgb_w = 0, rgb_h = 0;
  rotate_rgb(rgb.data(), scale_w, scale_h, rot, oriented, rgb_w, rgb_h);

  if (progress) progress(80, "encoding");
  if (!encode_jpeg(oriented.data(), rgb_w, rgb_h, output_path, err)) return false;
  if (progress) progress(95, "writing output");
  return true;
}

}  // namespace thumbnailer
