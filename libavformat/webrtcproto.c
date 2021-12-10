#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "avformat.h"
#include "avio_internal.h"
#include "webrtcproto.h"
#include "url.h"
#include "ip.h"

#include <stdarg.h>
#include "internal.h"
#include "network.h"
#include "os_support.h"
#include <fcntl.h>
#if HAVE_POLL_H
#include <poll.h>
#endif

#include "libavutil/json.h"
#include <pthread.h>
#include <rtc/rtc.h>
#include <unistd.h> // for sleep and usleep

#include "libavutil/httpserver.h"

#define API_PUBLISH "/api/publish"
#define API_PLAY "/api/play"
#define BUFF_SIZE 10240
// #define MAX_PKT_SIZE UDP_MAX_PKT_SIZE
#define MAX_PKT_SIZE 1000000

typedef struct Client
{
    bool isHasPeer;
    int pc;
    int audio;
    int video;
    bool isVideoConnected;
    bool isAudioConnected;
    rtcState state;
    rtcGatheringState gatheringState;
    rtcSignalingState signalingState;
} Client;

typedef struct WebrtcContext
{
    const AVClass *class;
    int http_port;
    int nb_clients;
    Client *receivers;
    Client sender;
    char *buff;
    char *buffExtra;
    pthread_mutex_t lock;
    pthread_t http_server_thread;
    pthread_t receiver_release_thread;
    http_server_t *http_server;
    uint32_t video_ssrc;
    uint32_t audio_ssrc;
    uint32_t video_clock_rate;
    uint32_t audio_clock_rate;
    int rtc_log_level;
} WebrtcContext;

#define OFFSET(x) offsetof(WebrtcContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"nb_clients", "Number clients can listen at the same time", OFFSET(nb_clients), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 100, .flags = D | E},
    {"video_ssrc", "SSRC number for video stream", OFFSET(video_ssrc), AV_OPT_TYPE_INT, {.i64 = 1}, 1, UINT_MAX, .flags = D | E},
    {"audio_ssrc", "SSRC number for audio stream", OFFSET(audio_ssrc), AV_OPT_TYPE_INT, {.i64 = 2}, 1, UINT_MAX, .flags = D | E},
    {"video_clock_rate", "Video clock rate", OFFSET(video_clock_rate), AV_OPT_TYPE_INT, {.i64 = 90000}, 1, UINT_MAX, .flags = D | E},
    {"audio_clock_rate", "Audio clock rate", OFFSET(audio_clock_rate), AV_OPT_TYPE_INT, {.i64 = 48000}, 1, UINT_MAX, .flags = D | E},
    {"rtc_log_level", "Set webrtc loglevel 0-6 (None, Fatal, Error, Warning, Info, Debug, Verbose)", OFFSET(rtc_log_level), AV_OPT_TYPE_INT, {.i64 = 6}, 0, 6, .flags = D | E},
    {NULL}};

