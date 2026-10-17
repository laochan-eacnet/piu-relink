#pragma once
#include <stdint.h>
#include <stddef.h>

/* i386 ALSA scalar ABI; handles/parameter objects remain opaque to guest code. */
enum {
    LX_SND_PLAYBACK = 0, LX_SND_CAPTURE = 1,
    LX_SND_RW_INTERLEAVED = 3, LX_SND_S16_LE = 2,
    LX_SND_NONBLOCK = 1,
    LX_EIO = 5, LX_ENXIO = 6, LX_EAGAIN = 11, LX_ENOMEM = 12,
    LX_EACCES = 13, LX_EINVAL = 22, LX_EPIPE = 32, LX_ENOSYS = 38,
    LX_ESTRPIPE = 86, LX_ENODEV = 19, LX_EBADFD = 77, LX_EINTR = 4
};
typedef uint32_t lx_uframes;
typedef int32_t lx_sframes;

#pragma pack(push, 4)
typedef struct lx_snd_interval {
    uint32_t min, max;
    uint32_t flags; /* openmin:0, openmax:1, integer:2, empty:3 */
} lx_snd_interval;

typedef struct lx_snd_hw_params {
    uint32_t flags;
    uint32_t masks[3][8], mres[5][8];
    lx_snd_interval intervals[12], ires[9];
    uint32_t rmask, cmask, info, msbits, rate_num, rate_den, fifo_size;
    uint8_t sync[16], reserved[48];
} lx_snd_hw_params;

typedef struct lx_snd_sw_params {
    int32_t tstamp_mode;
    uint32_t period_step, sleep_min;
    lx_uframes avail_min, xfer_align, start_threshold, stop_threshold;
    lx_uframes silence_threshold, silence_size, boundary;
    uint32_t proto, tstamp_type;
    uint8_t reserved[56];
} lx_snd_sw_params;
#pragma pack(pop)

_Static_assert(sizeof(lx_uframes) == 4 && sizeof(lx_sframes) == 4, "i386 frame counts");
_Static_assert(sizeof(lx_snd_interval) == 12, "ALSA interval layout");
_Static_assert(sizeof(lx_snd_hw_params) == 604, "ALSA i386 hardware params");
_Static_assert(offsetof(lx_snd_hw_params, intervals) == 260, "ALSA intervals offset");
_Static_assert(offsetof(lx_snd_hw_params, fifo_size) == 536, "ALSA fifo offset");
_Static_assert(sizeof(lx_snd_sw_params) == 104, "ALSA i386 software params");
_Static_assert(offsetof(lx_snd_sw_params, avail_min) == 12, "ALSA avail_min offset");
_Static_assert(offsetof(lx_snd_sw_params, start_threshold) == 20, "ALSA start offset");
