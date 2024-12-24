/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_FFMPEG)
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include "Util/File.h"
#include "Util/uv_errno.h"
#include "Transcode.h"
#include "Common/config.h"
#define MAX_DELAY_SECOND 3
#include "Extension/Factory.h"
using namespace std;
using namespace toolkit;


namespace mediakit {

static string ffmpeg_err(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

std::shared_ptr<AVPacket> alloc_av_packet() {
    auto pkt = std::shared_ptr<AVPacket>(av_packet_alloc(), [](AVPacket *pkt) {
        av_packet_free(&pkt);
    });
    pkt->data = NULL;    // packet data will be allocated by the encoder
    pkt->size = 0;
    return pkt;
}

//////////////////////////////////////////////////////////////////////////////////////////
static void on_ffmpeg_log(void *ctx, int level, const char *fmt, va_list args) {
    GET_CONFIG(bool, enable_ffmpeg_log, General::kEnableFFmpegLog);
    if (!enable_ffmpeg_log) {
        return;
    }
    LogLevel lev;
    switch (level) {
        case AV_LOG_FATAL: lev = LError; break;
        case AV_LOG_ERROR: lev = LError; break;
        case AV_LOG_WARNING: lev = LWarn; break;
        case AV_LOG_INFO: lev = LInfo; break;
        case AV_LOG_VERBOSE: lev = LDebug; break;
        case AV_LOG_DEBUG: lev = LDebug; break;
        case AV_LOG_TRACE: lev = LTrace; break;
        default: lev = LTrace; break;
    }
    LoggerWrapper::printLogV(::toolkit::getLogger(), lev, __FILE__, ctx ? av_default_item_name(ctx) : "NULL", level, fmt, args);
}

static bool setupFFmpeg_l() {
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_flags(AV_LOG_PRINT_LEVEL);
    av_log_set_callback(on_ffmpeg_log);
#if (LIBAVCODEC_VERSION_MAJOR < 58)
    avcodec_register_all();
#endif
    return true;
}

static void setupFFmpeg() {
    static auto flag = setupFFmpeg_l();
}

static bool checkIfSupportedNvidia_l() {
#if !defined(_WIN32)
    GET_CONFIG(bool, check_nvidia_dev, General::kCheckNvidiaDev);
    if (!check_nvidia_dev) {
        return false;
    }
    auto so = dlopen("libnvcuvid.so.1", RTLD_LAZY);
    if (!so) {
        WarnL << "libnvcuvid.so.1加载失败:" << get_uv_errmsg();
        return false;
    }
    dlclose(so);

    bool find_driver = false;
    File::scanDir("/dev", [&](const string &path, bool is_dir) {
        if (!is_dir && start_with(path, "/dev/nvidia")) {
            // 找到nvidia的驱动  [AUTO-TRANSLATED:5b87bf81]
            // Find the Nvidia driver
            find_driver = true;
            return false;
        }
        return true;
    }, false);

    if (!find_driver) {
        WarnL << "英伟达硬件编解码器驱动文件 /dev/nvidia* 不存在";
    }
    return find_driver;
#else
    return false;
#endif
}

static bool checkIfSupportedNvidia() {
    static auto ret = checkIfSupportedNvidia_l();
    return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

bool TaskManager::addEncodeTask(function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            WarnL << "encoder thread task is too more, now drop frame!";
            _task.pop_front();
        }
    }
    _sem.post();
    return true;
}

bool TaskManager::addDecodeTask(bool key_frame, function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        if (_decode_drop_start) {
            if (!key_frame) {
                TraceL << "decode thread drop frame";
                return false;
            }
            _decode_drop_start = false;
            InfoL << "decode thread stop drop frame";
        }

        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            _decode_drop_start = true;
            WarnL << "decode thread start drop frame";
        }
    }
    _sem.post();
    return true;
}

void TaskManager::setMaxTaskSize(size_t size) {
    CHECK(size >= 3 && size <= 1000, "async task size limited to 3 ~ 1000, now size is:", size);
    _max_task = size;
}

void TaskManager::startThread(const string &name) {
    _thread.reset(new thread([this, name]() {
        onThreadRun(name);
    }), [](thread *ptr) {
        ptr->join();
        delete ptr;
    });
}

void TaskManager::stopThread(bool drop_task) {
    TimeTicker();
    if (!_thread) {
        return;
    }
    {
        lock_guard<mutex> lck(_task_mtx);
        if (drop_task) {
            _exit = true;
            _task.clear();
        }
        _task.emplace_back([]() {
            throw ThreadExitException();
        });
    }
    _sem.post(10);
    _thread = nullptr;
}

TaskManager::~TaskManager() {
    stopThread(true);
}

bool TaskManager::isEnabled() const {
    return _thread.operator bool();
}