static const AVClass webrtc_class = {
    .class_name = "webrtc",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

static void String_to_CRLF(char *dest, char *src)
{
    printf("%s\n", __func__);
    int i, j = 0;
    for (i = 0; i < strlen(src); i++)
    {
        if (src[i] == '\\' && src[i + 1] == 'r')
        {
            dest[j++] = 13;
            i++;
            continue;
        }
        else if (src[i] == '\\' && src[i + 1] == 'n')
        {
            dest[j++] = 10;
            i++;
            continue;
        }
        dest[j++] = src[i];
    }
    dest[j] = '\0';
}

static void CRLF_to_String(char *dest, char *src)
{
    printf("%s\n", __func__);
    int i, j = 0;
    for (i = 0; i < strlen(src); i++)
    {
        if (src[i] == 10)
        {
            dest[j++] = '\\';
            dest[j++] = 'n';
            continue;
        }
        else if (src[i] == 13)
        {
            dest[j++] = '\\';
            dest[j++] = 'r';
            continue;
        }
        dest[j++] = src[i];
    }
    dest[j] = '\0';
}

static void *ClientRelease(Client *client)
{
    if (!client->isHasPeer)
    {
        return NULL;
    }
    if (client->video)
    {
        rtcDeleteTrack(client->video);
    }
    if (client->audio)
    {
        rtcDeleteTrack(client->audio);
    }
    if (client->pc)
    {
        rtcDeletePeerConnection(client->pc);
    }
    memset(client, 0, sizeof(Client));
    return NULL;
}

static void RTC_API StateChangeCallbackFunc(int pc, rtcState state, void *ptr)
{
    Client *receiver = (Client *)ptr;
    receiver->state = state;
    if (receiver->state == RTC_DISCONNECTED || receiver->state == RTC_FAILED || receiver->state == RTC_CLOSED)
    {
        pthread_t t;
        pthread_create(&t, NULL, ClientRelease, receiver);
    }
    av_log(NULL, AV_LOG_INFO, "WEBRTC pc: %d state: %d\n", pc, (int)state);
}

static void RTC_API GatheringStateCallbackFunc(int pc, rtcGatheringState state, void *ptr)
{
    Client *receiver = (Client *)ptr;
    receiver->gatheringState = state;
    av_log(NULL, AV_LOG_INFO, "WEBRTC pc: %d gatheringState: %d\n", pc, (int)state);
}

static void RTC_API SignalingStateCallbackFunc(int pc, rtcSignalingState state, void *ptr)
{
    Client *receiver = (Client *)ptr;
    receiver->signalingState = state;
    av_log(NULL, AV_LOG_INFO, "WEBRTC pc: %d signalingState: %d\n", pc, (int)state);
}

static void RTC_API DescriptionCallbackFunc(int pc, const char *sdp, const char *type, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC pc: %d type: %s sdp:\n%s\n", pc, type, sdp);
}

static void RTC_API CandidateCallbackFunc(int pc, const char *cand, const char *mid, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC pc: %d mid: %s cand: %s\n", pc, mid, cand);
}

static void RTC_API TrackOpenCallbackFunc(int id, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC %s id: %d\n", __func__, id);
    Client *client = (Client *)ptr;
    if (id == client->video)
    {
        client->isVideoConnected = true;
    }
    else if (id == client->audio)
    {
        client->isAudioConnected = true;
    }
}

static void RTC_API TrackCloseCallbackFunc(int id, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC %s id: %d\n", __func__, id);
    Client *client = (Client *)ptr;
    if (id == client->video)
    {
        client->isVideoConnected = false;
    }
    else if (id == client->audio)
    {
        client->isAudioConnected = false;
    }
}

static void RTC_API TrackErrorCallbackFunc(int id, const char *error, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC %s id: %d error: %s\n", __func__, id, error);
    Client *client = (Client *)ptr;
    rtcDeleteTrack(id);
    if (id == client->video)
    {
        client->isVideoConnected = false;
        client->video = 0;
    }
    else if (id == client->audio)
    {
        client->isAudioConnected = false;
        client->audio = 0;
    }
    if (!client->isVideoConnected && !client->isAudioConnected)
    {
        ClientRelease(client);
    }
}

#define LOCAL_BUF_SIZE 3072
static void RTC_API TrackCallbackFunc(int pc, int tr, void *ptr)
{
    av_log(NULL, AV_LOG_INFO, "WEBRTC %s pc: %d tr: %d\n", __func__, pc, tr);
    Client *client = (Client *)ptr;
    char buffer[LOCAL_BUF_SIZE];
    int err = rtcGetTrackDescription(tr, buffer, LOCAL_BUF_SIZE);
    if (err >= 0)
    {
        av_log(NULL, AV_LOG_INFO, "ykuta track description\n%s\n", buffer);
        if (strncmp(buffer, "m=video", strlen("m=video")) == 0)
        {
            err = rtcGetTrackPayloadTypesForCodec(tr, "h264", buffer, LOCAL_BUF_SIZE);
            if (err >= 0)
                av_log(NULL, AV_LOG_INFO, "WEBRTC codec h264: %s\n", buffer);
            else
                av_log(NULL, AV_LOG_INFO, "ykuta codec h264 error: %d\n", err);
            client->video = tr;
            client->isVideoConnected = true;
            rtcSetUserPointer(tr, client);
            rtcSetClosedCallback(tr, TrackCloseCallbackFunc);
        }
        else if (strncmp(buffer, "m=audio", strlen("m=audio")) == 0)
        {
            err = rtcGetTrackPayloadTypesForCodec(tr, "opus", buffer, LOCAL_BUF_SIZE);
            if (err >= 0)
                av_log(NULL, AV_LOG_INFO, "WEBRTC codec opus: %s\n", buffer);
            else
                av_log(NULL, AV_LOG_INFO, "ykuta codec opus error: %d\n", err);
            client->audio = tr;
            client->isAudioConnected = true;
            rtcSetUserPointer(tr, client);
            rtcSetClosedCallback(tr, TrackCloseCallbackFunc);
        }
        rtcStartRtcpSenderReporterRecording(tr);
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "WEBRTC rtcGetTrackDescription error: %d\n", err);
    }
}

