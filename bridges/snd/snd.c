#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "snd_abi.h"
#include "../../lxrt/lxabi.h"
#include "../common/lxini.h"

/* phoenixtools.ini beside the EXE: [audio] mode=shared (default) or exclusive.
 * Exclusive uses event-driven native-rate stereo: S16, float32, then PCM
 * 24-in-32/16-bit containers. Unsupported/busy endpoints fall back to shared.
 * Guest queues and frame counts remain S16 at the requested rate in both modes.
 * Windows reclaims the endpoint on process termination; close stops/releases it
 * on the COM-owning worker, without unsafe loader-lock shutdown callbacks. */

/* Opt-in [debug] diagnostics, sampled when hw_params commits:
 * audio_capture=<Windows path.wav>: tee every writei submission before format
 * conversion (including a rejected/short-write tail), checkpoint every second
 * and on close. Relative paths resolve against the EXE directory. One PCM owns
 * a capture path; use a separate path per process.
 * audio_stats=true: stderr every five seconds, frames/underruns for that interval,
 * peak and RMS across both S16 channels normalized by 32768. Prepare does not
 * clear these interval counters. Both diagnostics are disabled by default. */

static const CLSID clsid_enumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const IID iid_enumerator = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const IID iid_client = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const IID iid_render = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const IID iid_clock = {0xcd63314f,0x3fba,0x4a1b,{0x81,0x2c,0xef,0x96,0x35,0x87,0x28,0xe7}};

enum { PCM_MAGIC = 0x534e4450, CMD_CONFIG = 1, CMD_PREPARE, CMD_CLOSE };
enum { HW_CHANNELS = 2, HW_RATE = 3, HW_PERIODS = 7, HW_BUFFER = 9 };

enum { SAMPLE_S16, SAMPLE_FLOAT, SAMPLE_S24_32 };
typedef struct audio_packet {
    uint64_t start;
    UINT frames;
} audio_packet;
typedef struct pcm {
    unsigned magic;
    CRITICAL_SECTION lock, command_lock;
    CONDITION_VARIABLE changed;
    HANDLE thread, command_event, command_done, audio_event, data_event;
    int command, command_result, error, nonblock, configured, started, draining;
    lx_snd_hw_params hw;
    lx_snd_sw_params sw;
    BYTE *ring;
    UINT read_pos, write_pos, queued, padding, capacity, engine_capacity, rate;
    int want_exclusive, exclusive, sample_format, render_ready;
    UINT sample_bytes, packet_count;
    REFERENCE_TIME period;
    IAudioClock *clock;
    uint64_t clock_frequency, clock_frames, submitted_frames;
    audio_packet packets[3]; /* Exclusive event streams use two device buffers. */
    LARGE_INTEGER stamp, frequency;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    HANDLE capture;
    uint64_t capture_bytes;
    ULONGLONG capture_next, stats_next;
    int stats_enabled;
    uint64_t stats_frames, stats_squares;
    unsigned stats_peak, stats_underruns;
} pcm;

/* Reserve a ds64-sized JUNK chunk so long captures can become RF64 without
 * moving sample data. Ordinary captures remain standard RIFF/WAVE files. */
#pragma pack(push, 1)
typedef struct capture_header {
    char riff[4];
    uint32_t riff_size;
    char wave[4], reserved[4];
    uint32_t reserved_size;
    uint64_t riff_size64, data_size64, sample_count64;
    uint32_t table_length;
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format, channels;
    uint32_t rate, byte_rate;
    uint16_t block_align, bits;
    char data[4];
    uint32_t data_size;
} capture_header;
#pragma pack(pop)
_Static_assert(sizeof(capture_header) == 80, "WAVE/RF64 header layout");

static void capture_failure(pcm *p, const char *operation)
{
    fprintf(stderr, "lxsnd: capture %s failed (Windows error %lu)\n", operation, GetLastError());
    CloseHandle(p->capture);
    p->capture = NULL;
}

/* Called with the PCM lock held, just like sample appends. Publish a header
 * checkpoint once a second even after drain. WriteFile has no userspace
 * buffering, so process termination preserves it without a disk flush that
 * could stall the real-time render thread. This is not power-loss durability. */
static void capture_checkpoint(pcm *p)
{
    capture_header h = {0};
    LARGE_INTEGER position;
    DWORD written;
    int rf64;
    if (!p->capture) return;
    rf64 = p->capture_bytes > UINT32_MAX - (sizeof(h) - 8);
    memcpy(h.riff, rf64 ? "RF64" : "RIFF", 4);
    h.riff_size = rf64 ? UINT32_MAX : (uint32_t)(p->capture_bytes + sizeof(h) - 8);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.reserved, rf64 ? "ds64" : "JUNK", 4);
    h.reserved_size = 28;
    if (rf64) {
        h.riff_size64 = p->capture_bytes + sizeof(h) - 8;
        h.data_size64 = p->capture_bytes;
        h.sample_count64 = p->capture_bytes / 4;
    }
    memcpy(h.fmt, "fmt ", 4);
    h.fmt_size = 16;
    h.format = WAVE_FORMAT_PCM;
    h.channels = 2;
    h.rate = p->rate;
    h.byte_rate = p->rate * 4;
    h.block_align = 4;
    h.bits = 16;
    memcpy(h.data, "data", 4);
    h.data_size = rf64 ? UINT32_MAX : (uint32_t)p->capture_bytes;
    position.QuadPart = 0;
    if (!SetFilePointerEx(p->capture, position, NULL, FILE_BEGIN) ||
        !WriteFile(p->capture, &h, sizeof(h), &written, NULL) || written != sizeof(h)) {
        capture_failure(p, "header");
        return;
    }
    position.QuadPart = sizeof(h) + p->capture_bytes;
    if (!SetFilePointerEx(p->capture, position, NULL, FILE_BEGIN))
        capture_failure(p, "seek");
}

