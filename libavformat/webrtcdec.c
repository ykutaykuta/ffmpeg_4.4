#include "webrtcdec.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "network.h"
#include "libavcodec/get_bits.h"

#define RTP_MIN_HEADER_SIZE 12

#define RECVBUF_SIZE 10240

static int parse_rtp_buffer_to_packet_internal(WebrtcDemuxContext *ctx, AVPacket *pkt, const uint8_t *buf, int len)
{
    GetBitContext gb;
    int version, padding, extension, CSRC_count, marker, payload_type;
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t SSRC_identifier;
    AVStream *st;
    int rv = 0, curr_pos = 0;

    init_get_bits(&gb, buf, RTP_MIN_HEADER_SIZE * 8);

    version = get_bits(&gb, 2);
    if (version != 2)
        return AVERROR(EINVAL);
    padding = get_bits1(&gb);
    extension = get_bits1(&gb);
    CSRC_count = get_bits(&gb, 4);
    marker = get_bits1(&gb);
    payload_type = get_bits(&gb, 7);
    sequence_number = get_bits(&gb, 16);
    timestamp = get_bits_long(&gb, 32);
    SSRC_identifier = get_bits_long(&gb, 32);
    len -= RTP_MIN_HEADER_SIZE;
    curr_pos += RTP_MIN_HEADER_SIZE;

    av_log(NULL, AV_LOG_INFO, "ykuta %s: version: %u padding: %u extension: %u CSRC_count: %u marker: %u payload_type: %u sequence_number: %u timestamp: %u SSRC_identifier: %u\n",
           __func__, version, padding, extension, CSRC_count, marker, payload_type, sequence_number, timestamp, SSRC_identifier);

    len -= 4 * CSRC_count;
    curr_pos += 4 * CSRC_count;

    /* RFC 3550 Section 5.3.1 RTP Header Extension handling */
    if (extension)
    {
        uint16_t extension_header_ID = AV_RB16(buf + curr_pos);
        len -= 2;
        curr_pos += 2;

        uint16_t extension_header_length = AV_RB16(buf + curr_pos);
        len -= 2;
        curr_pos += 2;

        if (len < 0)
            return AVERROR(EINVAL);

        len -= extension_header_length * 4;
        curr_pos += extension_header_length * 4;
    }

    if (padding)
    {
        int padding_size = buf[len - 1];
        len -= padding_size;
        av_log(NULL, AV_LOG_INFO, "ykuta padding_size: %d\n", padding_size);
    }

    if (len < 0)
        return AVERROR(EINVAL);

    pkt->dts = timestamp;
    switch (payload_type)
    {
    case 102:
        if ((rv = av_new_packet(pkt, len + 4)) < 0)
            return rv;
        pkt->stream_index = ctx->v_stream_index;
        pkt->data[0] = pkt->data[1] = pkt->data[2] = 0;
        pkt->data[3] = 1;
        memcpy(pkt->data + 4, buf + curr_pos, len);
        break;
    case 111:
        if ((rv = av_new_packet(pkt, len)) < 0)
            return rv;
        pkt->stream_index = ctx->a_stream_index;
        memcpy(pkt->data, buf + curr_pos, len);
        break;
    default:
        break;
    }
    av_log(NULL, AV_LOG_INFO, "ykuta pkt->data %d %d %d %d %d len: %d\n", buf[curr_pos], buf[curr_pos + 1], buf[curr_pos + 2], buf[curr_pos + 3], buf[curr_pos + 4], len);

    return 0;
}

static int webrtc_read_probe(const AVProbeData *p)
{
    if (av_strstart(p->filename, "webrtc:", NULL))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int webrtc_read_header(AVFormatContext *s)
{
    int ret;
    AVStream *v_st, *a_st;
    URLContext *in = NULL;
    WebrtcDemuxContext *ctx = s->priv_data;

    if (!ff_network_init())
        return AVERROR(EIO);

    ret = ffurl_open_whitelist(&in, s->url, AVIO_FLAG_READ, &s->interrupt_callback, NULL, s->protocol_whitelist, s->protocol_blacklist, NULL);
    if (ret)
        goto fail;
    ctx->webrtc_hd = in;

    ctx->recvbuf_size = RECVBUF_SIZE;
    ctx->recvbuf = av_malloc(ctx->recvbuf_size);
    if (!ctx->recvbuf)
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    v_st = avformat_new_stream(s, NULL);
    a_st = avformat_new_stream(s, NULL);
    ctx->v_stream_index = 0;
    ctx->a_stream_index = 1;
    if (!v_st || !a_st)
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    v_st->id = 0;
    v_st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    v_st->codecpar->codec_id = AV_CODEC_ID_H264;

    a_st->id = 1;
    a_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    a_st->codecpar->codec_id = AV_CODEC_ID_OPUS;

    return ret;
fail:
    ffurl_closep(&in);
    ff_network_close();
    av_freep(&ctx->recvbuf);
    return ret;
}

static int webrtc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WebrtcDemuxContext *ctx = s->priv_data;
    int ret, info;
    ret = ffurl_read(ctx->webrtc_hd, ctx->recvbuf, ctx->recvbuf_size);
    if (ret < 0)
        return ret;
    // info = ctx->recvbuf[0];

    return parse_rtp_buffer_to_packet_internal(ctx, pkt, ctx->recvbuf + 5, ret);
    // if (info == AVMEDIA_TYPE_VIDEO)
    // {
    //     pkt->stream_index = ctx->v_stream_index;
    // }
    // else if (info == AVMEDIA_TYPE_AUDIO)
    // {
    //     pkt->stream_index = ctx->a_stream_index;
    // }

    // return ret;
}

static int webrtc_read_close(AVFormatContext *s)
{
    WebrtcDemuxContext *ctx = s->priv_data;
    ffurl_closep(&ctx->webrtc_hd);
    ff_network_close();
    av_freep(&ctx->recvbuf);
    return 0;
}

static const AVClass webrtc_demuxer_class = {
    .class_name = "Webrtc demuxer",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_webrtc_demuxer = {
    .name = "webrtc",
    .long_name = NULL_IF_CONFIG_SMALL("Webrtc input"),
    .priv_data_size = sizeof(WebrtcDemuxContext),
    .read_probe = webrtc_read_probe,
    .read_header = webrtc_read_header,
    .read_packet = webrtc_read_packet,
    .read_close = webrtc_read_close,
    .flags = AVFMT_NOFILE,
    .priv_class = &webrtc_demuxer_class,
};