void TaskManager::onThreadRun(const string &name) {
    setThreadName(name.data());
    function<void()> task;
    _exit = false;
    while (!_exit) {
        _sem.wait();
        {
            unique_lock<mutex> lck(_task_mtx);
            if (_task.empty()) {
                continue;
            }
            task = _task.front();
            _task.pop_front();
        }

        try {
            TimeTicker2(50, TraceL);
            task();
            task = nullptr;
        } catch (ThreadExitException &ex) {
            break;
        } catch (std::exception &ex) {
            WarnL << ex.what();
            continue;
        } catch (...) {
            WarnL << "catch one unknown exception";
            throw;
        }
    }
    InfoL << name << " exited!";
}

//////////////////////////////////////////////////////////////////////////////////////////

FFmpegFrame::FFmpegFrame(std::shared_ptr<AVFrame> frame) {
    if (frame) {
        _frame = std::move(frame);
    } else {
        _frame.reset(av_frame_alloc(), [](AVFrame *ptr) {
            av_frame_free(&ptr);
        });
    }
}

FFmpegFrame::~FFmpegFrame() {
    if (_data) {
        delete[] _data;
        _data = nullptr;
    }
}

AVFrame *FFmpegFrame::get() const {
    return _frame.get();
}

void FFmpegFrame::fillPicture(AVPixelFormat target_format, int target_width, int target_height) {
    assert(_data == nullptr);
    _data = new char[av_image_get_buffer_size(target_format, target_width, target_height, 32)];
    av_image_fill_arrays(_frame->data, _frame->linesize, (uint8_t *) _data,  target_format, target_width, target_height, 32);
}

///////////////////////////////////////////////////////////////////////////

template<bool decoder = true>
static inline const AVCodec *getCodec_l(const char *name) {
    auto codec = decoder ? avcodec_find_decoder_by_name(name) : avcodec_find_encoder_by_name(name);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << name;
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << name << " not found";
    }
    return codec;
}

template<bool decoder = true>
static inline const AVCodec *getCodec_l(enum AVCodecID id) {
    auto codec = decoder ? avcodec_find_decoder(id) : avcodec_find_encoder(id);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << avcodec_get_name(id);
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << avcodec_get_name(id) << " not found";
    }
    return codec;
}

class CodecName {
public:
    CodecName(string name) : _codec_name(std::move(name)) {}
    CodecName(enum AVCodecID id) : _id(id) {}

    template <bool decoder>
    const AVCodec *getCodec() const {
        if (!_codec_name.empty()) {
            return getCodec_l<decoder>(_codec_name.data());
        }
        return getCodec_l<decoder>(_id);
    }

private:
    string _codec_name;
    enum AVCodecID _id;
};

template <bool decoder = true>
static inline const AVCodec *getCodec(const std::initializer_list<CodecName> &codec_list) {
    const AVCodec *ret = nullptr;
    for (int i = codec_list.size(); i >= 1; --i) {
        ret = codec_list.begin()[i - 1].getCodec<decoder>();
        if (ret) {
            return ret;
        }
    }
    return ret;
}

template<bool decoder = true>
static inline const AVCodec *getCodecByName(const std::vector<std::string> &codec_list) {
    const AVCodec *ret = nullptr;
    for (auto &codec : codec_list) {
        ret = getCodec_l<decoder>(codec.data());
        if (ret) {
            return ret;
        }
    }
    return ret;
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track, int thread_num, const std::vector<std::string> &codec_name) {
    setupFFmpeg();
    const AVCodec *codec = nullptr;
    const AVCodec *codec_default = nullptr;
    if (!codec_name.empty()) {
        codec = getCodecByName(codec_name);
    }
    switch (track->getCodecId()) {
        case CodecH264:
            codec_default = getCodec({AV_CODEC_ID_H264});
            if (codec && codec->id == AV_CODEC_ID_H264) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_cuvid"}, {"h264_nvmpi"}});
            } else {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_nvmpi"}});
            }
            break;
        case CodecH265:
            codec_default = getCodec({AV_CODEC_ID_HEVC});
            if (codec && codec->id == AV_CODEC_ID_HEVC) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_cuvid"}, {"hevc_nvmpi"}});
            } else {
                codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_nvmpi"}});
            }
            break;
        case CodecAAC:
            if (codec && codec->id == AV_CODEC_ID_AAC) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_AAC});
            break;
        case CodecG711A:
            if (codec && codec->id == AV_CODEC_ID_PCM_ALAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_ALAW});
            break;
        case CodecG711U:
            if (codec && codec->id == AV_CODEC_ID_PCM_MULAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_MULAW});
            break;
        case CodecOpus:
            if (codec && codec->id == AV_CODEC_ID_OPUS) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_OPUS});
            break;
        case CodecJPEG:
            if (codec && codec->id == AV_CODEC_ID_MJPEG) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_MJPEG});
            break;
        case CodecVP8:
            if (codec && codec->id == AV_CODEC_ID_VP8) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP8});
            break;
        case CodecVP9:
            if (codec && codec->id == AV_CODEC_ID_VP9) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP9});
            break;
        default: codec = nullptr; break;
    }

    codec = codec ? codec : codec_default;
    if (!codec) {
        throw std::runtime_error("未找到解码器");
    }

    while (true) {
        _decoder_context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
            avcodec_free_context(&ctx);
        });

        if (!_decoder_context) {
            throw std::runtime_error("创建解码器失败");
        }

        // 保存AVFrame的引用  [AUTO-TRANSLATED:2df53d07]
        // Save the AVFrame reference