static void diagnostics_close(pcm *p)
{
    capture_checkpoint(p);
    if (p->capture) CloseHandle(p->capture);
    p->capture = NULL;
    p->stats_enabled = 0;
}

static void diagnostics_open(pcm *p)
{
    wchar_t path[LX_INI_PATH_CAPACITY];
    ULONGLONG now = GetTickCount64();
    p->capture_bytes = 0;
    p->capture_next = now + 1000;
    p->stats_next = now + 5000;
    p->stats_frames = p->stats_squares = 0;
    p->stats_peak = p->stats_underruns = 0;
    p->stats_enabled = lx_ini_get_bool("debug", "audio_stats", 0);
    if (lx_ini_get_path("debug", "audio_capture", path, sizeof(path) / sizeof(path[0]))) {
        p->capture = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (p->capture == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "lxsnd: capture open failed (Windows error %lu)\n", GetLastError());
            p->capture = NULL;
        } else {
            capture_checkpoint(p);
        }
    }
}

static void diagnostics_samples(pcm *p, const BYTE *source, UINT frames)
{
    if (p->capture) {
        DWORD written = 0, bytes = frames * 4;
        if (!WriteFile(p->capture, source, bytes, &written, NULL) || written != bytes)
            capture_failure(p, "samples");
        else
            p->capture_bytes += bytes;
    }
    if (p->stats_enabled) {
        const int16_t *samples = (const int16_t *)source;
        UINT i;
        p->stats_frames += frames;
        for (i = 0; i < frames * 2; ++i) {
            int sample = samples[i];
            unsigned magnitude = sample < 0 ? (unsigned)-sample : (unsigned)sample;
            if (magnitude > p->stats_peak) p->stats_peak = magnitude;
            p->stats_squares += (uint64_t)(sample * sample);
        }
    }
}

static void diagnostics_tick(pcm *p)
{
    ULONGLONG now;
    if (!p->capture && !p->stats_enabled) return;
    now = GetTickCount64();
    if (p->capture && now >= p->capture_next) {
        capture_checkpoint(p);
        p->capture_next = now + 1000;
    }
    if (p->stats_enabled && now >= p->stats_next) {
        double rms = p->stats_frames ?
            sqrt((double)p->stats_squares / ((double)p->stats_frames * 2.0)) / 32768.0 : 0.0;
        fprintf(stderr, "lxsnd: frames=%llu underruns=%u peak=%.6f rms=%.6f\n",
            (unsigned long long)p->stats_frames, p->stats_underruns, p->stats_peak / 32768.0, rms);
        p->stats_frames = p->stats_squares = 0;
        p->stats_peak = p->stats_underruns = 0;
        p->stats_next = now + 5000;
    }
}

static DWORD diagnostics_timeout(pcm *p)
{
    ULONGLONG now, next = UINT64_MAX;
    if (p->capture) next = p->capture_next;
    if (p->stats_enabled && p->stats_next < next) next = p->stats_next;
    if (next == UINT64_MAX) return INFINITE;
    now = GetTickCount64();
    return next <= now ? 0 : (DWORD)(next - now);
}