static void RtcInitPeerConnection(Client *client)
{
    rtcConfiguration pcConfig;
    memset(&pcConfig, 0, sizeof(rtcConfiguration));
    ClientRelease(client);
    client->pc = rtcCreatePeerConnection(&pcConfig);
    rtcSetUserPointer(client->pc, client);
    rtcSetStateChangeCallback(client->pc, StateChangeCallbackFunc);
    rtcSetGatheringStateChangeCallback(client->pc, GatheringStateCallbackFunc);
    rtcSetSignalingStateChangeCallback(client->pc, SignalingStateCallbackFunc);
    rtcSetLocalDescriptionCallback(client->pc, DescriptionCallbackFunc);
    rtcSetLocalCandidateCallback(client->pc, CandidateCallbackFunc);
    client->isHasPeer = true;
}

static void RecevierInit(WebrtcContext *ctx, Client *receiver)
{
    rtcTrackInit videoInit = {.direction = RTC_DIRECTION_SENDONLY,
                              .codec = RTC_CODEC_H264,
                              .payloadType = 102,
                              .ssrc = ctx->video_ssrc,
                              .mid = "video-stream",
                              .name = "video-stream",
                              .msid = "stream1",
                              .trackId = "video-stream"};
    rtcTrackInit audioInit = {.direction = RTC_DIRECTION_SENDONLY,
                              .codec = RTC_CODEC_OPUS,
                              .payloadType = 111,
                              .ssrc = ctx->audio_ssrc,
                              .mid = "audio-stream",
                              .name = "audio-stream",
                              .msid = "stream1",
                              .trackId = "audio-stream"};
    rtcPacketizationHandlerInit videoPktHandlerInit = {.ssrc = ctx->video_ssrc,
                                                       .cname = "video-stream",
                                                       .payloadType = 102,
                                                       .clockRate = ctx->video_clock_rate,
                                                       .maxFragmentSize = RTC_DEFAULT_MAXIMUM_FRAGMENT_SIZE,
                                                       .sequenceNumber = 0,
                                                       .timestamp = 0};
    rtcPacketizationHandlerInit audioPktHandlerInit = {.ssrc = ctx->audio_ssrc,
                                                       .cname = "audio-stream",
                                                       .payloadType = 111,
                                                       .clockRate = ctx->audio_clock_rate,
                                                       .maxFragmentSize = RTC_DEFAULT_MAXIMUM_FRAGMENT_SIZE,
                                                       .sequenceNumber = 0,
                                                       .timestamp = 0};

    RtcInitPeerConnection(receiver);

    receiver->video = rtcAddTrackEx(receiver->pc, &videoInit);
    rtcSetUserPointer(receiver->video, receiver);
    rtcSetOpenCallback(receiver->video, TrackOpenCallbackFunc);
    rtcSetClosedCallback(receiver->video, TrackCloseCallbackFunc);
    rtcSetErrorCallback(receiver->video, TrackErrorCallbackFunc);
    rtcSetH264PacketizationHandler(receiver->video, &videoPktHandlerInit);
    rtcChainRtcpSrReporter(receiver->video);
    rtcChainRtcpNackResponder(receiver->video, RTC_DEFAULT_MAXIMUM_PACKET_COUNT_FOR_NACK_CACHE);

    receiver->audio = rtcAddTrackEx(receiver->pc, &audioInit);
    rtcSetUserPointer(receiver->audio, receiver);
    rtcSetOpenCallback(receiver->audio, TrackOpenCallbackFunc);
    rtcSetClosedCallback(receiver->audio, TrackCloseCallbackFunc);
    rtcSetErrorCallback(receiver->audio, TrackErrorCallbackFunc);
    rtcSetOpusPacketizationHandler(receiver->audio, &audioPktHandlerInit);
    rtcChainRtcpSrReporter(receiver->audio);
    rtcChainRtcpNackResponder(receiver->audio, RTC_DEFAULT_MAXIMUM_PACKET_COUNT_FOR_NACK_CACHE);
}

static void SenderInit(WebrtcContext *ctx, Client *sender)
{
    RtcInitPeerConnection(sender);
    rtcSetTrackCallback(sender->pc, TrackCallbackFunc);
}