#ifdef FF_API_OLD_ENCDEC
        _decoder_context->refcounted_frames = 1;
#endif
        _decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        _decoder_context->flags2 |= AV_CODEC_FLAG2_FAST;
        if (track->getTrackType() == TrackVideo) {
            _decoder_context->width = static_pointer_cast<VideoTrack>(track)->getVideoWidth();
            _decoder_context->height = static_pointer_cast<VideoTrack>(track)->getVideoHeight();
        }

        switch (track->getCodecId()) {
            case CodecG711A:
            case CodecG711U: {
                AudioTrack::Ptr audio = static_pointer_cast<AudioTrack>(track);
                _decoder_context->channels = audio->getAudioChannel();
                _decoder_context->sample_rate = audio->getAudioSampleRate();
                _decoder_context->channel_layout = av_get_default_channel_layout(_decoder_context->channels);
                break;
            }
            default:
                break;
        }
        AVDictionary *dict = nullptr;
        if (thread_num <= 0) {
            av_dict_set(&dict, "threads", "auto", 0);
        } else {
            av_dict_set(&dict, "threads", to_string(MIN((unsigned int)thread_num, thread::hardware_concurrency())).data(), 0);
        }
        av_dict_set(&dict, "zerolatency", "1", 0);
        av_dict_set(&dict, "strict", "-2", 0);

#ifdef AV_CODEC_CAP_TRUNCATED
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            /* we do not send complete frames */
            _decoder_context->flags |= AV_CODEC_FLAG_TRUNCATED;
            _do_merger = false;
        } else {
            // 此时业务层应该需要合帧  [AUTO-TRANSLATED:8dea0fff]
            // The business layer should need to merge frames at this time
            _do_merger = true;
        }
#endif

        int ret = avcodec_open2(_decoder_context.get(), codec, &dict);
        av_dict_free(&dict);
        if (ret >= 0) {
            // 成功  [AUTO-TRANSLATED:7d878ca9]
            // Success
            InfoL << "打开解码器成功:" << codec->name;
            break;
        }

        if (codec_default && codec_default != codec) {
            // 硬件编解码器打开失败，尝试软件的  [AUTO-TRANSLATED:060200f4]
            // Hardware codec failed to open, try software codec
            WarnL << "打开解码器" << codec->name << "失败，原因是:" << ffmpeg_err(ret) << ", 再尝试打开解码器" << codec_default->name;
            codec = codec_default;
            continue;
        }
        throw std::runtime_error(StrPrinter << "打开解码器" << codec->name << "失败:" << ffmpeg_err(ret));
    }
}

FFmpegDecoder::~FFmpegDecoder() {
    stopThread(true);
    if (_do_merger) {
        _merger.flush();
    }
    flush();
}

void FFmpegDecoder::flush() {
    while (true) {
        auto out_frame = std::make_shared<FFmpegFrame>();
        auto ret = avcodec_receive_frame(_decoder_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN)) {
            avcodec_send_packet(_decoder_context.get(), nullptr);
            continue;
        }
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        onDecode(out_frame);
    }
}

const AVCodecContext *FFmpegDecoder::getContext() const {
    return _decoder_context.get();
}
const AVCodecContext *FFmpegDecoder::getDecoderContext() const {
    return getContext();
}

bool FFmpegDecoder::inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge) {
    if (_do_merger && enable_merge) {
        return _merger.inputFrame(frame, [this, live](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
            decodeFrame(buffer->data(), buffer->size(), dts, pts, live, have_idr);
        });
    }

    return decodeFrame(frame->data(), frame->size(), frame->dts(), frame->pts(), live, frame->keyFrame());
}