static int hresult_errno(HRESULT hr)
{
    if (SUCCEEDED(hr)) return 0;
    if (hr == E_OUTOFMEMORY) return -LX_ENOMEM;
    if (hr == E_ACCESSDENIED) return -LX_EACCES;
    if (hr == E_INVALIDARG || hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return -LX_EINVAL;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return -LX_ENODEV;
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return -LX_ENXIO;
    return -LX_EIO;
}

static int valid(pcm *p) { return p && p->magic == PCM_MAGIC; }
static UINT value(const lx_snd_hw_params *h, unsigned i) { return h->intervals[i].min; }
static void interval(lx_snd_hw_params *h, unsigned i, UINT n)
{
    h->intervals[i].min = h->intervals[i].max = n;
    h->intervals[i].flags = 4;
}

/* The worker owns all COM interfaces. piu configures on its main thread, writes
 * on its mixer pthread, and queries delay concurrently from its main thread. */
static void release_client(pcm *p)
{
    if (p->client) IAudioClient_Stop(p->client);
    if (p->clock) IAudioClock_Release(p->clock);
    if (p->render) IAudioRenderClient_Release(p->render);
    if (p->client) IAudioClient_Release(p->client);
    p->clock = NULL;
    p->render = NULL;
    p->client = NULL;
    p->exclusive = 0;
    p->packet_count = 0;
    p->clock_frames = p->submitted_frames = 0;
}

static void release_stream(pcm *p)
{
    release_client(p);
    diagnostics_close(p);
    free(p->ring);
    p->ring = NULL;
    p->configured = p->started = 0;
    p->queued = p->padding = p->read_pos = p->write_pos = 0;
}

static void exclusive_format(WAVEFORMATEXTENSIBLE *format, UINT rate, unsigned candidate)
{
    static const GUID pcm_subtype = {1,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    memset(format, 0, sizeof(*format));
    format->Format.wFormatTag = candidate == 3 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    format->Format.nChannels = 2;
    format->Format.nSamplesPerSec = rate;
    format->Format.wBitsPerSample = candidate == 1 || candidate == 2 ? 32 : 16;
    format->Format.nBlockAlign = format->Format.wBitsPerSample / 4;
    format->Format.nAvgBytesPerSec = rate * format->Format.nBlockAlign;
    format->Format.cbSize = candidate == 3 ? 0 : sizeof(*format) - sizeof(WAVEFORMATEX);
    format->Samples.wValidBitsPerSample = candidate == 2 ? 24 : format->Format.wBitsPerSample;
    format->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format->SubFormat = pcm_subtype;
    if (candidate == 1) format->SubFormat.Data1 = WAVE_FORMAT_IEEE_FLOAT;
}

static HRESULT stream_services(pcm *p)
{
    HRESULT hr = IAudioClient_SetEventHandle(p->client, p->audio_event);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(p->client, &p->engine_capacity);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(p->client, &iid_render, (void **)&p->render);
    return hr;
}

static HRESULT configure_exclusive(pcm *p, const char **operation)
{
    WAVEFORMATEXTENSIBLE format;
    REFERENCE_TIME default_period, minimum_period;
    unsigned candidate;
    HRESULT hr;
    *operation = "activate";
    hr = IMMDevice_Activate(p->device, &iid_client, CLSCTX_ALL, NULL, (void **)&p->client);
    if (FAILED(hr)) return hr;
    *operation = "format/rate negotiation";
    for (candidate = 0; candidate < 4; ++candidate) {
        exclusive_format(&format, p->rate, candidate);
        hr = IAudioClient_IsFormatSupported(p->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
            &format.Format, NULL);
        if (hr == S_OK) break;
        if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT && hr != S_FALSE) return hr;
    }
    if (candidate == 4) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    p->sample_format = candidate == 1 ? SAMPLE_FLOAT : candidate == 2 ? SAMPLE_S24_32 : SAMPLE_S16;
    p->sample_bytes = format.Format.nBlockAlign;
    *operation = "GetDevicePeriod";
    hr = IAudioClient_GetDevicePeriod(p->client, &default_period, &minimum_period);
    if (FAILED(hr)) return hr;
    p->period = minimum_period > 0 ? minimum_period : default_period;
    *operation = "Initialize";
    hr = IAudioClient_Initialize(p->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p->period, p->period, &format.Format, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT aligned_frames;
        /* The failed client cannot be initialized again. Query its aligned
         * frame count, release it, and activate a fresh client (MSDN pattern). */
        *operation = "aligned GetBufferSize";
        hr = IAudioClient_GetBufferSize(p->client, &aligned_frames);
        if (FAILED(hr)) return hr;
        p->period = (REFERENCE_TIME)((10000000ULL * aligned_frames + p->rate / 2) / p->rate);
        IAudioClient_Release(p->client);
        p->client = NULL;
        *operation = "aligned activate";
        hr = IMMDevice_Activate(p->device, &iid_client, CLSCTX_ALL, NULL, (void **)&p->client);
        if (FAILED(hr)) return hr;
        *operation = "aligned Initialize";
        hr = IAudioClient_Initialize(p->client, AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p->period, p->period, &format.Format, NULL);
    }
    if (FAILED(hr)) return hr;
    *operation = "render services";
    hr = stream_services(p);
    if (FAILED(hr)) return hr;
    *operation = "audio clock";
    hr = IAudioClient_GetService(p->client, &iid_clock, (void **)&p->clock);
    if (SUCCEEDED(hr)) hr = IAudioClock_GetFrequency(p->clock, &p->clock_frequency);
    if (FAILED(hr)) return hr;
    if (!p->clock_frequency) return E_FAIL;
    p->exclusive = 1;
    return S_OK;
}

static const char *exclusive_failure(HRESULT hr)
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE: return "device busy";
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "exclusive mode disabled";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return "unsupported format/rate";
    case AUDCLNT_E_DEVICE_INVALIDATED: return "device invalidated";
    default: return "exclusive stream unavailable";
    }
}

static int configure_stream(pcm *p)
{
    HRESULT hr;
    WAVEFORMATEX source = {0};
    WAVEFORMATEX *mix = NULL;
    const char *operation;
    UINT capacity = value(&p->hw, HW_BUFFER), rate = value(&p->hw, HW_RATE);
    release_stream(p);
    p->error = 0;
    p->capacity = capacity;
    p->rate = rate;
    if (p->want_exclusive) {
        hr = configure_exclusive(p, &operation);
        if (FAILED(hr)) {
            fprintf(stderr, "lxsnd: WASAPI exclusive fallback to shared: %s at %s (HRESULT 0x%08lx)\n",
                exclusive_failure(hr), operation, (unsigned long)hr);
            release_client(p);
        }
    }
    if (!p->exclusive) {
        hr = IMMDevice_Activate(p->device, &iid_client, CLSCTX_ALL, NULL, (void **)&p->client);
        if (FAILED(hr)) goto fail;
        hr = IAudioClient_GetMixFormat(p->client, &mix);
        if (FAILED(hr)) goto fail;
        hr = IAudioClient_GetDevicePeriod(p->client, &p->period, NULL);
        if (FAILED(hr)) goto fail;
        source.wFormatTag = WAVE_FORMAT_PCM;
        source.nChannels = 2;
        source.nSamplesPerSec = rate;
        source.wBitsPerSample = 16;
        source.nBlockAlign = 4;
        source.nAvgBytesPerSec = rate * 4;
        p->sample_format = SAMPLE_S16;
        p->sample_bytes = 4;
        /* Only shared mode delegates conversion and resampling to Windows. */
        hr = IAudioClient_Initialize(p->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            ((REFERENCE_TIME)capacity * 10000000 + rate - 1) / rate, 0, &source, NULL);
        if (FAILED(hr)) goto fail;
        hr = stream_services(p);
        if (FAILED(hr)) goto fail;
    }
    p->ring = (BYTE *)malloc((size_t)capacity * 4);
    if (!p->ring) { hr = E_OUTOFMEMORY; goto fail; }
    memset(&p->sw, 0, sizeof(p->sw));
    p->sw.period_step = p->sw.xfer_align = p->sw.start_threshold = 1;
    p->sw.avail_min = capacity / value(&p->hw, HW_PERIODS);
    p->sw.stop_threshold = capacity;
    p->sw.boundary = capacity;
    while (p->sw.boundary <= (0x7fffffffU - capacity) / 2) p->sw.boundary *= 2;
    p->configured = 1;
    p->draining = 0;
    diagnostics_open(p);
    QueryPerformanceCounter(&p->stamp);
    if (p->exclusive) {
        fprintf(stderr, "lxsnd: WASAPI exclusive S16 stereo %u Hz -> %s stereo %u Hz; buffer %u/%u frames; period %.3f ms\n",
            rate, p->sample_format == SAMPLE_FLOAT ? "float32" :
                p->sample_format == SAMPLE_S24_32 ? "PCM24-in-32" : "S16",
            rate, capacity, p->engine_capacity, p->period / 10000.0);
    } else {
        fprintf(stderr, "lxsnd: WASAPI shared S16 stereo %u Hz -> mix %lu Hz/%u bits/tag %u/subtype %lu; buffer %u/%u frames; period %.3f ms\n",
            rate, mix->nSamplesPerSec, mix->wBitsPerSample, mix->wFormatTag,
            mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22 ?
                ((const WAVEFORMATEXTENSIBLE *)mix)->SubFormat.Data1 : (DWORD)mix->wFormatTag,
            capacity, p->engine_capacity, p->period / 10000.0);
    }
    CoTaskMemFree(mix);
    return 0;
fail:
    fprintf(stderr, "lxsnd: WASAPI configure HRESULT 0x%08lx\n", (unsigned long)hr);
    CoTaskMemFree(mix);
    release_stream(p);
    return hresult_errno(hr);
}

