﻿/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_TRANSCODE_H
#define ZLMEDIAKIT_TRANSCODE_H

#if defined(ENABLE_FFMPEG)

#include "Util/TimeTicker.h"
#include "Common/MediaSink.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "libswscale/swscale.h"
#include "libavutil/avutil.h"
#include "libavutil/pixdesc.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/imgutils.h"
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

#include <cstdio>
#include <ctime>
#include <sys/stat.h>
// #include <sys/types>

namespace mediakit {

class FFmpegWatermark;

class FFmpegFrame {
public:
    using Ptr = std::shared_ptr<FFmpegFrame>;

    FFmpegFrame(std::shared_ptr<AVFrame> frame = nullptr);
    ~FFmpegFrame();

    AVFrame *get() const;
    void fillPicture(AVPixelFormat target_format, int target_width, int target_height);

private:
    char *_data = nullptr;
    std::shared_ptr<AVFrame> _frame;
};

class FFmpegSwr {
public:
    using Ptr = std::shared_ptr<FFmpegSwr>;

    FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate);
    ~FFmpegSwr();
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame);
    AVFrame* inputFrame(AVFrame *frame);

private:
    int _target_channels;
    int _target_channel_layout;
    int _target_samplerate;
    AVSampleFormat _target_format;
    SwrContext *_ctx = nullptr;
};

class TaskManager {
public:
    virtual ~TaskManager();

    void setMaxTaskSize(size_t size);
    void stopThread(bool drop_task);

protected:
    void startThread(const std::string &name);
    bool addEncodeTask(std::function<void()> task);
    bool addDecodeTask(bool key_frame, std::function<void()> task);
    bool isEnabled() const;

private:
    void onThreadRun(const std::string &name);

private:
    class ThreadExitException : public std::runtime_error {
    public:
        ThreadExitException() : std::runtime_error("exit") {}
    };

private:
    bool _decode_drop_start = false;
    bool _exit = false;
    size_t _max_task = 30;
    std::mutex _task_mtx;
    toolkit::semaphore _sem;
    toolkit::List<std::function<void()> > _task;
    std::shared_ptr<std::thread> _thread;
};

class FFmpegDecoder : public TaskManager {
public:
    using Ptr = std::shared_ptr<FFmpegDecoder>;
    using onDec = std::function<void(const FFmpegFrame::Ptr &)>;
    using onDecAvframe = std::function<void(const AVFrame *)>;


    FFmpegDecoder(const Track::Ptr &track, int thread_num = 2, const std::vector<std::string> &codec_name = {});
    ~FFmpegDecoder() override;

    bool inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge = true);
    void setOnDecode(onDec cb);
    void setOnDecode(onDecAvframe cb);
    void flush();
    const AVCodecContext *getContext() const;
    const AVCodecContext *getDecoderContext()const;
private:
    void onDecode(const FFmpegFrame::Ptr &frame);
    void onDecode(const AVFrame *frame);
    bool inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge);
    bool decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame);

private:
    // default merge frame
    bool _do_merger = true;
    toolkit::Ticker _ticker;
    onDec _cb;
    onDecAvframe _cb_avframe;
    // std::shared_ptr<AVCodecContext> _context;
    std::shared_ptr<AVCodecContext> _decoder_context;
    FrameMerger _merger{FrameMerger::h264_prefix};
    std::shared_ptr<FFmpegWatermark> _watermark;
};

class FFmpegSws {
public:
    using Ptr = std::shared_ptr<FFmpegSws>;

    FFmpegSws(AVPixelFormat output, int width, int height);
    ~FFmpegSws();
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame);
    int inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data);
    AVFrame * inputFrame(AVFrame *frame);
    int inputFrame(AVFrame *frame, uint8_t *data);

private:
    FFmpegFrame::Ptr inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data);
    AVFrame * inputFrame(AVFrame *frame, int &ret, uint8_t *data);


private:
    int _target_width = 0;
    int _target_height = 0;
    int _src_width = 0;
    int _src_height = 0;
    SwsContext *_ctx = nullptr;
    AVPixelFormat _src_format = AV_PIX_FMT_NONE;
    AVPixelFormat _target_format = AV_PIX_FMT_NONE;
};

class FFmpegAudioFifo {
public:
    FFmpegAudioFifo() = default;
    ~FFmpegAudioFifo();

    bool Write(const AVFrame *frame);
    bool Read(AVFrame *frame, int sample_size);
    int size() const;

private:
    int _channels = 0;
    int _samplerate = 0;
    double _tsp = 0;
    double _timebase = 0;
    AVAudioFifo *_fifo = nullptr;
    AVSampleFormat _format = AV_SAMPLE_FMT_NONE;
};

class FFmpegWatermark {
public:
    using Ptr = std::shared_ptr<FFmpegWatermark>;
    explicit FFmpegWatermark(const std::string &watermark_text);
    ~FFmpegWatermark();

    // 初始化滤镜图
    bool init(const AVCodecContext *codec_ctx);

    // 为帧添加水印
    bool addWatermark( AVFrame *frame, AVFrame *output_frame);


    void save_avframe_to_yuv(AVFrame *frame);

private:
    std::string watermark_text_;
    AVFilterContext *buffersrc_ctx = nullptr;
    AVFilterContext *buffersink_ctx = nullptr;
    AVFilterGraph *filter_graph = nullptr;
    AVIOContext * avio_ctx_ = nullptr;
};//// class FFmpegWatermark end

class FFmpegEncoder : public TaskManager, public CodecInfo {
public:
    using Ptr = std::shared_ptr<FFmpegEncoder>;
    using onEnc = std::function<void(const Frame::Ptr &)>;

    FFmpegEncoder(const Track::Ptr &track, int thread_num = 2);
    ~FFmpegEncoder() override;

    void flush();
    CodecId getCodecId() const override { return _codecId; }
    const AVCodecContext *getContext() const { return _context.get(); }

    void setOnEncode(onEnc cb) { _cb = std::move(cb); }
    bool inputFrame(const FFmpegFrame::Ptr &frame, bool async);
    bool inputFrame(AVFrame *frame, bool async);

    void save_avpacket_to_h264(const AVPacket *packet);
private:
    bool inputFrame_l(FFmpegFrame::Ptr frame);
    bool inputFrame_l(AVFrame *frame);
    bool encodeFrame(AVFrame *frame);
    void onEncode(AVPacket *packet);
    bool openVideoCodec(int width, int height, int bitrate, const AVCodec *codec);
    bool openAudioCodec(int samplerate, int channel, int bitrate, const AVCodec *codec);

private:
    onEnc _cb;
    CodecId _codecId;
    const AVCodec *_codec = nullptr;
    AVDictionary *_dict = nullptr;
    std::shared_ptr<AVCodecContext> _context;
    AVIOContext * avio_ctx_ = nullptr;

    std::unique_ptr<FFmpegSws> _sws;
    std::unique_ptr<FFmpegSwr> _swr;
    std::unique_ptr<FFmpegAudioFifo> _fifo;
    bool var_frame_size = false;
};

Frame::Ptr convertAVPacketToFrame(const AVPacket *packet, CodecId codecId, TrackType trackType, const AVCodecContext *codecContext);
CodecId getCodecIdFromContext(const std::shared_ptr<AVCodecContext> _encoder_context);
TrackType getTrackTypeFromContext(const std::shared_ptr<AVCodecContext> _encoder_context);
CodecId convertCodecId(AVCodecID avCodecId);

void save_packet_to_yuv(const Frame::Ptr &frame);
}//namespace mediakit
#endif// ENABLE_FFMPEG
#endif //ZLMEDIAKIT_TRANSCODE_H