bool FFmpegDecoder::inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge) {
    if (async && !TaskManager::isEnabled() && getContext()->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 开启异步编码，且为视频，尝试启动异步解码线程  [AUTO-TRANSLATED:17a68fc6]
        // Enable asynchronous encoding, and it is video, try to start asynchronous decoding thread
        startThread("decoder thread");
    }

    if (!async || !TaskManager::isEnabled()) {
        return inputFrame_l(frame, live, enable_merge);
    }

    auto frame_cache = Frame::getCacheAbleFrame(frame);
    return addDecodeTask(frame->keyFrame(), [this, live, frame_cache, enable_merge]() {
        inputFrame_l(frame_cache, live, enable_merge);
        // 此处模拟解码太慢导致的主动丢帧  [AUTO-TRANSLATED:fc8bea8a]
        // Here simulates decoding too slow, resulting in active frame dropping
        //usleep(100 * 1000);
    });
}

bool FFmpegDecoder::decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame) {
    TimeTicker2(30, TraceL);

    auto pkt = alloc_av_packet();
    pkt->data = (uint8_t *) data;
    pkt->size = size;
    pkt->dts = dts;
    pkt->pts = pts;
    if (key_frame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    auto ret = avcodec_send_packet(_decoder_context.get(), pkt.get());
    if (ret < 0) {
        if (ret != AVERROR_INVALIDDATA) {
            WarnL << "avcodec_send_packet failed:" << ffmpeg_err(ret);
        }
        return false;
    }

    while (true) {
        AVFrame *temp_frame = av_frame_alloc();
        auto out_frame = std::make_shared<FFmpegFrame>();
        ret = avcodec_receive_frame(_decoder_context.get(), temp_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        

        if (live && pts - temp_frame->pts > MAX_DELAY_SECOND * 1000 && _ticker.createdTime() > 10 * 1000) {
            // 后面的帧才忽略,防止Track无法ready  [AUTO-TRANSLATED:23f1a7c9]
            // The following frames are ignored to prevent the Track from being ready
            WarnL << "解码时，忽略" << MAX_DELAY_SECOND << "秒前的数据:" << pts << " " << temp_frame->pts;
            continue;
        }
        // if(_watermark == nullptr){
        //     _watermark = std::make_shared<FFmpegWatermark>("dddd");
        // }
        
        // _watermark->save_avframe_to_yuv(temp_frame);
        onDecode(temp_frame);
        av_frame_free(&temp_frame);
    }
    return true;
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}
void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDecAvframe cb) {
    _cb_avframe = std::move(cb);
}

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame) {
    if (_cb) {
        _cb(frame);
    }
}
void FFmpegDecoder::onDecode(const AVFrame *frame) {
    if (_cb_avframe) {
        _cb_avframe(frame);
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegSwr::FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate) {
    _target_format = output;
    _target_channels = channel;
    _target_channel_layout = channel_layout;
    _target_samplerate = samplerate;
}

FFmpegSwr::~FFmpegSwr() {
    if (_ctx) {
        swr_free(&_ctx);
    }
}

FFmpegFrame::Ptr FFmpegSwr::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (frame->get()->format == _target_format &&
        frame->get()->channels == _target_channels &&
        frame->get()->channel_layout == (uint64_t)_target_channel_layout &&
        frame->get()->sample_rate == _target_samplerate) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (!_ctx) {
        _ctx = swr_alloc_set_opts(nullptr, _target_channel_layout, _target_format, _target_samplerate,
                                  frame->get()->channel_layout, (AVSampleFormat) frame->get()->format,
                                  frame->get()->sample_rate, 0, nullptr);
        InfoL << "swr_alloc_set_opts:" << av_get_sample_fmt_name((enum AVSampleFormat) frame->get()->format) << " -> "
              << av_get_sample_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = std::make_shared<FFmpegFrame>();
        out->get()->format = _target_format;
        out->get()->channel_layout = _target_channel_layout;
        out->get()->channels = _target_channels;
        out->get()->sample_rate = _target_samplerate;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;

        int ret = 0;
        if (0 != (ret = swr_convert_frame(_ctx, out->get(), frame->get()))) {
            WarnL << "swr_convert_frame failed:" << ffmpeg_err(ret);
            return nullptr;
        }
        return out;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegSws::FFmpegSws(AVPixelFormat output, int width, int height) {
    _target_format = output;
    _target_width = width;
    _target_height = height;
}

FFmpegSws::~FFmpegSws() {
    if (_ctx) {
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
}

int FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data) {
    int ret;
    inputFrame(frame, ret, data);
    return ret;
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame) {
    int ret;
    return inputFrame(frame, ret, nullptr);
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data) {
    ret = -1;
    TimeTicker2(30, TraceL);
    auto target_width = _target_width ? _target_width : frame->get()->width;
    auto target_height = _target_height ? _target_height : frame->get()->height;
    if (frame->get()->format == _target_format && frame->get()->width == target_width && frame->get()->height == target_height) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (_ctx && (_src_width != frame->get()->width || _src_height != frame->get()->height || _src_format != (enum AVPixelFormat) frame->get()->format)) {
        // 输入分辨率发生变化了  [AUTO-TRANSLATED:0e4ea2e8]
        // Input resolution has changed
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
    if (!_ctx) {
        _src_format = (enum AVPixelFormat) frame->get()->format;
        _src_width = frame->get()->width;
        _src_height = frame->get()->height;
        _ctx = sws_getContext(frame->get()->width, frame->get()->height, (enum AVPixelFormat) frame->get()->format, target_width, target_height, _target_format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        InfoL << "sws_getContext:" << av_get_pix_fmt_name((enum AVPixelFormat) frame->get()->format) << " -> " << av_get_pix_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = std::make_shared<FFmpegFrame>();
        if (!out->get()->data[0]) {
            if (data) {
                av_image_fill_arrays(out->get()->data, out->get()->linesize, data, _target_format, target_width, target_height, 32);
            } else {
                out->fillPicture(_target_format, target_width, target_height);
            }
        }
        if (0 >= (ret = sws_scale(_ctx, frame->get()->data, frame->get()->linesize, 0, frame->get()->height, out->get()->data, out->get()->linesize))) {
            WarnL << "sws_scale failed:" << ffmpeg_err(ret);
            return nullptr;
        }

        out->get()->format = _target_format;
        out->get()->width = target_width;
        out->get()->height = target_height;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;
        return out;
    }
    return nullptr;
}


FFmpegWatermark::FFmpegWatermark(const std::string &watermark_text) 
    : watermark_text_(watermark_text) {
    avfilter_register_all();
}

FFmpegWatermark::~FFmpegWatermark() {
    if (buffersink_ctx) avfilter_free(buffersink_ctx);
    if (buffersrc_ctx) avfilter_free(buffersrc_ctx);
    if (filter_graph) avfilter_graph_free(&filter_graph);
    if ( avio_ctx_ )avio_close(avio_ctx_);
}

bool FFmpegWatermark::init(const AVCodecContext *codec_ctx) {
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    filter_graph = avfilter_graph_alloc();
    std::string filter_desc = "drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf:text=" + watermark_text_ +
                              ":x=(w-tw):y=(2*lh):fontsize=24:fontcolor=red";
    int ret = 0;
    if (!inputs || !outputs || !filter_graph) {
        std::cerr << "Could not allocate filter graph resources" << std::endl;
        ret = AVERROR(ENOMEM);
        goto end;
    }

    char args[512];
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
             codec_ctx->time_base.num, codec_ctx->time_base.den,
             codec_ctx->sample_aspect_ratio.num, codec_ctx->sample_aspect_ratio.den);
    std::cerr << "Buffer source args: " << args << std::endl;

    if (avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, nullptr, filter_graph) < 0) {
        std::cerr << "Could not create buffer source" << std::endl;
        goto end;
    }

    if (avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", nullptr, nullptr, filter_graph) < 0) {
        std::cerr << "Could not create buffer sink" << std::endl;
        goto end;;
    }
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
 
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;   

    if (ret = avfilter_graph_parse_ptr(filter_graph, filter_desc.c_str(), &inputs, &outputs, nullptr) < 0) {
        std::cerr << "Could not parse filter graph" << std::endl;
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Error in avfilter_graph_parse_ptr: %s\n", errbuf);
        goto end;
    }

    if (ret = avfilter_graph_config(filter_graph, nullptr) < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "Error in avfilter_graph_config: %s\n", errbuf);
        std::cerr << "Could not configure filter graph" << std::endl;
        goto end;
    }
    // avfilter_inout_free(&inputs);
    // avfilter_inout_free(&outputs);
    return true;
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return false;
}

bool FFmpegWatermark::addWatermark( AVFrame *frame, AVFrame *output_frame) {
    // std::cout << "addWatermark frame->width="<< frame->width  << " frame->height="<< frame->height << std::endl;
    if (av_buffersrc_add_frame(buffersrc_ctx, frame) < 0) {
        std::cerr << "Error while feeding frame to filter graph" << std::endl;
        return false;
    }

    if (av_buffersink_get_frame(buffersink_ctx, output_frame) < 0) {
        std::cerr << "Error while getting frame from filter graph" << std::endl;
        return false;
    }
    // save_avframe_to_yuv(output_frame);
    return true;
}
void FFmpegWatermark::save_avframe_to_yuv(AVFrame *frame) {
    if (!frame) {
        fprintf(stderr, "Invalid frame or directory.\n");
        return;
    }
    if(frame->width <= 0 || frame->height <= 0){
        fprintf(stderr, "Invalid frame or directory.\n");
        return;
    }
    if (avio_ctx_ == nullptr){
         std::string current_file = "/home/lym_work/ZLMediaKit/release/linux/Debug/www/record";
        //    std::string current_file = File::absolutePath(abFile,nullptr,true);
            // 获取时间并生成文件名
            char filename[256];
            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            snprintf(
                filename, sizeof(filename),
                "%s/frame_%dx%d_%s_%04d%02d%02d%02d%02d%02d.yuv",
                current_file.c_str(),                // 确保使用 c_str()
                frame->width, frame->height,
                av_get_pix_fmt_name((AVPixelFormat)frame->format),
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec
            );
            std::cerr << "save_avframe_to_yuv filename: " << filename << std::endl;
            // 使用 FFmpeg 的写入工具打开文件
            if (avio_open(&avio_ctx_, filename, AVIO_FLAG_WRITE) < 0) {
                fprintf(stderr, "Failed to open file: %s\n", filename);
                return;
            }

    }
   

    // 写入 YUV 数据，根据具体像素格式处理
    switch (frame->format) {
        case AV_PIX_FMT_YUV420P:
            for (int i = 0; i < frame->height; ++i) {
                avio_write(avio_ctx_, frame->data[0] + i * frame->linesize[0], frame->width); // Y 平面
            }
            for (int i = 0; i < frame->height / 2; ++i) {
                avio_write(avio_ctx_, frame->data[1] + i * frame->linesize[1], frame->width / 2); // U 平面
            }
            for (int i = 0; i < frame->height / 2; ++i) {
                avio_write(avio_ctx_, frame->data[2] + i * frame->linesize[2], frame->width / 2); // V 平面
            }
            break;
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_NV21: {
            for (int i = 0; i < frame->height; ++i) {
                avio_write(avio_ctx_, frame->data[0] + i * frame->linesize[0], frame->width); // Y 平面
            }
            int chroma_height = frame->height / 2;
            int chroma_width = frame->width;
            for (int i = 0; i < chroma_height; ++i) {
                avio_write(avio_ctx_, frame->data[1] + i * frame->linesize[1], chroma_width); // UV 平面（交错存储）
            }
            break;
        }
        // 如果需要支持其他格式，可在此添加更多 case
        default:
            fprintf(stderr, "Unsupported pixel format: %s\n", av_get_pix_fmt_name((AVPixelFormat)frame->format));
            avio_close(avio_ctx_);
            return;
    }
    // printf("Frame saved to %s\n", filename);
}
FFmpegEncoder::FFmpegEncoder(const Track::Ptr &track, int thread_num, const std::vector<std::string> &codec_name) {
    setupFFmpeg();

    const AVCodec *codec = nullptr;
    const AVCodec *codec_default = nullptr;

    // 根据名称优先选择编码器
    if (!codec_name.empty()) {
        codec = getCodecByName<false>(codec_name);
    }

    // 根据轨道类型选择默认编码器
    switch (track->getCodecId()) {
        case CodecH264:
            codec_default = getCodec<false>({AV_CODEC_ID_H264});
            if (!codec || codec->id != AV_CODEC_ID_H264) {
                codec = getCodec<false>({{"libx264"}, {AV_CODEC_ID_H264}, {"h264_nvenc"}, {"h264_qsv"}, {"h264_vaapi"}});
            }
            break;
        case CodecH265:
            codec_default = getCodec<false>({AV_CODEC_ID_HEVC});
            if (!codec || codec->id != AV_CODEC_ID_HEVC) {
                codec = getCodec<false>({{"libx265"}, {AV_CODEC_ID_HEVC}, {"hevc_nvenc"}, {"hevc_qsv"}, {"hevc_vaapi"}});
            }
            break;
        case CodecAAC:
            codec_default = getCodec<false>({AV_CODEC_ID_AAC});
            if (!codec || codec->id != AV_CODEC_ID_AAC) {
                codec = getCodec<false>({{"aac"}, {AV_CODEC_ID_AAC}});
            }
            break;
        default:
            throw std::runtime_error("Unsupported codec for encoding.");
    }

    codec = codec ? codec : codec_default;
    if (!codec) {
        throw std::runtime_error("Failed to find suitable encoder.");
    }

    _encoder_context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
        avcodec_free_context(&ctx);
    });

    if (!_encoder_context) {
        throw std::runtime_error("Failed to allocate codec context.");
    }

    // 设置线程数
    if (thread_num <= 0) {
        _encoder_context->thread_count = thread::hardware_concurrency();
    } else {
        _encoder_context->thread_count = thread_num;
    }

    // 根据轨道类型设置编码器参数
    if (track->getTrackType() == TrackVideo) {
        auto videoTrack = static_pointer_cast<VideoTrack>(track);
        _encoder_context->width = videoTrack->getVideoWidth();
        _encoder_context->height = videoTrack->getVideoHeight();
        _encoder_context->time_base = {1, (int)videoTrack->getVideoFps()};
        _encoder_context->framerate = av_make_q(videoTrack->getVideoFps(), 1);
        _encoder_context->gop_size = 50;  // 设置关键帧间隔为 50
        _encoder_context->max_b_frames = 0;  // 禁用 B 帧
        _encoder_context->pix_fmt = AV_PIX_FMT_YUV420P;
    } else if (track->getTrackType() == TrackAudio) {
        auto audioTrack = static_pointer_cast<AudioTrack>(track);
        _encoder_context->sample_rate = audioTrack->getAudioSampleRate();
        _encoder_context->channels = audioTrack->getAudioChannel();
        _encoder_context->channel_layout = av_get_default_channel_layout(audioTrack->getAudioChannel());
        _encoder_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    } else {
        throw std::runtime_error("Unsupported track type for encoding.");
    }

    // 打开编码器
    AVDictionary *dict = nullptr;
    av_dict_set(&dict, "preset", "ultrafast", 0);
    av_dict_set(&dict, "tune", "zerolatency", 0);

    if (avcodec_open2(_encoder_context.get(), codec, &dict) < 0) {
        av_dict_free(&dict);
        throw std::runtime_error(StrPrinter << "Failed to open encoder: " << codec->name);
    }

    av_dict_free(&dict);
    InfoL << "Encoder opened successfully: " << codec->name;
}