static int prepare_stream(pcm *p)
{
    HRESULT hr;
    if (!p->configured) return -LX_EBADFD;
    hr = IAudioClient_Stop(p->client);
    if (SUCCEEDED(hr)) hr = IAudioClient_Reset(p->client);
    if (FAILED(hr)) return hresult_errno(hr);
    p->queued = p->padding = p->read_pos = p->write_pos = 0;
    p->error = p->started = p->draining = 0;
    p->packet_count = 0;
    p->clock_frames = p->submitted_frames = 0;
    p->render_ready = 0;
    QueryPerformanceCounter(&p->stamp);
    return 0;
}

/* Exclusive event streams require full device-period packets, not shared-mode
 * GetCurrentPadding/refills. The audio clock tracks the two physical buffers;
 * only real guest samples (not a final packet's silence) contribute to delay. */
static UINT exclusive_pending(pcm *p, uint64_t played)
{
    UINT i, pending = 0;
    for (i = 0; i < p->packet_count; ++i) {
        const audio_packet *packet = &p->packets[i];
        uint64_t end = packet->start + packet->frames;
        if (played < end)
            pending += (UINT)(end - (played > packet->start ? played : packet->start));
    }
    return pending;
}

static void copy_samples(pcm *p, BYTE *dest, const BYTE *source, UINT frames)
{
    UINT i;
    const int16_t *samples = (const int16_t *)source;
    if (p->sample_format == SAMPLE_FLOAT) {
        float *out = (float *)dest;
        for (i = 0; i < frames * 2; ++i) out[i] = samples[i] / 32768.0f;
    } else if (p->sample_format == SAMPLE_S24_32) {
        int32_t *out = (int32_t *)dest;
        /* PCM valid bits are left-aligned; multiplication avoids negative-shift UB. */
        for (i = 0; i < frames * 2; ++i) out[i] = (int32_t)samples[i] * 65536;
    } else {
        memcpy(dest, source, frames * 4);
    }
}

static void service_exclusive(pcm *p, int audio_event)
{
    UINT frames, first, i;
    uint64_t position;
    BYTE *dest;
    HRESULT hr = IAudioClock_GetPosition(p->clock, &position, NULL);
    if (FAILED(hr)) goto fail;
    p->clock_frames = position / p->clock_frequency * p->rate +
        position % p->clock_frequency * p->rate / p->clock_frequency;
    QueryPerformanceCounter(&p->stamp);
    p->padding = exclusive_pending(p, p->clock_frames);
    for (i = 0; i < p->packet_count &&
            p->packets[i].start + p->engine_capacity <= p->clock_frames; ++i) {}
    if (i) {
        p->packet_count -= i;
        memmove(p->packets, p->packets + i, p->packet_count * sizeof(p->packets[0]));
    }
    if (p->started && !p->padding && !p->queued) {
        if (!p->draining) {
            p->error = -LX_EPIPE;
            if (p->stats_enabled) ++p->stats_underruns;
            IAudioClient_Stop(p->client);
            goto changed;
        }
        hr = IAudioClient_Stop(p->client);
        if (SUCCEEDED(hr)) hr = IAudioClient_Reset(p->client);
        if (FAILED(hr)) goto fail;
        p->started = p->draining = p->render_ready = 0;
        p->packet_count = 0;
        p->clock_frames = p->submitted_frames = 0;
        goto changed;
    }
    if (audio_event) p->render_ready = 1;
    if (!p->started) {
        if (!p->queued || (!p->draining && p->queued < p->sw.start_threshold))
            goto changed;
        p->render_ready = 1;
    }
    if (!p->render_ready || !p->queued) goto changed;
    frames = p->queued < p->engine_capacity ? p->queued : p->engine_capacity;
    /* While another packet is playing, allow the guest's 256-frame producer
     * to finish a period instead of inserting silence between small writes. */
    if (p->started && frames < p->engine_capacity && p->padding && !p->draining)
        goto changed;
    if (p->packet_count == sizeof(p->packets) / sizeof(p->packets[0])) {
        hr = AUDCLNT_E_BUFFER_ERROR;
        goto fail;
    }
    hr = IAudioRenderClient_GetBuffer(p->render, p->engine_capacity, &dest);
    if (FAILED(hr)) goto fail;
    first = p->capacity - p->read_pos;
    if (first > frames) first = frames;
    copy_samples(p, dest, p->ring + p->read_pos * 4, first);
    if (first < frames)
        copy_samples(p, dest + first * p->sample_bytes, p->ring, frames - first);
    if (frames < p->engine_capacity)
        memset(dest + frames * p->sample_bytes, 0, (p->engine_capacity - frames) * p->sample_bytes);
    hr = IAudioRenderClient_ReleaseBuffer(p->render, p->engine_capacity, 0);
    if (FAILED(hr)) goto fail;
    p->packets[p->packet_count].start = p->submitted_frames;
    p->packets[p->packet_count++].frames = frames;
    p->submitted_frames += p->engine_capacity;
    p->read_pos = (p->read_pos + frames) % p->capacity;
    p->queued -= frames;
    p->padding += frames;
    p->render_ready = 0;
    if (!p->started) {
        hr = IAudioClient_Start(p->client);
        if (FAILED(hr)) goto fail;
        p->started = 1;
        QueryPerformanceCounter(&p->stamp);
    }
changed:
    WakeAllConditionVariable(&p->changed);
    return;
fail:
    fprintf(stderr, "lxsnd: WASAPI exclusive render HRESULT 0x%08lx\n", (unsigned long)hr);
    p->error = hresult_errno(hr);
    WakeAllConditionVariable(&p->changed);
}

