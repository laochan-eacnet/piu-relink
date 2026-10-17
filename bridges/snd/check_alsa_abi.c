/* Compile with i686-linux-gnu-gcc; needs libasound2-dev headers, not libasound. */
#include "snd_abi.h"
#include <stdio.h>
#include <errno.h>
#ifdef CHECK_KERNEL_LAYOUT
#include <sound/asound.h>
_Static_assert(sizeof(lx_snd_interval) == sizeof(struct snd_interval), "interval ABI");
_Static_assert(sizeof(lx_snd_hw_params) == sizeof(struct snd_pcm_hw_params), "hw ABI");
_Static_assert(offsetof(lx_snd_hw_params, intervals) == offsetof(struct snd_pcm_hw_params, intervals), "hw intervals ABI");
_Static_assert(offsetof(lx_snd_hw_params, fifo_size) == offsetof(struct snd_pcm_hw_params, fifo_size), "hw fifo ABI");
_Static_assert(sizeof(lx_snd_sw_params) == sizeof(struct snd_pcm_sw_params), "sw ABI");
_Static_assert(offsetof(lx_snd_sw_params, avail_min) == offsetof(struct snd_pcm_sw_params, avail_min), "sw avail ABI");
_Static_assert(offsetof(lx_snd_sw_params, start_threshold) == offsetof(struct snd_pcm_sw_params, start_threshold), "sw start ABI");
#else
#include <alsa/asoundlib.h>
_Static_assert(LX_SND_PLAYBACK == SND_PCM_STREAM_PLAYBACK, "playback enum");
_Static_assert(LX_SND_CAPTURE == SND_PCM_STREAM_CAPTURE, "capture enum");
_Static_assert(LX_SND_RW_INTERLEAVED == SND_PCM_ACCESS_RW_INTERLEAVED, "access enum");
_Static_assert(LX_SND_S16_LE == SND_PCM_FORMAT_S16_LE, "format enum");
_Static_assert(LX_SND_NONBLOCK == SND_PCM_NONBLOCK, "nonblock flag");
_Static_assert(sizeof(lx_uframes) == sizeof(snd_pcm_uframes_t), "unsigned frame ABI");
_Static_assert(sizeof(lx_sframes) == sizeof(snd_pcm_sframes_t), "signed frame ABI");
#endif
_Static_assert(LX_EPIPE == EPIPE && LX_EAGAIN == EAGAIN && LX_EINVAL == EINVAL, "Linux errno values");
_Static_assert(LX_EBADFD == EBADFD && LX_ESTRPIPE == ESTRPIPE, "Linux PCM errno values");
_Static_assert(sizeof(void *) == 4, "compile this probe for i386");
int main(void)
{
    printf("ALSA i386 ABI: access=%d format=%d playback=%d nonblock=%d; frames=%u/%u hw=%u sw=%u; EPIPE=%d EAGAIN=%d\n",
        LX_SND_RW_INTERLEAVED, LX_SND_S16_LE, LX_SND_PLAYBACK, LX_SND_NONBLOCK,
        (unsigned)sizeof(lx_uframes), (unsigned)sizeof(lx_sframes),
        (unsigned)sizeof(lx_snd_hw_params), (unsigned)sizeof(lx_snd_sw_params), LX_EPIPE, LX_EAGAIN);
    return 0;
}