static bool WebrtcHandleApiPlay(WebrtcContext *ctx)
{
    int pos;
    Client *receiver;
    for (pos = 0; pos < ctx->nb_clients; pos++)
    {
        if (!ctx->receivers[pos].isHasPeer)
        {
            break;
        }
    }
    if (pos == ctx->nb_clients)
    {
        return false;
    }
    receiver = &ctx->receivers[pos];
    RecevierInit(ctx, receiver);
    if (rtcSetRemoteDescription(receiver->pc, ctx->buff, "offer") < 0)
    {
        return false;
    }
    while (receiver->gatheringState != RTC_GATHERING_COMPLETE)
    {
        usleep(100000);
    }
    if (rtcGetLocalDescription(receiver->pc, ctx->buff, BUFF_SIZE) < 0)
    {
        ClientRelease(receiver);
        return false;
    }
    else
    {
        CRLF_to_String(ctx->buffExtra, ctx->buff);
        sprintf(ctx->buff, "{\"type\":\"answer\",\"sdp\":\"%s\"}", ctx->buffExtra);
    }
    return true;
}

static bool WebrtcHandleApiPublish(WebrtcContext *ctx)
{
    av_log(NULL, AV_LOG_INFO, "ykuta %s\n", __func__);
    Client *sender;
    sender = &(ctx->sender);
    if (sender->isHasPeer)
        return false;
    SenderInit(ctx, sender);
    if (rtcSetRemoteDescription(sender->pc, ctx->buff, "offer") < 0)
    {
        return false;
    }
    while (sender->gatheringState != RTC_GATHERING_COMPLETE)
    {
        usleep(100000);
    }
    if (rtcGetLocalDescription(sender->pc, ctx->buff, BUFF_SIZE) < 0)
    {
        ClientRelease(sender);
        return false;
    }
    else
    {
        CRLF_to_String(ctx->buffExtra, ctx->buff);
        sprintf(ctx->buff, "{\"type\":\"answer\",\"sdp\":\"%s\"}", ctx->buffExtra);
    }
    return true;
}

