#ifndef AVFORMAT_WEBRTCDEC_H
#define AVFORMAT_WEBRTCDEC_H


#include "avformat.h"
#include "url.h"

typedef struct WebrtcDemuxContext
{
    const AVClass *av_class;
    int nal_length_size;
    URLContext *webrtc_hd;
    uint8_t * recvbuf;
    int recvbuf_size;
    int v_stream_index;
    int a_stream_index;
} WebrtcDemuxContext;


#endif /* AVFORMAT_WEBRTCDEC_H */