FFmpegEncoder::~FFmpegEncoder() {
    flush();
}

bool FFmpegEncoder::inputFrame(const AVFrame *frame, bool live, bool async, bool enable_merge) {
     if (async && !TaskManager::isEnabled() && getEncodeContext()->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 开启异步编码，且为视频，尝试启动异步解码线程  [AUTO-TRANSLATED:17a68fc6]
        // Enable asynchronous encoding, and it is video, try to start asynchronous decoding thread
        startThread("decoder thread");
    }

    if (!async || !TaskManager::isEnabled()) {
        return inputFrame_l(frame, live, enable_merge);
    }

    // auto frame_cache = Frame::getCacheAbleFrame(frame);
    return addEncodeTask([this, live, frame, enable_merge]() {
        inputFrame_l(frame, live, enable_merge);
        // 此处模拟解码太慢导致的主动丢帧  [AUTO-TRANSLATED:fc8bea8a]
        // Here simulates decoding too slow, resulting in active frame dropping
        //usleep(100 * 1000);
    });
}

bool FFmpegEncoder::inputFrame_l(const AVFrame *frame, bool live, bool enable_merge) {
    if (!_encoder_context) {
        WarnL << "Encoder context is not initialized.";
        return false;
    }

    if (avcodec_send_frame(_encoder_context.get(), frame) < 0) {
        WarnL << "Failed to send frame to encoder.";
        return false;
    }

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        WarnL << "Failed to allocate AVPacket.";
        return false;
    }

    int ret = 0;
    while ((ret = avcodec_receive_packet(_encoder_context.get(), packet)) == 0) {
        onEncode(packet);
        // WarnL << "  Stream Index: %d " << packet->stream_index 
        //         << "   Duration: %lld " << packet->duration
        //         << " Size: %d bytes = " << packet->size;
        save_avpacket_to_h264(packet);

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        WarnL << "Error during encoding: " << ret;
        return false;
    }

    return true;
}

