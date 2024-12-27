/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "Rtmp/utils.h"
#include "H264Rtmp.h"

using namespace std;
using namespace toolkit;

namespace mediakit {
// 创建 AVFrame 的自定义管理器
std::shared_ptr<AVFrame> createAVFrame() {
    AVFrame* raw_frame = av_frame_alloc();  // 使用 FFmpeg 的分配函数
    if (!raw_frame) {
        throw std::runtime_error("Failed to allocate AVFrame");
    }
    return std::shared_ptr<AVFrame>(raw_frame, [](AVFrame* frame) {
        av_frame_free(&frame);  // 自定义删除器，确保内存安全释放
    });
}
void H264RtmpDecoder::inputRtmp(const RtmpPacket::Ptr &pkt) {
    if (pkt->isConfigFrame()) {
        CHECK_RET(pkt->size() > 5);
        getTrack()->setExtraData((uint8_t *)pkt->data() + 5, pkt->size() - 5);
        return;
    }

    CHECK_RET(pkt->size() > 9);
    uint8_t *cts_ptr = (uint8_t *)(pkt->buffer.data() + 2);
    int32_t cts = (((cts_ptr[0] << 16) | (cts_ptr[1] << 8) | (cts_ptr[2])) + 0xff800000) ^ 0xff800000;
    auto pts = pkt->time_stamp + cts;
    splitFrame((uint8_t *)pkt->data() + 5, pkt->size() - 5, pkt->time_stamp, pts);
}

void H264RtmpDecoder::splitFrame(const uint8_t *data, size_t size, uint32_t dts, uint32_t pts) {
    auto end = data + size;
    while (data + 4 < end) {
        uint32_t frame_len = load_be32(data);
        data += 4;
        if (data + frame_len > end) {
            break;
        }
        outputFrame((const char *)data, frame_len, dts, pts);
        data += frame_len;
    }
}

void H264RtmpDecoder::outputFrame(const char *data, size_t len, uint32_t dts, uint32_t pts) {
    auto frame = FrameImp::create<H264Frame>();
    frame->_prefix_size = 4;
    frame->_dts = dts;
    frame->_pts = pts;
    frame->_buffer.assign("\x00\x00\x00\x01", 4); // 添加264头
    frame->_buffer.append(data, len);
    RtmpCodec::inputFrame(frame);
}

////////////////////////////////////////////////////////////////////////

void H264RtmpEncoder::flush() {
    inputFrame(nullptr);
}

bool H264RtmpEncoder::inputFrame(const Frame::Ptr &frame) {
    if (!_rtmp_packet) {
        _rtmp_packet = RtmpPacket::create();
        // flags/not config/cts预占位  [AUTO-TRANSLATED:7effb692]
        // flags/not config/cts placeholder
        _rtmp_packet->buffer.resize(5);
    }

      inputFrame_l_water(getTrack(),frame);

    return true;
}

void H264RtmpEncoder::makeConfigPacket() {
    auto flags = (uint8_t)RtmpVideoCodec::h264;
    flags |= ((uint8_t)RtmpFrameType::key_frame << 4);
    auto pkt = RtmpPacket::create();
    // header
    pkt->buffer.push_back(flags);
    pkt->buffer.push_back((uint8_t)RtmpH264PacketType::h264_config_header);
    // cts
    pkt->buffer.append("\x0\x0\x0", 3);
    // AVCDecoderConfigurationRecord start
    auto extra_data = getTrack()->getExtraData();
    CHECK(extra_data);
    pkt->buffer.append(extra_data->data(), extra_data->size());

    pkt->body_size = pkt->buffer.size();
    pkt->chunk_id = CHUNK_VIDEO;
    pkt->stream_index = STREAM_MEDIA;
    pkt->time_stamp = 0;
    pkt->type_id = MSG_VIDEO;
    RtmpCodec::inputRtmp(pkt);
}
bool H264RtmpEncoder::inputFrame_l_water(const Track::Ptr & track,const Frame::Ptr &frame){
    if(!frame)return false;
    if (_decoder == nullptr) {
        try {
             // 获取当前对象的 shared_ptr
                // auto self = shared_from_this();
                std::vector<std::string> codec_names = {"h264"};
               _encoder = std::make_shared<FFmpegEncoder>(track, 1);
               _encoder->setOnEncode([=](const Frame::Ptr & encode_frame) {
                  if(!encode_frame->data() || encode_frame->size() <= 0){
                     std::cerr << "Failed to Encode encode_frame =" <<  encode_frame << std::endl;

                    return;
                  }
                    // // 转换 AVPacket 到 FrameImp
                        // const AVCodecContext *codecContext = _encoder->getEncodeContext(); // 包含 SPS/PPS 数据
			            // auto waterFrame = convertAVPacketToFrame(packet, codecId, trackType,codecContext);
                    
                    // bool retB = _merger.inputFrame(encode_frame, [this,encode_frame](uint64_t dts, uint64_t pts, const Buffer::Ptr &, bool have_key_frame) {
                    //     // flags
                    //     _rtmp_packet->buffer[0] = (uint8_t)RtmpVideoCodec::h264 | ((uint8_t)(have_key_frame ? RtmpFrameType::key_frame : RtmpFrameType::inter_frame) << 4);
                    //     _rtmp_packet->buffer[1] = (uint8_t)RtmpH264PacketType::h264_nalu;
                    //     int32_t cts = pts - dts;
                    //     // cts
                    //     set_be24(&_rtmp_packet->buffer[2], cts);
                    //     _rtmp_packet->time_stamp = dts;
                    //     _rtmp_packet->body_size = _rtmp_packet->buffer.size();
                    //     _rtmp_packet->chunk_id = CHUNK_VIDEO;
                    //     _rtmp_packet->stream_index = STREAM_MEDIA;
                    //     _rtmp_packet->type_id = MSG_VIDEO;
                    //     // 输出rtmp packet  [AUTO-TRANSLATED:d72e89a7]
                    //     // Output rtmp packet
                    //     RtmpCodec::inputRtmp(_rtmp_packet);
                    //     _rtmp_packet = nullptr;
                    // }, &_rtmp_packet->buffer);
                  
               });
                _decoder = std::make_shared<FFmpegDecoder>(track, 1);
                _decoder->setOnDecode([=](const AVFrame * avframe) {
                         AVFrame *modifiable_frame = av_frame_clone(avframe);
                        if (!modifiable_frame) {
                            fprintf(stderr, "Failed to clone frame.\n");
                            return ;
                        }
                         if (_watermark == nullptr)
                         {
                             _watermark = std::make_shared<FFmpegWatermark>("SampleWatermark");
                            if (!_watermark->init(_decoder->getContext())) {
                            std::cerr << "Failed to initialize watermark filter" << std::endl;
                            return ;
                            }
                         }
                       
                        // auto output_frame = createAVFrame();
                        AVFrame *output_frame = av_frame_alloc();
                        if (_watermark->addWatermark(modifiable_frame, output_frame)) {
//                            std::cout << "Watermark added successfully" << std::endl;
                        } else {
                            std::cerr << "Failed to add watermark" << std::endl;
                        }
                       
                    // std::cout << "Decoded frame callback triggered output_frame->width="<< output_frame->width 
                    //                                             << " output_frame->height="<< output_frame->height
                    //                                             << std::endl;
                    // auto out_frame = std::make_shared<FFmpegFrame>(output_frame);
                     _encoder->inputFrame(output_frame,true);     
                     av_frame_free(&output_frame);
                     av_frame_free(&modifiable_frame);
                });
              

               
            // auto self = shared_from_this(); // 从当前对象获取 std::shared_ptr
            // auto future = std::async(std::launch::async, [this, self]() {
              
            //     return _decoder;
            // });

            // // 等待解码器完成初始化
            // future.get();

        } catch (const std::runtime_error &e) {
            std::cerr << "捕获到异常: " << e.what() << std::endl;
        }

        WarnL << "H264Track Decoded init success";
    }
    // 调用 FFmpegDecoder 尝试解码
   if (_decoder) {
        bool decoded = _decoder->inputFrame(frame, true, false);
        if (!decoded) {
            WarnL << "Failed to decode frame, PTS=" << frame->pts();
        }
   }
    return true;
}

}//namespace mediakit