static void service_stream(pcm *p, int audio_event)
{
    UINT padding, frames, first;
    BYTE *dest;
    HRESULT hr;
    if (!p->configured || p->error) return;
    if (p->exclusive) {
        service_exclusive(p, audio_event);
        return;
    }
    hr = IAudioClient_GetCurrentPadding(p->client, &padding);
    if (FAILED(hr)) goto fail;
    p->padding = padding;
    QueryPerformanceCounter(&p->stamp);
    if (p->started && padding == 0) {
        if (p->draining && p->queued == 0) {
            hr = IAudioClient_Stop(p->client);
            if (FAILED(hr)) goto fail;
            p->started = p->draining = 0;
            WakeAllConditionVariable(&p->changed);
            return;
        }
        /* WASAPI inserts silence after starvation; ALSA exposes it as XRUN. */
        p->error = -LX_EPIPE;
        if (p->stats_enabled) ++p->stats_underruns;
        IAudioClient_Stop(p->client);
        WakeAllConditionVariable(&p->changed);
        return;
    }
    frames = p->engine_capacity - padding;
    if (frames > p->queued) frames = p->queued;
    if (frames) {
        hr = IAudioRenderClient_GetBuffer(p->render, frames, &dest);
        if (FAILED(hr)) goto fail;
        first = p->capacity - p->read_pos;
        if (first > frames) first = frames;
        memcpy(dest, p->ring + p->read_pos * 4, first * 4);
        if (first < frames) memcpy(dest + first * 4, p->ring, (frames - first) * 4);
        hr = IAudioRenderClient_ReleaseBuffer(p->render, frames, 0);
        if (FAILED(hr)) goto fail;
        p->read_pos = (p->read_pos + frames) % p->capacity;
        p->queued -= frames;
        p->padding += frames;
        QueryPerformanceCounter(&p->stamp);
    }
    if (!p->started && p->padding >= p->sw.start_threshold) {
        hr = IAudioClient_Start(p->client);
        if (FAILED(hr)) goto fail;
        p->started = 1;
        QueryPerformanceCounter(&p->stamp);
    }
    WakeAllConditionVariable(&p->changed);
    return;
fail:
    p->error = hresult_errno(hr);
    WakeAllConditionVariable(&p->changed);
}

static DWORD WINAPI audio_thread(void *arg)
{
    pcm *p = (pcm *)arg;
    IMMDeviceEnumerator *enumerator = NULL;
    HANDLE events[3] = {p->command_event, p->audio_event, p->data_event};
    DWORD task_index = 0;
    HANDLE priority = NULL;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com_initialized = SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL,
        &iid_enumerator, (void **)&enumerator);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &p->device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    p->command_result = hresult_errno(hr);
    SetEvent(p->command_done);
    if (FAILED(hr)) goto done;
    priority = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    for (;;) {
        DWORD timeout, event;
        EnterCriticalSection(&p->lock);
        timeout = diagnostics_timeout(p);
        LeaveCriticalSection(&p->lock);
        event = WaitForMultipleObjects(3, events, FALSE, timeout);
        EnterCriticalSection(&p->lock);
        if (event == WAIT_OBJECT_0) {
            int command = p->command;
            if (command == CMD_CLOSE) {
                release_stream(p);
                LeaveCriticalSection(&p->lock);
                break;
            }
            p->command_result = command == CMD_CONFIG ? configure_stream(p) : prepare_stream(p);
            SetEvent(p->command_done);
            WakeAllConditionVariable(&p->changed);
        } else if (event == WAIT_FAILED) {
            p->error = -LX_EIO;
            WakeAllConditionVariable(&p->changed);
            LeaveCriticalSection(&p->lock);
            break;
        }
        service_stream(p, event == WAIT_OBJECT_0 + 1);
        diagnostics_tick(p);
        LeaveCriticalSection(&p->lock);
    }
done:
    release_stream(p);
    if (priority) AvRevertMmThreadCharacteristics(priority);
    if (p->device) IMMDevice_Release(p->device);
    p->device = NULL;
    if (com_initialized) CoUninitialize();
    return 0;
}

