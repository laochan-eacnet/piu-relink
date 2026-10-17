/* Guest i386 SysV probe: intentionally exercise piu's legacy packet/decode/picture APIs,
 * including caller-owned AVPacket storage and RGB24 output through swscale. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

static int report_error(const char *operation, int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, text, sizeof(text));
    fprintf(stderr, "%s: %s\n", operation, text);
    return 1;
}

int main(int argc, char **argv)
{
    AVFormatContext *format = NULL;
    AVCodecContext *context = NULL;
    AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVFrame *rgb = NULL;
    struct SwsContext *scale = NULL;
    AVPacket packet;
    uint8_t *pixels = NULL;
    int wanted = argc > 2 ? atoi(argv[2]) : 30;
    int stream, error, frames = 0, width = 0, height = 0, status = 1;
    uint32_t checksum = 2166136261u;
    if (argc < 2 || wanted <= 0) {
        fprintf(stderr, "usage: decode-test MEDIA [FRAMES]\n");
        return 2;
    }
    error = avformat_open_input(&format, argv[1], NULL, NULL);
    if (error < 0) return report_error("avformat_open_input", error);
    error = avformat_find_stream_info(format, NULL);
    if (error < 0) { report_error("avformat_find_stream_info", error); goto done; }
    stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) { report_error("av_find_best_stream", stream); goto done; }
    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    rgb = av_frame_alloc();
    if (!context || !frame || !rgb) goto done;
    error = avcodec_parameters_to_context(context, format->streams[stream]->codecpar);
    if (error < 0) { report_error("avcodec_parameters_to_context", error); goto done; }
    context->thread_count = 2;
    error = avcodec_open2(context, codec, NULL);
    if (error < 0) { report_error("avcodec_open2", error); goto done; }
    width = context->width;
    height = context->height;
    int size = avpicture_get_size(AV_PIX_FMT_RGB24, width, height);
    if (size <= 0 || !(pixels = av_malloc(size))) goto done;
    error = avpicture_fill((AVPicture *)rgb, pixels, AV_PIX_FMT_RGB24, width, height);
    if (error < 0) { report_error("avpicture_fill", error); goto done; }
    scale = sws_getContext(width, height, context->pix_fmt, width, height,
                           AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale) goto done;
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;
    while (frames < wanted && (error = av_read_frame(format, &packet)) >= 0) {
        AVPacket remaining = packet;
        while (remaining.stream_index == stream && remaining.size > 0 && frames < wanted) {
            int got = 0;
            int used = avcodec_decode_video2(context, frame, &got, &remaining);
            if (used < 0) { report_error("avcodec_decode_video2", used); av_packet_unref(&packet); goto done; }
            if (got) {
                int rows = sws_scale(scale, (const uint8_t *const *)frame->data,
                                     frame->linesize, 0, frame->height,
                                     rgb->data, rgb->linesize);
                if (rows != height) { av_packet_unref(&packet); goto done; }
                for (int row = 0; row < height; ++row)
                    for (int col = 0; col < width * 3; ++col)
                        checksum = (checksum ^ rgb->data[0][row * rgb->linesize[0] + col]) * 16777619u;
                ++frames;
                av_frame_unref(frame);
            }
            if (!used) break;
            remaining.data += used;
            remaining.size -= used;
        }
        av_packet_unref(&packet);
    }
    if (frames != wanted) {
        fprintf(stderr, "decoded %d of %d requested frames\n", frames, wanted);
        goto done;
    }
    printf("decoded_frames=%d dimensions=%dx%d output=RGB24 checksum=%08" PRIx32 " codec=%s\n",
           frames, width, height, checksum, codec->name);
    status = 0;
done:
    sws_freeContext(scale);
    av_free(pixels);
    av_frame_free(&rgb);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return status;
}
