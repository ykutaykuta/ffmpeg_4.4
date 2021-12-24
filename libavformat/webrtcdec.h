#ifndef AVFORMAT_WEBRTCDEC_H
#define AVFORMAT_WEBRTCDEC_H

#include "avformat.h"
#include "url.h"

typedef struct WebrtcDemuxContext
{
    const AVClass *av_class;
    int nal_length_size;
    URLContext *webrtc_hd;
    uint8_t *recvbuf;
    int recvbuf_size;
    int v_stream_index;
    int a_stream_index;
    uint32_t v_start_ts;
    uint32_t a_start_ts;
    uint8_t *fragment_unit;
    int fragment_len;
    int nal_prefix_len; // exclude nalu start code
} WebrtcDemuxContext;

#endif /* AVFORMAT_WEBRTCDEC_H */