static int command(pcm *p, int which, const lx_snd_hw_params *hw)
{
    int result;
    EnterCriticalSection(&p->command_lock);
    EnterCriticalSection(&p->lock);
    if (hw) p->hw = *hw;
    p->command = which;
    ResetEvent(p->command_done);
    SetEvent(p->command_event);
    LeaveCriticalSection(&p->lock);
    if (WaitForSingleObject(p->command_done, INFINITE) != WAIT_OBJECT_0) result = -LX_EIO;
    else result = p->command_result;
    LeaveCriticalSection(&p->command_lock);
    return result;
}

static void destroy_handle(pcm *p)
{
    if (p->thread) CloseHandle(p->thread);
    if (p->command_event) CloseHandle(p->command_event);
    if (p->command_done) CloseHandle(p->command_done);
    if (p->audio_event) CloseHandle(p->audio_event);
    if (p->data_event) CloseHandle(p->data_event);
    DeleteCriticalSection(&p->command_lock);
    DeleteCriticalSection(&p->lock);
    p->magic = 0;
    free(p);
}

static int snd_pcm_open(pcm **out, const char *name, int stream, int mode)
{
    pcm *p;
    int result;
    if (!out) return -LX_EINVAL;
    *out = NULL;
    if (!name || stream != LX_SND_PLAYBACK || (mode & ~LX_SND_NONBLOCK)) return -LX_EINVAL;
    if (strcmp(name, "hw:0,0") && strcmp(name, "default") && strcmp(name, "plughw:0,0")) return -LX_ENODEV;
    p = (pcm *)calloc(1, sizeof(*p));
    if (!p) return -LX_ENOMEM;
    p->magic = PCM_MAGIC;
    p->nonblock = mode & LX_SND_NONBLOCK;
    {
        char audio_mode[128];
        if (lx_ini_get("audio", "mode", audio_mode, sizeof(audio_mode))) {
            if (!_stricmp(audio_mode, "exclusive")) p->want_exclusive = 1;
            else if (_stricmp(audio_mode, "shared"))
                fprintf(stderr, "lxsnd: invalid [audio] mode '%s'; using shared\n", audio_mode);
        }
    }
    InitializeCriticalSection(&p->lock);
    InitializeCriticalSection(&p->command_lock);
    InitializeConditionVariable(&p->changed);
    QueryPerformanceFrequency(&p->frequency);
    p->command_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    p->command_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    p->audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    p->data_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!p->command_event || !p->command_done || !p->audio_event || !p->data_event) {
        destroy_handle(p);
        return -LX_ENOMEM;
    }
    p->thread = CreateThread(NULL, 0, audio_thread, p, 0, NULL);
    if (!p->thread) { destroy_handle(p); return -LX_EAGAIN; }
    WaitForSingleObject(p->command_done, INFINITE);
    result = p->command_result;
    if (result < 0) {
        WaitForSingleObject(p->thread, INFINITE);
        destroy_handle(p);
        return result;
    }
    *out = p;
    return 0;
}

static int snd_pcm_close(pcm *p)
{
    if (!valid(p)) return -LX_EINVAL;
    EnterCriticalSection(&p->command_lock);
    EnterCriticalSection(&p->lock);
    p->command = CMD_CLOSE;
    SetEvent(p->command_event);
    LeaveCriticalSection(&p->lock);
    WaitForSingleObject(p->thread, INFINITE);
    LeaveCriticalSection(&p->command_lock);
    destroy_handle(p);
    return 0;
}

static size_t snd_pcm_hw_params_sizeof(void) { return sizeof(lx_snd_hw_params); }
static size_t snd_pcm_sw_params_sizeof(void) { return sizeof(lx_snd_sw_params); }
static int snd_pcm_hw_params_malloc(lx_snd_hw_params **out)
{
    if (!out) return -LX_EINVAL;
    *out = (lx_snd_hw_params *)calloc(1, sizeof(**out));
    return *out ? 0 : -LX_ENOMEM;
}
static int snd_pcm_sw_params_malloc(lx_snd_sw_params **out)
{
    if (!out) return -LX_EINVAL;
    *out = (lx_snd_sw_params *)calloc(1, sizeof(**out));
    return *out ? 0 : -LX_ENOMEM;
}
static void snd_pcm_hw_params_free(lx_snd_hw_params *h) { free(h); }
static void snd_pcm_sw_params_free(lx_snd_sw_params *s) { free(s); }