void FFmpegEncoder::setOnEncode(onEncAvframe cb) {
    _cb = std::move(cb);
}

void FFmpegEncoder::onEncode(const AVPacket *packet) {
    if (_cb) {
        //从_encoder_context获取_, CodecId codecId, TrackType trackType
        CodecId codecId     = getCodecIdFromContext(_encoder_context);
        TrackType trackType = getTrackTypeFromContext(_encoder_context);


        if (codecId == CodecInvalid || trackType == TrackInvalid) {
            ErrorL << "Invalid codec or track type";
            return;
        }
        _cb(packet,codecId,trackType);
    }
}

void FFmpegEncoder::flush() {
    if (!_encoder_context) {
        return;
    }

    avcodec_send_frame(_encoder_context.get(), nullptr);

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return;
    }

    while (avcodec_receive_packet(_encoder_context.get(), packet) == 0) {
        onEncode(packet);
        // save_avpacket_to_h264(packet);
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

const AVCodecContext *FFmpegEncoder::getEncodeContext() const {
    return _encoder_context.get();
}

void FFmpegEncoder::save_avpacket_to_h264(const AVPacket *packet) {
    if (!packet || packet->size <= 0) {
        fprintf(stderr, "Invalid packet or size.\n");
        return;
    }
    // Create a copy of the AVPacket
        AVPacket *packet_copy = av_packet_alloc();
        if (!packet_copy) {
            fprintf(stderr, "Failed to allocate AVPacket.\n");
            return;
        }

        if (av_packet_ref(packet_copy, packet) < 0) {
            fprintf(stderr, "Failed to copy AVPacket.\n");
            av_packet_free(&packet_copy);
            return;
        }
    if (avio_ctx_ == nullptr) {
        std::string current_file = "/home/lym_work/ZLMediaKit/release/linux/Debug/www/record";

        // Generate filename based on timestamp
        char filename[256];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        // 获取 this 指针的十六进制地址
        uintptr_t id = reinterpret_cast<uintptr_t>(this);
        snprintf(
            filename, sizeof(filename),
            "%s/encoded_%04d%02d%02d%02d%02d%02d_%p.h264",
            current_file.c_str(),
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec,
            reinterpret_cast<void*>(id)
        );
        
        std::cerr << "save_avpacket_to_h264 filename: " << filename << std::endl;

        // Open file for writing using FFmpeg's avio API
        if (avio_open(&avio_ctx_, filename, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Failed to open file: %s\n", filename);
            return;
        }
    }

    // Write the H264 packet data
    if (avio_ctx_) {
        avio_write(avio_ctx_, packet_copy->data, packet_copy->size);
    } else {
        fprintf(stderr, "AVIO context is not initialized.\n");
    }
      // Free the copied packet
    av_packet_free(&packet_copy);
}


Frame::Ptr convertAVPacketToFrame(const AVPacket *packet, CodecId codecId, TrackType trackType, const AVCodecContext *codecContext) {
    if (!packet || !packet->data || packet->size <= 0) {
        WarnL << "Invalid AVPacket";
        return nullptr;
    }

    if (!codecContext) {
        WarnL << "Invalid AVCodecContext";
        return nullptr;
    }

    // 从 codecContext 获取时间基并转换时间戳
    AVRational time_base = codecContext->time_base;
    uint64_t dts = av_rescale_q(packet->dts, time_base, AVRational{1, 1000});
    uint64_t pts = av_rescale_q(packet->pts, time_base, AVRational{1, 1000});

    // // 计算缓冲区大小
    // size_t sps_pps_size = 0;
    // size_t nal_start_code_size = 4;
    // size_t packet_data_size = packet->size;

    // if ((packet->flags & AV_PKT_FLAG_KEY) && codecContext->extradata && codecContext->extradata_size > 0) {
    //     sps_pps_size = nal_start_code_size + codecContext->extradata_size;
    // }
    // size_t total_size = sps_pps_size + nal_start_code_size + packet_data_size;

    // // 分配内存缓冲区
    // char *data = new char[total_size];
    // char *ptr = data; // 用于操作内存的指针

    // // 如果是关键帧，拷贝 SPS 和 PPS 数据
    // if (sps_pps_size > 0) {
    //     static const char start_code[] = "\x00\x00\x00\x01";
    //     memcpy(ptr, start_code, nal_start_code_size); // 添加起始码
    //     ptr += nal_start_code_size;
    //     memcpy(ptr, codecContext->extradata, codecContext->extradata_size); // 添加 SPS 和 PPS
    //     ptr += codecContext->extradata_size;
    // }

    // // 拷贝当前帧数据
    // static const char start_code[] = "\x00\x00\x00\x01";
    // memcpy(ptr, start_code, nal_start_code_size); // 添加起始码
    // ptr += nal_start_code_size;
    // memcpy(ptr, packet->data, packet_data_size); // 添加帧数据

    // 使用 Factory::getFrameFromPtr 创建 Frame
    auto frame = Factory::getFrameFromPtr(codecId, reinterpret_cast<const char*>(packet->data), packet->size, dts, pts);

    // 手动释放缓冲区（Factory::getFrameFromPtr 内部会拷贝数据）
    // delete[] data;

    return frame;
}

CodecId getCodecIdFromContext(std::shared_ptr<AVCodecContext> _encoder_context) {
    if (!_encoder_context) {
        return CodecInvalid;
    }

    switch (_encoder_context->codec_id) {
        case AV_CODEC_ID_H264:
            return CodecH264;
        case AV_CODEC_ID_AAC:
            return CodecAAC;
        // 添加其他 codec 的映射
        case AV_CODEC_ID_H265:
            return CodecH265;
        default:
            return CodecInvalid;
    }
}

TrackType getTrackTypeFromContext(std::shared_ptr<AVCodecContext> _encoder_context) {
    if (!_encoder_context) {
        return TrackInvalid;
    }

    switch (_encoder_context->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            return TrackVideo;
        case AVMEDIA_TYPE_AUDIO:
            return TrackAudio;
        default:
            return TrackInvalid;
    }
}
} //namespace mediakit
#endif//ENABLE_FFMPEG
