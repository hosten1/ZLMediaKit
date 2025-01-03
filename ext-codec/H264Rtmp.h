﻿/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_H264RTMPCODEC_H
#define ZLMEDIAKIT_H264RTMPCODEC_H

#include "H264.h"
#include "Rtmp/RtmpCodec.h"
#include "Extension/Track.h"
#include "src/Codec/Transcode.h"
namespace mediakit {
/**
 * h264 Rtmp解码类
 * 将 h264 over rtmp 解复用出 h264-Frame
 * h264 Rtmp decoder class
 * Demultiplex h264-Frame from h264 over rtmp
 
 * [AUTO-TRANSLATED:4908a1f3]
 */
class H264RtmpDecoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H264RtmpDecoder>;

    H264RtmpDecoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * 输入264 Rtmp包
     * @param rtmp Rtmp包
     * Input 264 Rtmp package
     * @param rtmp Rtmp package
     
     * [AUTO-TRANSLATED:06f3e94c]
     */
    void inputRtmp(const RtmpPacket::Ptr &rtmp) override;

private:
    void outputFrame(const char *data, size_t len, uint32_t dts, uint32_t pts);
    void splitFrame(const uint8_t *data, size_t size, uint32_t dts, uint32_t pts);
};

/**
 * 264 Rtmp打包类
 * 264 Rtmp packaging class
 
 * [AUTO-TRANSLATED:e5bc7c66]
 */
class H264RtmpEncoder : public RtmpCodec {
public:
    using Ptr = std::shared_ptr<H264RtmpEncoder>;

    /**
     * 构造函数，track可以为空，此时则在inputFrame时输入sps pps
     * 如果track不为空且包含sps pps信息，
     * 那么inputFrame时可以不输入sps pps
     * @param track
     * Constructor, track can be empty, in which case sps pps is input when inputFrame
     * If track is not empty and contains sps pps information,
     * then sps pps can be omitted when inputFrame
     * @param track
     
     * [AUTO-TRANSLATED:e61fdfed]
     */
    H264RtmpEncoder(const Track::Ptr &track) : RtmpCodec(track) {}

    /**
     * 输入264帧，可以不带sps pps
     * @param frame 帧数据
     * Input 264 frame, sps pps can be omitted
     * @param frame Frame data
     
     * [AUTO-TRANSLATED:caefd055]
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 刷新输出所有frame缓存
     * Flush all frame cache output
     
     * [AUTO-TRANSLATED:adaea568]
     */
    void flush() override;

    /**
     * 生成config包
     * Generate config package
     
     
     * [AUTO-TRANSLATED:8f851364]
     */
    void makeConfigPacket() override;

private:
   bool inputFrame_l_water(const Track::Ptr & track,const Frame::Ptr &frame);
    RtmpPacket::Ptr _rtmp_packet;
    FrameMerger _merger { FrameMerger::mp4_nal_size };

    FFmpegDecoder::Ptr _decoder;  // FFmpeg 解码器实例
    FFmpegEncoder::Ptr _encoder;  // FFmpeg 编器实例
    FFmpegWatermark::Ptr _watermark;  // FFmpeg 水印
};

}//namespace mediakit

#endif //ZLMEDIAKIT_H264RTMPCODEC_H
