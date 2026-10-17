/* Compare upstream 4.2 and 4.2.11 public i386 layouts; assertions match piu's
 * observed video-loader and decode-loop field offsets, not a Windows ABI. */
#include <stddef.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>

#define FIELD(type, member) printf(#type "." #member "=%u\n", (unsigned)offsetof(type, member))
#define SIZE(type) printf("sizeof(" #type ")=%u\n", (unsigned)sizeof(type))
_Static_assert(sizeof(void *) == 4, "i386 required");
_Static_assert(sizeof(AVPacket) == 68, "game allocates a 68-byte AVPacket");
_Static_assert(offsetof(AVPacket, data) == 20, "game packet.data");
_Static_assert(offsetof(AVPacket, size) == 24, "game packet.size");
_Static_assert(offsetof(AVPacket, stream_index) == 28, "game packet.stream_index");
_Static_assert(offsetof(AVFormatContext, streams) == 28, "game format.streams");
_Static_assert(offsetof(AVFormatContext, flags) == 0x444, "game format.flags");
_Static_assert(offsetof(AVStream, r_frame_rate) == 0x9c, "game stream.r_frame_rate");
_Static_assert(offsetof(AVStream, codecpar) == 0xa8, "game stream.codecpar");
_Static_assert(offsetof(AVCodecContext, width) == 0x58, "game decoder.width");
_Static_assert(offsetof(AVCodecContext, height) == 0x5c, "game decoder.height");
_Static_assert(offsetof(AVCodecContext, pix_fmt) == 0x6c, "game decoder.pix_fmt");
_Static_assert(offsetof(AVFrame, linesize) == 0x20, "game frame.linesize");
_Static_assert(offsetof(AVFrame, height) == 0x48, "game frame.height");

int main(void)
{
    printf("headers: avcodec=%s avformat=%s avutil=%s swscale=%s\n",
           AV_STRINGIFY(LIBAVCODEC_VERSION), AV_STRINGIFY(LIBAVFORMAT_VERSION),
           AV_STRINGIFY(LIBAVUTIL_VERSION), AV_STRINGIFY(LIBSWSCALE_VERSION));
    SIZE(AVPacket);
    FIELD(AVPacket, buf); FIELD(AVPacket, pts); FIELD(AVPacket, dts);
    FIELD(AVPacket, data); FIELD(AVPacket, size); FIELD(AVPacket, stream_index);
    FIELD(AVPacket, flags); FIELD(AVPacket, side_data); FIELD(AVPacket, side_data_elems);
    FIELD(AVPacket, duration); FIELD(AVPacket, pos); FIELD(AVPacket, convergence_duration);
    SIZE(AVFormatContext); FIELD(AVFormatContext, pb); FIELD(AVFormatContext, nb_streams);
    FIELD(AVFormatContext, streams); FIELD(AVFormatContext, flags);
    SIZE(AVStream); FIELD(AVStream, time_base); FIELD(AVStream, avg_frame_rate);
    FIELD(AVStream, r_frame_rate); FIELD(AVStream, codecpar);
    SIZE(AVCodecParameters); FIELD(AVCodecParameters, codec_type); FIELD(AVCodecParameters, codec_id);
    SIZE(AVCodecContext); FIELD(AVCodecContext, width); FIELD(AVCodecContext, height); FIELD(AVCodecContext, pix_fmt);
    SIZE(AVFrame); FIELD(AVFrame, data); FIELD(AVFrame, linesize); FIELD(AVFrame, width);
    FIELD(AVFrame, height); FIELD(AVFrame, format);
    SIZE(AVPicture); FIELD(AVPicture, data); FIELD(AVPicture, linesize);
    SIZE(AVProbeData); FIELD(AVProbeData, filename); FIELD(AVProbeData, buf); FIELD(AVProbeData, buf_size);
    SIZE(AVIOContext); FIELD(AVIOContext, buffer); FIELD(AVIOContext, opaque);
    FIELD(AVIOContext, read_packet); FIELD(AVIOContext, seek);
    return 0;
}