static int snd_pcm_hw_params_any(pcm *p, lx_snd_hw_params *h)
{
    if (!valid(p) || !h) return -LX_EINVAL;
    memset(h, 0, sizeof(*h));
    h->masks[0][0] = 1U << LX_SND_RW_INTERLEAVED;
    h->masks[1][0] = 1U << LX_SND_S16_LE;
    interval(h, HW_CHANNELS, 2);
    interval(h, HW_RATE, 48000);
    interval(h, HW_PERIODS, 4);
    interval(h, HW_BUFFER, 2048);
    h->msbits = 16;
    h->rate_num = 48000;
    h->rate_den = 1;
    return 0;
}
static int snd_pcm_hw_params_set_access(pcm *p, lx_snd_hw_params *h, int access)
{
    if (!valid(p) || !h || access != LX_SND_RW_INTERLEAVED) return -LX_EINVAL;
    h->masks[0][0] = 1U << access;
    return 0;
}
static int snd_pcm_hw_params_set_format(pcm *p, lx_snd_hw_params *h, int format)
{
    if (!valid(p) || !h || format != LX_SND_S16_LE) return -LX_EINVAL;
    h->masks[1][0] = 1U << format;
    return 0;
}
static int snd_pcm_hw_params_set_channels(pcm *p, lx_snd_hw_params *h, unsigned channels)
{
    if (!valid(p) || !h || channels != 2) return -LX_EINVAL;
    interval(h, HW_CHANNELS, channels);
    return 0;
}
static int snd_pcm_hw_params_set_rate_near(pcm *p, lx_snd_hw_params *h, unsigned *rate, int *dir)
{
    if (!valid(p) || !h || !rate || !*rate) return -LX_EINVAL;
    if (*rate < 8000) *rate = 8000;
    if (*rate > 192000) *rate = 192000;
    if (dir) *dir = 0;
    interval(h, HW_RATE, *rate);
    h->rate_num = *rate;
    h->rate_den = 1;
    return 0;
}
static int snd_pcm_hw_params_set_periods(pcm *p, lx_snd_hw_params *h, unsigned periods, int dir)
{
    if (!valid(p) || !h || !periods || periods > 128 || dir) return -LX_EINVAL;
    interval(h, HW_PERIODS, periods);
    return 0;
}
static int snd_pcm_hw_params_set_buffer_size(pcm *p, lx_snd_hw_params *h, lx_uframes frames)
{
    if (!valid(p) || !h || frames < 64 || frames > 262144) return -LX_EINVAL;
    interval(h, HW_BUFFER, frames);
    return 0;
}
static int snd_pcm_hw_params(pcm *p, lx_snd_hw_params *h)
{
    if (!valid(p) || !h || h->masks[0][0] != (1U << LX_SND_RW_INTERLEAVED) ||
        h->masks[1][0] != (1U << LX_SND_S16_LE) || value(h, HW_CHANNELS) != 2 ||
        value(h, HW_RATE) < 8000 || value(h, HW_RATE) > 192000 ||
        !value(h, HW_PERIODS) || value(h, HW_PERIODS) > value(h, HW_BUFFER) ||
        value(h, HW_BUFFER) < 64 || value(h, HW_BUFFER) > 262144) return -LX_EINVAL;
    return command(p, CMD_CONFIG, h);
}
static int snd_pcm_sw_params_current(pcm *p, lx_snd_sw_params *s)
{
    int result = 0;
    if (!valid(p) || !s) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    if (!p->configured) result = -LX_EBADFD;
    else *s = p->sw;
    LeaveCriticalSection(&p->lock);
    return result;
}
static int snd_pcm_sw_params_set_avail_min(pcm *p, lx_snd_sw_params *s, lx_uframes frames)
{
    if (!valid(p) || !s || !frames) return -LX_EINVAL;
    s->avail_min = frames;
    return 0;
}
static int snd_pcm_sw_params(pcm *p, lx_snd_sw_params *s)
{
    int result = 0;
    if (!valid(p) || !s) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    if (!p->configured) result = -LX_EBADFD;
    else if (!s->avail_min || s->avail_min > p->capacity || !s->start_threshold) result = -LX_EINVAL;
    else p->sw = *s;
    LeaveCriticalSection(&p->lock);
    return result;
}
static int snd_pcm_prepare(pcm *p)
{
    if (!valid(p)) return -LX_EINVAL;
    return command(p, CMD_PREPARE, NULL);
}

/* Estimate progress only inside the last WASAPI padding snapshot. Never count
 * queued ring frames as played, and keep all quantities in the source rate. */
