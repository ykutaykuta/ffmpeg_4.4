#ifndef AVFORMAT_WEBRTCENC_H
#define AVFORMAT_WEBRTCENC_H

#include "avformat.h"

typedef struct WebrtcMuxContext
{
    const AVClass *av_class;
    int nal_length_size;
} WebrtcMuxContext;

#endif /* AVFORMAT_WEBRTCENC_H */