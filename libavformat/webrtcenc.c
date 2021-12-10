#include "avformat.h"
#include "internal.h"
#include "avc.h"
#include "webrtcenc.h"

static const AVClass webrtc_muxer_class = {
    .class_name = "Webrtc muxer",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

static void send_nal(AVFormatContext *s, const uint8_t *start, int size, unsigned char info, uint32_t time_us)
{
    avio_w8(s->pb, info);
    avio_wb32(s->pb, time_us);
    avio_wb32(s->pb, size); // must writing using big endian
    avio_write(s->pb, start, size);
    avio_flush(s->pb);
}

static void send_raw(AVFormatContext *s, const uint8_t *start, int size, unsigned char info, uint32_t time_us)
{
    avio_w8(s->pb, info);
    avio_wb32(s->pb, time_us);
    avio_write(s->pb, start, size);
    avio_flush(s->pb);
}

static void webrtc_send_h264(AVFormatContext *s, const uint8_t *data, int size, uint32_t time_us)
{
    const uint8_t *r, *r1, *end = data + size;
    WebrtcMuxContext *ctx = s->priv_data;
    if (ctx->nal_length_size)
        r = ff_avc_mp4_find_startcode(data, end, ctx->nal_length_size) ? data : end;
    else
        r = ff_avc_find_startcode(data, end);
    while (r < end)
    {
        if (ctx->nal_length_size)
        {
            r1 = ff_avc_mp4_find_startcode(r, end, ctx->nal_length_size);
            if (!r1)
                r1 = end;
            r += ctx->nal_length_size;
        }
        else
        {
            while (!*(r++))
            {
            }
            r1 = ff_avc_find_startcode(r, end);
        }
        send_nal(s, r, r1 - r, AVMEDIA_TYPE_VIDEO, time_us);
        r = r1;
    }
}

static int webrtc_write_header(AVFormatContext *s)
{
    // force maximun one stream for video and one stream for audio
    WebrtcMuxContext *ctx = s->priv_data;
    AVStream *st;
    int i, nb_video, nb_audio;
    nb_video = 0;
    nb_audio = 0;
    for (i = 0; i < s->nb_streams; i++)
    {
        st = s->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            nb_audio++;
            if (st->codecpar->codec_id == AV_CODEC_ID_OPUS)
            {
                /** 
                 * The opus RTP RFC says that all opus streams should use 48000 Hz
                 * as clock rate, since all opus sample rates can be expressed in
                 * this clock rate, and sample rate changes on the fly are supported. 
                 */
                avpriv_set_pts_info(st, 32, 1, 48000);
            }
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            nb_video++;
            if (st->codecpar->codec_id == AV_CODEC_ID_H264)
            {
                if (st->codecpar->extradata_size > 4 && st->codecpar->extradata[0] == 1)
                {
                    ctx->nal_length_size = (st->codecpar->extradata[4] & 0x03) + 1;
                }
            }
        }
    }
    if (nb_video > 1 || nb_audio > 1)
    {
        av_log(s, AV_LOG_ERROR, "Webrtc muxer must have maximun one video stream and one audio stream\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

static int webrtc_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    AVRational tb = st->time_base;
    uint32_t time_us = pkt->dts * 1000000 * tb.num / tb.den;
    switch (st->codecpar->codec_id)
    {
    case AV_CODEC_ID_H264:
        webrtc_send_h264(s, pkt->data, pkt->size, time_us);
        break;
    case AV_CODEC_ID_OPUS:
        send_raw(s, pkt->data, pkt->size, AVMEDIA_TYPE_AUDIO, time_us);
        break;
    default:
        avio_write(s->pb, pkt->data, pkt->size);
        break;
    }
    return 0;
}

AVOutputFormat ff_webrtc_muxer = {
    .name = "webrtc",
    .long_name = NULL_IF_CONFIG_SMALL("Webrtc output"),
    .audio_codec = AV_CODEC_ID_OPUS,
    .video_codec = AV_CODEC_ID_H264,
    .write_header = webrtc_write_header,
    .write_packet = webrtc_write_packet,
    .priv_data_size = sizeof(WebrtcMuxContext),
    .priv_class = &webrtc_muxer_class,
    .flags = AVFMT_TS_NONSTRICT,
};