static UINT pending_frames(pcm *p)
{
    UINT padding = p->padding;
    if (p->started && padding) {
        LARGE_INTEGER now;
        uint64_t played;
        QueryPerformanceCounter(&now);
        played = (uint64_t)(now.QuadPart - p->stamp.QuadPart) * p->rate / p->frequency.QuadPart;
        padding = p->exclusive ? exclusive_pending(p, p->clock_frames + played) :
            played >= padding ? 0 : padding - (UINT)played;
    }
    return p->queued + padding;
}
static int stream_error(pcm *p)
{
    if (!p->configured) return -LX_EBADFD;
    return p->error;
}
static lx_sframes snd_pcm_avail_update(pcm *p)
{
    lx_sframes result;
    UINT pending;
    if (!valid(p)) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    result = stream_error(p);
    if (!result) {
        pending = pending_frames(p);
        result = pending >= p->capacity ? 0 : (lx_sframes)(p->capacity - pending);
    }
    LeaveCriticalSection(&p->lock);
    return result;
}
static int snd_pcm_delay(pcm *p, lx_sframes *delay)
{
    int result;
    if (!valid(p) || !delay) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    result = stream_error(p);
    *delay = result ? 0 : (lx_sframes)pending_frames(p);
    LeaveCriticalSection(&p->lock);
    return result;
}
static int snd_pcm_wait(pcm *p, int timeout)
{
    ULONGLONG deadline = timeout < 0 ? 0 : GetTickCount64() + (unsigned)timeout;
    int result;
    if (!valid(p)) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    for (;;) {
        UINT pending;
        DWORD remaining = INFINITE;
        result = stream_error(p);
        if (result) break;
        pending = pending_frames(p);
        if (pending <= p->capacity && p->capacity - pending >= p->sw.avail_min) { result = 1; break; }
        if (timeout >= 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) { result = 0; break; }
            remaining = (DWORD)(deadline - now);
        }
        if (!SleepConditionVariableCS(&p->changed, &p->lock, remaining) && GetLastError() != ERROR_TIMEOUT) {
            result = -LX_EIO;
            break;
        }
    }
    LeaveCriticalSection(&p->lock);
    return result;
}
static lx_sframes snd_pcm_writei(pcm *p, const void *buffer, lx_uframes frames)
{
    UINT written = 0;
    int error = 0;
    const BYTE *source = (const BYTE *)buffer;
    if (!valid(p) || (!buffer && frames) || frames > 0x7fffffffU / 4) return -LX_EINVAL;
    /* The guest thread that feeds PCM is the latency-critical producer (piu: 256-frame writes
     * into a 2048-frame buffer). Register it with MMCSS once, like the native render worker,
     * so it is not starved by the render thread under emulation. */
    static __declspec(thread) int writer_registered;
    if (!writer_registered) {
        DWORD task_index = 0;
        writer_registered = 1;
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    }
    EnterCriticalSection(&p->lock);
    /* Tee the caller's complete pre-conversion submission exactly once, even
     * if a nonblocking write later accepts only a prefix or reports XRUN. */
    if (p->capture || p->stats_enabled) diagnostics_samples(p, source, frames);
    while (written < frames) {
        UINT pending, space, count, first;
        error = stream_error(p);
        if (error) break;
        pending = pending_frames(p);
        space = pending >= p->capacity ? 0 : p->capacity - pending;
        /* The ring limit also protects against clock-estimation rounding. */
        if (space > p->capacity - p->queued) space = p->capacity - p->queued;
        if (!space) {
            if (p->nonblock) { error = -LX_EAGAIN; break; }
            if (!SleepConditionVariableCS(&p->changed, &p->lock, INFINITE)) { error = -LX_EIO; break; }
            continue;
        }
        count = frames - written;
        if (count > space) count = space;
        first = p->capacity - p->write_pos;
        if (first > count) first = count;
        memcpy(p->ring + p->write_pos * 4, source + written * 4, first * 4);
        if (first < count) memcpy(p->ring, source + (written + first) * 4, (count - first) * 4);
        p->write_pos = (p->write_pos + count) % p->capacity;
        p->queued += count;
        written += count;
        SetEvent(p->data_event);
    }
    LeaveCriticalSection(&p->lock);
    return written ? (lx_sframes)written : error;
}
static int snd_pcm_drain(pcm *p)
{
    int result;
    if (!valid(p)) return -LX_EINVAL;
    EnterCriticalSection(&p->lock);
    result = stream_error(p);
    if (!result) {
        p->draining = 1;
        SetEvent(p->data_event);
        while ((p->queued || p->padding) && !(result = stream_error(p))) {
            if (p->nonblock) { result = -LX_EAGAIN; break; }
            if (!SleepConditionVariableCS(&p->changed, &p->lock, INFINITE)) { result = -LX_EIO; break; }
        }
        if (!result) p->draining = 0;
    }
    LeaveCriticalSection(&p->lock);
    return result;
}
static int snd_pcm_recover(pcm *p, int error, int silent)
{
    (void)silent;
    if (error == -LX_EINTR) return 0;
    if (error == -LX_EPIPE || error == -LX_ESTRPIPE) return snd_pcm_prepare(p);
    return error;
}
static const char *snd_strerror(int error)
{
    if (error > 0) error = -error;
    switch (error) {
    case 0: return "Success";
    case -LX_EINTR: return "Interrupted system call";
    case -LX_EIO: return "Input/output error";
    case -LX_ENXIO: return "No such device or address";
    case -LX_EAGAIN: return "Resource temporarily unavailable";
    case -LX_ENOMEM: return "Cannot allocate memory";
    case -LX_EACCES: return "Permission denied";
    case -LX_ENODEV: return "No such device";
    case -LX_EINVAL: return "Invalid argument";
    case -LX_EPIPE: return "Broken pipe";
    case -LX_EBADFD: return "File descriptor in bad state";
    case -LX_ESTRPIPE: return "Streams pipe error";
    default: return "Unknown error";
    }
}

#define S(name, ver) {#name, ver, (void *)name}
static const lx_sym symbols[] = {
    S(snd_pcm_writei, "ALSA_0.9"), S(snd_pcm_avail_update, "ALSA_0.9"),
    S(snd_pcm_close, "ALSA_0.9"), S(snd_pcm_hw_params_set_buffer_size, "ALSA_0.9"),
    S(snd_pcm_sw_params, "ALSA_0.9"), S(snd_pcm_hw_params_set_channels, "ALSA_0.9"),
    S(snd_pcm_sw_params_malloc, "ALSA_0.9"), S(snd_pcm_hw_params_free, "ALSA_0.9"),
    S(snd_pcm_hw_params_any, "ALSA_0.9"), S(snd_pcm_wait, "ALSA_0.9"),
    S(snd_pcm_hw_params, "ALSA_0.9"), S(snd_pcm_hw_params_malloc, "ALSA_0.9"),
    S(snd_pcm_delay, "ALSA_0.9"), S(snd_pcm_sw_params_current, "ALSA_0.9"),
    S(snd_pcm_hw_params_set_rate_near, "ALSA_0.9.0rc4"), S(snd_pcm_open, "ALSA_0.9"),
    S(snd_pcm_hw_params_set_format, "ALSA_0.9"), S(snd_pcm_hw_params_set_periods, "ALSA_0.9"),
    S(snd_pcm_sw_params_free, "ALSA_0.9"), S(snd_pcm_sw_params_set_avail_min, "ALSA_0.9"),
    S(snd_pcm_hw_params_set_access, "ALSA_0.9"), S(snd_pcm_prepare, "ALSA_0.9"),
    S(snd_pcm_hw_params_sizeof, "ALSA_0.9"), S(snd_pcm_sw_params_sizeof, "ALSA_0.9"),
    S(snd_pcm_recover, "ALSA_0.9"), S(snd_pcm_drain, "ALSA_0.9"), S(snd_strerror, "ALSA_0.9")
};
__declspec(dllexport) const lx_sym *lx_provider_symbols(unsigned *count)
{
    if (count) *count = sizeof(symbols) / sizeof(symbols[0]);
    return symbols;
}