static void HandleRequest(http_request_t *request)
{
    WebrtcContext *ctx = (WebrtcContext *)request->server->userPtr;
    int method, contentLength, i;
    char *api = NULL, *body = NULL;
    JSONObject *obj = NULL;

    char *buf = request->stream.buf, *tmp1, *tmp2;
    http_response_t *response = http_response_init();

    // process first line of request
    char *token = strtok_r(buf, "\n", &buf);
    if (strncmp(token, "GET", strlen("GET")) == 0)
    {
        method = METHOD_GET;
    }
    else if (strncmp(token, "POST", strlen("POST")) == 0)
    {
        method = METHOD_POST;
    }
    else if (strncmp(token, "OPTIONS", strlen("OPTIONS")) == 0)
    {
        http_response_status(response, RES_NO_CONTENT);
        http_response_header(response, "Access-Control-Allow-Origin", "*");
        http_response_header(response, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
        http_response_header(response, "Access-Control-Allow-Headers", "*");
        http_response_header(response, "Access-Control-Max-Age", "86400");
        goto end;
    }
    else
    {
        http_response_status(response, RES_BAD_RESQUEST);
        goto end;
    }
    tmp1 = strchr(token, 32);
    tmp1++;
    tmp2 = strchr(tmp1, 32);
    *tmp2 = '\0';
    api = av_mallocz(strlen(tmp1) + 1);
    if (!api)
    {
        http_response_status(response, RES_INTERNAL_SERVER_ERROR);
        goto end;
    }
    strcpy(api, tmp1);
    *tmp2 = 32;

    // process the remain line to get body
    while (token = strtok_r(buf, "\n", &buf))
    {
        if (strncmp(token, "Content-Length", strlen("Content-Length")) == 0)
        {
            tmp1 = strchr(token, 32);
            tmp1++;
            tmp2 = strchr(tmp1, 13);
            *tmp2 = '\0';
            contentLength = atoi(tmp1);
            *tmp2 = 13;
            break;
        }
    }
    body = request->stream.buf + request->stream.length - contentLength;

    obj = parseJSON(body);
    for (i = 0; i < obj->count; i++)
    {
        if (strncmp(obj->pairs[i].key, "type", strlen("type")) == 0)
        {
            if (strncmp(obj->pairs[i].value->stringValue, "offer", strlen("offer")) != 0)
            {
                http_response_status(response, RES_BAD_RESQUEST);
                goto end;
            }
        }
        else if (strncmp(obj->pairs[i].key, "sdp", strlen("sdp")) == 0)
        {
            String_to_CRLF(ctx->buff, obj->pairs[i].value->stringValue);
        }
    }

    if (strncmp(api, API_PLAY, strlen(API_PLAY)) == 0)
    {
        if (WebrtcHandleApiPlay(ctx))
        {
            http_response_status(response, RES_OK);
            http_response_header(response, "Access-Control-Allow-Origin", "*");
            http_response_header(response, "Content-Type", "application/json");
            http_response_body(response, ctx->buff, strlen(ctx->buff));
        }
        else
        {
            http_response_status(response, RES_INTERNAL_SERVER_ERROR);
        }
    }
    else if (strncmp(api, API_PUBLISH, strlen(API_PUBLISH)) == 0)
    {
        if (WebrtcHandleApiPublish(ctx))
        {
            http_response_status(response, RES_OK);
            http_response_header(response, "Access-Control-Allow-Origin", "*");
            http_response_header(response, "Content-Type", "application/json");
            http_response_body(response, ctx->buff, strlen(ctx->buff));
        }
        else
        {
            http_response_status(response, RES_INTERNAL_SERVER_ERROR);
        }
    }
    else
    {
        http_response_status(response, RES_NOT_IMPLEMENTED);
    }

end:
    http_respond(request, response);
    if (api)
    {
        av_freep(&api);
    }
    freeJSONFromMemory(obj);
}

static void *StartHTTP(void *vargp)
{
    WebrtcContext *ctx = (WebrtcContext *)vargp;
    ctx->http_server = http_server_init(ctx->http_port, HandleRequest, vargp);
    http_server_listen(ctx->http_server);
    return NULL;
}

static void RTC_API LogCallbackFunc(rtcLogLevel level, const char *message)
{
    int _level;
    if (level == RTC_LOG_VERBOSE)
    {
        _level = AV_LOG_VERBOSE;
    }
    else if (level == RTC_LOG_DEBUG)
    {
        _level = AV_LOG_DEBUG;
    }
    else if (level == RTC_LOG_INFO)
    {
        _level = AV_LOG_INFO;
    }
    else if (level == RTC_LOG_WARNING)
    {
        _level = AV_LOG_WARNING;
    }
    else if (level == RTC_LOG_ERROR)
    {
        _level = AV_LOG_ERROR;
    }
    else if (level == RTC_LOG_FATAL)
    {
        _level = AV_LOG_FATAL;
    }
    else
    {
        _level = AV_LOG_PANIC;
    }
    av_log(NULL, _level, "WEBRTC: %s\n", message);
}

static int WebrtcReadMessage(WebrtcContext *ctx, const char *data, int size)
{
    Client *sender = &(ctx->sender);
    int ret = 0;
    const char *buf = data + 5;
    char *info = data;
    char *dts_us = data + 1;
    int max_size = size - 5;
    while (true)
    {
        if (sender->isHasPeer && sender->state == RTC_CONNECTED)
        {
            if (sender->isVideoConnected)
            {
                ret = rtcReceiveMessage(sender->video, buf, &max_size);
                if (ret == RTC_ERR_SUCCESS)
                {
                    ret = max_size;
                    info[0] = AVMEDIA_TYPE_VIDEO;
                    break;
                }
                else if (ret != RTC_ERR_NOT_AVAIL)
                {
                    break;
                }
            }

            if (sender->isAudioConnected)
            {
                ret = rtcReceiveMessage(sender->audio, buf, &max_size);
                if (ret == RTC_ERR_SUCCESS)
                {
                    ret = max_size;
                    info[0] = AVMEDIA_TYPE_AUDIO;
                    break;
                }
                else if (ret != RTC_ERR_NOT_AVAIL)
                {
                    break;
                }
            }
        }
        usleep(100000);
    }
    return ret;
}

static void WebrtcSendMessage(WebrtcContext *ctx, const char *data, int size, uint32_t time_us, bool isVideo)
{
    Client *receiver;
    int i, id;
    uint32_t timestamp, start_timestamp;
    uint32_t previousReportTimeStamp;
    double delta_s;
    // pthread_mutex_lock(&ctx->lock);
    for (i = 0; i < ctx->nb_clients; i++)
    {
        receiver = &(ctx->receivers[i]);
        if (receiver->isHasPeer)
        {
            if (receiver->state != RTC_CONNECTED)
            {
                continue;
            }
            id = -1;
            if (isVideo && receiver->isVideoConnected)
            {
                id = receiver->video;
            }
            else if (!isVideo && receiver->isAudioConnected)
            {
                id = receiver->audio;
            }
            if (id < 0)
            {
                continue;
            }
            rtcTransformSecondsToTimestamp(id, (double)time_us / 1000000, &timestamp);
            rtcGetTrackStartTimestamp(id, &start_timestamp);
            timestamp += start_timestamp;
            rtcSetTrackRtpTimestamp(id, timestamp);
            rtcGetPreviousTrackSenderReportTimestamp(id, &previousReportTimeStamp);
            rtcTransformTimestampToSeconds(id, timestamp - previousReportTimeStamp, &delta_s);
            if (delta_s > 1)
            {
                rtcSetNeedsToSendRtcpSr(id);
            }
            rtcSendMessage(id, data, size);
        }
    }
    // pthread_mutex_unlock(&ctx->lock);
}

/**
 * url syntax: webrtc://host:port[?option=val...]
 * option: 'nb_clients=n'            : Number clients can listen at the same time
 */

static int webrtc_open(URLContext *h, const char *uri, int flags)
{
    WebrtcContext *ctx = h->priv_data;
    char hostname[256];
    char buf[1024];
    char path[1024];
    const char *p;
    int ret = 0, http_port;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &http_port, path, sizeof(path), uri);

    /* extract parameters */
    p = strchr(uri, '?');
    if (p)
    {
        if (av_find_info_tag(buf, sizeof(buf), "nv_clients", p))
        {
            ctx->nb_clients = strtol(buf, NULL, 10);
        }
        else if (av_find_info_tag(buf, sizeof(buf), "loglevel", p))
        {
            ctx->rtc_log_level = strtol(buf, NULL, 10);
        }
    }

    ctx->http_port = http_port;
    ctx->nb_clients = ctx->nb_clients;
    // rtcPreload();
    // rtcCleanup();
    rtcInitLogger((rtcLogLevel)ctx->rtc_log_level, LogCallbackFunc);
    ctx->receivers = av_calloc(ctx->nb_clients, sizeof(Client));
    if (!ctx->receivers)
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ctx->buff = (char *)av_calloc(1, BUFF_SIZE);
    if (!ctx->buff)
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ctx->buffExtra = (char *)av_calloc(1, BUFF_SIZE);
    if (!ctx->buffExtra)
    {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    pthread_mutex_init(&ctx->lock, NULL);
    pthread_create(&ctx->http_server_thread, NULL, StartHTTP, (void *)ctx);

    h->max_packet_size = MAX_PKT_SIZE;
    h->is_streamed = 1;
    return ret;

fail:
    av_freep(&ctx->http_server);
    av_freep(&ctx->receivers);
    av_freep(&ctx->buff);
    av_freep(&ctx->buffExtra);

    return ret;
}

static int webrtc_read(URLContext *h, uint8_t *buf, int size)
{
    int ret;
    WebrtcContext *ctx = h->priv_data;
    ret = WebrtcReadMessage(ctx, buf, size);
    if (ret < 0)
        ret = AVERROR(EIO);
    return ret;
}

static int webrtc_write(URLContext *h, const uint8_t *buf, int size)
{
    int info, length;
    uint32_t time_us;
    const uint8_t *data;
    WebrtcContext *ctx = h->priv_data;
    if (size < 5)
    {
        av_log(h, AV_LOG_WARNING, "Data have size too small! Skip");
        return size;
    }
    info = (int)buf[0];
    time_us = (uint32_t)buf[1] << 24 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 8 | (uint32_t)buf[4];
    data = buf + 5;
    length = size - 5;

    if (info == AVMEDIA_TYPE_AUDIO)
    {
        WebrtcSendMessage(ctx, data, length, time_us, false);
    }
    else if (info == AVMEDIA_TYPE_VIDEO)
    {
        WebrtcSendMessage(ctx, data, length, time_us, true);
    }
    return size;
}

static int webrtc_close(URLContext *h)
{
    WebrtcContext *ctx = h->priv_data;
    pthread_cancel(ctx->http_server_thread);
    av_freep(&ctx->http_server);
    av_freep(&ctx->receivers);
    av_freep(&ctx->buff);
    av_freep(&ctx->buffExtra);
    pthread_mutex_destroy(&ctx->lock);

    return 0;
}

const URLProtocol ff_webrtc_protocol = {
    .name = "webrtc",
    .url_open = webrtc_open,
    .url_read = webrtc_read,
    .url_write = webrtc_write,
    .url_close = webrtc_close,
    .priv_data_size = sizeof(WebrtcContext),
    .flags = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class = &webrtc_class,
};
