#include <windows.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snd_abi.h"
#include "../../lxrt/lxabi.h"

typedef void pcm;
static const lx_sym *table;
static unsigned symbol_count;
#define FN(result, name, args) static result (*name) args
FN(int, snd_pcm_open, (pcm **, const char *, int, int));
FN(int, snd_pcm_close, (pcm *));
FN(size_t, snd_pcm_hw_params_sizeof, (void));
FN(size_t, snd_pcm_sw_params_sizeof, (void));
FN(int, snd_pcm_hw_params_malloc, (lx_snd_hw_params **));
FN(void, snd_pcm_hw_params_free, (lx_snd_hw_params *));
FN(int, snd_pcm_hw_params_any, (pcm *, lx_snd_hw_params *));
FN(int, snd_pcm_hw_params_set_access, (pcm *, lx_snd_hw_params *, int));
FN(int, snd_pcm_hw_params_set_format, (pcm *, lx_snd_hw_params *, int));
FN(int, snd_pcm_hw_params_set_rate_near, (pcm *, lx_snd_hw_params *, unsigned *, int *));
FN(int, snd_pcm_hw_params_set_channels, (pcm *, lx_snd_hw_params *, unsigned));
FN(int, snd_pcm_hw_params_set_periods, (pcm *, lx_snd_hw_params *, unsigned, int));
FN(int, snd_pcm_hw_params_set_buffer_size, (pcm *, lx_snd_hw_params *, lx_uframes));
FN(int, snd_pcm_hw_params, (pcm *, lx_snd_hw_params *));
FN(int, snd_pcm_sw_params_malloc, (lx_snd_sw_params **));
FN(void, snd_pcm_sw_params_free, (lx_snd_sw_params *));
FN(int, snd_pcm_sw_params_current, (pcm *, lx_snd_sw_params *));
FN(int, snd_pcm_sw_params_set_avail_min, (pcm *, lx_snd_sw_params *, lx_uframes));
FN(int, snd_pcm_sw_params, (pcm *, lx_snd_sw_params *));
FN(int, snd_pcm_prepare, (pcm *));
FN(lx_sframes, snd_pcm_avail_update, (pcm *));
FN(int, snd_pcm_wait, (pcm *, int));
FN(lx_sframes, snd_pcm_writei, (pcm *, const void *, lx_uframes));
FN(int, snd_pcm_delay, (pcm *, lx_sframes *));
FN(int, snd_pcm_drain, (pcm *));
FN(int, snd_pcm_recover, (pcm *, int, int));
FN(const char *, snd_strerror, (int));
#undef FN

static void *find(const char *name)
{
    unsigned i;
    for (i = 0; i < symbol_count; ++i) if (!strcmp(table[i].name, name)) return table[i].addr;
    fprintf(stderr, "Missing provider symbol: %s\n", name);
    exit(1);
}
#define LOAD(name) (*(void **)&name = find(#name))
static void check(const char *name, int result)
{
    printf("%s = %d (%s)\n", name, result, snd_strerror(result));
    if (result < 0) exit(1);
}
#define CHECK(call) check(#call, call)
static void expect(const char *name, int result, int wanted)
{
    printf("%s = %d (expected %d)\n", name, result, wanted);
    if (result != wanted) exit(1);
}

typedef struct playback {
    pcm *handle;
    short *samples;
    unsigned rate;
    int blocking;
    unsigned writes, waits, errors;
} playback;

static DWORD WINAPI play(void *arg)
{
    playback *b = (playback *)arg;
    unsigned offset = 0, next_report = b->rate / 4;
    CHECK(snd_pcm_prepare(b->handle));
    printf("mixer thread=%lu main-configured handle; block=%d\n", GetCurrentThreadId(), b->blocking);
    while (offset < b->rate) {
        unsigned frames = b->blocking ? b->rate - offset : 256;
        lx_sframes written, available, delay;
        int result;
        if (!b->blocking) {
            available = snd_pcm_avail_update(b->handle);
            if (available < 0) { ++b->errors; fprintf(stderr, "avail=%ld\n", (long)available); return 1; }
            available = snd_pcm_avail_update(b->handle);
            if (available < 256) {
                result = snd_pcm_wait(b->handle, 1000);
                ++b->waits;
                if (result <= 0) { ++b->errors; fprintf(stderr, "wait=%d\n", result); return 1; }
                continue;
            }
        }
        if (frames > b->rate - offset) frames = b->rate - offset;
        written = snd_pcm_writei(b->handle, b->samples + offset * 2, frames);
        ++b->writes;
        if (written != (lx_sframes)frames) {
            ++b->errors;
            fprintf(stderr, "writei(%u)=%ld\n", frames, (long)written);
            return 1;
        }
        offset += frames;
        if (offset >= next_report) {
            result = snd_pcm_delay(b->handle, &delay);
            printf("writei total=%u frames return=%ld delay=%ld rc=%d\n", offset, (long)written, (long)delay, result);
            if (result < 0) { ++b->errors; return 1; }
            next_report += b->rate / 4;
        }
    }
    CHECK(snd_pcm_drain(b->handle));
    return 0;
}

static void edges(pcm *handle, const short *samples)
{
    pcm *nonblock;
    lx_snd_hw_params h;
    lx_snd_sw_params s;
    lx_sframes delay;
    CHECK(snd_pcm_prepare(handle));
    expect("starvation seed writei", snd_pcm_writei(handle, samples, 256), 256);
    Sleep(150);
    expect("starved avail", snd_pcm_avail_update(handle), -LX_EPIPE);
    expect("starved wait", snd_pcm_wait(handle, 0), -LX_EPIPE);
    expect("starved delay", snd_pcm_delay(handle, &delay), -LX_EPIPE);
    CHECK(snd_pcm_recover(handle, -LX_EPIPE, 0));
    expect("recovered avail", snd_pcm_avail_update(handle), 2048);
    CHECK(snd_pcm_open(&nonblock, "default", 0, LX_SND_NONBLOCK));
    CHECK(snd_pcm_hw_params_any(nonblock, &h));
    CHECK(snd_pcm_hw_params(nonblock, &h));
    CHECK(snd_pcm_sw_params_current(nonblock, &s));
    s.start_threshold = 2049; /* Disable auto-start to make fullness deterministic. */
    CHECK(snd_pcm_sw_params(nonblock, &s));
    expect("nonblocking partial writei", snd_pcm_writei(nonblock, samples, 4096), 2048);
    expect("nonblocking full writei", snd_pcm_writei(nonblock, samples, 1), -LX_EAGAIN);
    expect("full buffer wait timeout", snd_pcm_wait(nonblock, 0), 0);
    expect("unsupported float format", snd_pcm_hw_params_set_format(nonblock, &h, 14), -LX_EINVAL);
    CHECK(snd_pcm_close(nonblock));
}

/* Isolated children exercise the executable-relative configuration reader
 * without overwriting the developer/game phoenixtools.ini. Their own provider stderr
 * is the source of truth for the negotiated mode, format, period and fallback. */
static int mode_case(const WCHAR *directory, const char *mode, int blocking)
{
    WCHAR ini[MAX_PATH], exe[MAX_PATH], log_path[MAX_PATH], capture_path[MAX_PATH], command[MAX_PATH + 64];
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    HANDLE log_file, input;
    FILE *file;
    char line[2048], fallback[2048] = "";
    int exclusive = 0, shared = 0, stats = 0, tone = 0, failed = 0;
    DWORD status = 1;
    swprintf(ini, MAX_PATH, L"%ls\\phoenixtools.ini", directory);
    swprintf(exe, MAX_PATH, L"%ls\\test_snd.exe", directory);
    swprintf(log_path, MAX_PATH, L"%ls\\test.log", directory);
    swprintf(capture_path, MAX_PATH, L"%ls\\capture#\u00e9.wav", directory);
    file = _wfopen(ini, L"w");
    if (!file) return 1;
    fprintf(file, "[audio]\nmode = %s\n[debug]\naudio_stats = YeS\naudio_capture = capture#\xc3\xa9.wav\n", mode);
    fclose(file);
    log_file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) return 1;
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE) { CloseHandle(log_file); return 1; }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log_file;
    startup.hStdInput = input;
    swprintf(command, MAX_PATH + 64, L"\"%ls\" 48000 %ls hold", exe,
        blocking ? L"block" : L"game");
    printf("\n=== configured mode=%s, %s writes ===\n", mode, blocking ? "blocking 48000-frame" : "game 256-frame");
    if (!CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
            NULL, &startup, &process)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", GetLastError());
        failed = 1;
    } else {
        if (WaitForSingleObject(process.hProcess, 20000) != WAIT_OBJECT_0) {
            fprintf(stderr, "audio child timed out\n");
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            failed = 1;
        }
        GetExitCodeProcess(process.hProcess, &status);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(input);
    CloseHandle(log_file);
    file = _wfopen(log_path, L"r");
    if (!file) return 1;
    while (fgets(line, sizeof(line), file)) {
        unsigned underruns;
        unsigned long long frames;
        fputs(line, stdout);
        if (strstr(line, "lxsnd: WASAPI exclusive S16")) exclusive = 1;
        if (strstr(line, "lxsnd: WASAPI shared S16")) shared = 1;
        if (strstr(line, "lxsnd: WASAPI exclusive fallback"))
            snprintf(fallback, sizeof(fallback), "%s", line);
        if (sscanf(line, "lxsnd: frames=%llu underruns=%u", &frames, &underruns) == 2) {
            ++stats;
            if (underruns) failed = 1;
        }
        if (strstr(line, "tone=48000 frames (1 second)") &&
                strstr(line, "errors=0 thread_exit=0")) tone = 1;
    }
    fclose(file);
    file = _wfopen(capture_path, L"rb");
    if (!file) {
        failed = 1;
    } else {
        unsigned char header[80];
        if (fread(header, sizeof(header), 1, file) != 1 ||
                memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
                memcmp(header + 72, "data", 4) || fseek(file, 0, SEEK_END) ||
                ftell(file) != 80 + 48000 * 4)
            failed = 1;
        fclose(file);
    }
    if (status || !tone || !stats || exclusive == shared ||
            (!strcmp(mode, "shared") && !shared) ||
            (!strcmp(mode, "exclusive") && shared && !fallback[0]))
        failed = 1;
    printf("mode=%s obtained=%s writes=%s stats_intervals=%d underruns_zero=%s result=%s\n",
        mode, exclusive ? "exclusive" : shared ? "shared" : "none",
        blocking ? "blocking" : "game", stats, failed ? "no" : "yes", failed ? "FAIL" : "PASS");
    if (fallback[0]) printf("fallback reason: %s", fallback);
    return failed;
}

static int modes(void)
{
    WCHAR root[MAX_PATH], directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH], *slash;
    static const WCHAR *const files[] = {L"test_snd.exe", L"lxsnd.dll"};
    static const WCHAR *const cleanup[] = {L"test_snd.exe", L"lxsnd.dll", L"phoenixtools.ini", L"test.log", L"capture#\u00e9.wav"};
    unsigned i;
    int failed = 0;
    if (!GetTempPathW(MAX_PATH, root) ||
            !GetTempFileNameW(root, L"snd", 0, directory)) return 1;
    DeleteFileW(directory);
    if (!CreateDirectoryW(directory, NULL)) return 1;
    if (!GetModuleFileNameW(NULL, source, MAX_PATH)) { failed = 1; goto done; }
    slash = wcsrchr(source, L'\\');
    if (!slash) { failed = 1; goto done; }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        wcscpy(slash + 1, files[i]);
        swprintf(destination, MAX_PATH, L"%ls\\%ls", directory, files[i]);
        if (!CopyFileW(source, destination, FALSE)) {
            fprintf(stderr, "CopyFile failed: %lu\n", GetLastError());
            failed = 1;
            goto done;
        }
    }
    failed |= mode_case(directory, "shared", 0);
    failed |= mode_case(directory, "shared", 1);
    failed |= mode_case(directory, "exclusive", 0);
    failed |= mode_case(directory, "exclusive", 1);
done:
    for (i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); ++i) {
        swprintf(destination, MAX_PATH, L"%ls\\%ls", directory, cleanup[i]);
        DeleteFileW(destination);
    }
    RemoveDirectoryW(directory);
    printf("shared/exclusive suite: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}

int main(int argc, char **argv)
{
    char dll_path[MAX_PATH], *slash;
    HMODULE dll;
    lx_provider_symbols_fn provider;
    pcm *handle;
    lx_snd_hw_params *hw, *stack_hw;
    lx_snd_sw_params *sw, *stack_sw;
    unsigned rate = argc > 1 ? (unsigned)atoi(argv[1]) : 48000, i;
    int direction = 0, query_errors = 0;
    HANDLE thread;
    DWORD exit_code;
    playback b = {0};
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--modes")) return modes();
    GetModuleFileNameA(NULL, dll_path, sizeof(dll_path));
    slash = strrchr(dll_path, '\\');
    if (!slash) return 1;
    strcpy(slash + 1, "lxsnd.dll");
    dll = LoadLibraryA(dll_path);
    if (!dll) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    provider = (lx_provider_symbols_fn)GetProcAddress(dll, "lx_provider_symbols");
    if (!provider) return 1;
    table = provider(&symbol_count);
    LOAD(snd_pcm_open); LOAD(snd_pcm_close); LOAD(snd_pcm_hw_params_sizeof); LOAD(snd_pcm_sw_params_sizeof);
    LOAD(snd_pcm_hw_params_malloc); LOAD(snd_pcm_hw_params_free); LOAD(snd_pcm_hw_params_any);
    LOAD(snd_pcm_hw_params_set_access); LOAD(snd_pcm_hw_params_set_format); LOAD(snd_pcm_hw_params_set_rate_near);
    LOAD(snd_pcm_hw_params_set_channels); LOAD(snd_pcm_hw_params_set_periods); LOAD(snd_pcm_hw_params_set_buffer_size);
    LOAD(snd_pcm_hw_params); LOAD(snd_pcm_sw_params_malloc); LOAD(snd_pcm_sw_params_free);
    LOAD(snd_pcm_sw_params_current); LOAD(snd_pcm_sw_params_set_avail_min); LOAD(snd_pcm_sw_params);
    LOAD(snd_pcm_prepare); LOAD(snd_pcm_avail_update); LOAD(snd_pcm_wait); LOAD(snd_pcm_writei);
    LOAD(snd_pcm_delay); LOAD(snd_pcm_drain); LOAD(snd_pcm_recover); LOAD(snd_strerror);
    printf("provider=%u symbols; i686 pointers=%u frame_counts=%u/%u hw=%u sw=%u\n", symbol_count,
        (unsigned)sizeof(void *), (unsigned)sizeof(lx_uframes), (unsigned)sizeof(lx_sframes),
        (unsigned)snd_pcm_hw_params_sizeof(), (unsigned)snd_pcm_sw_params_sizeof());
    /* Exact initialization order from piu 0x08072739..0x08072901. */
    CHECK(snd_pcm_hw_params_malloc(&hw));
    CHECK(snd_pcm_open(&handle, "hw:0,0", 0, 0));
    CHECK(snd_pcm_hw_params_any(handle, hw));
    CHECK(snd_pcm_hw_params_set_access(handle, hw, 3));
    CHECK(snd_pcm_hw_params_set_format(handle, hw, 2));
    CHECK(snd_pcm_hw_params_set_rate_near(handle, hw, &rate, &direction));
    printf("negotiated_rate=%u direction=%d\n", rate, direction);
    CHECK(snd_pcm_hw_params_set_channels(handle, hw, 2));
    CHECK(snd_pcm_hw_params_set_periods(handle, hw, 4, 0));
    CHECK(snd_pcm_hw_params_set_buffer_size(handle, hw, 2048));
    CHECK(snd_pcm_hw_params(handle, hw));
    snd_pcm_hw_params_free(hw);
    CHECK(snd_pcm_sw_params_malloc(&sw));
    CHECK(snd_pcm_sw_params_current(handle, sw));
    CHECK(snd_pcm_sw_params_set_avail_min(handle, sw, 256));
    CHECK(snd_pcm_sw_params(handle, sw));
    snd_pcm_sw_params_free(sw);
    /* Supplemental alloca+memset ABI checks; no heap ownership inside params. */
    stack_hw = (lx_snd_hw_params *)_alloca(snd_pcm_hw_params_sizeof());
    memset(stack_hw, 0, snd_pcm_hw_params_sizeof());
    CHECK(snd_pcm_hw_params_any(handle, stack_hw));
    stack_sw = (lx_snd_sw_params *)_alloca(snd_pcm_sw_params_sizeof());
    memset(stack_sw, 0, snd_pcm_sw_params_sizeof());
    CHECK(snd_pcm_sw_params_current(handle, stack_sw));
    printf("stack params: channels=%u rate=%u periods=%u buffer=%u avail_min=%u start=%u\n",
        stack_hw->intervals[2].min, stack_hw->intervals[3].min, stack_hw->intervals[7].min,
        stack_hw->intervals[9].min, stack_sw->avail_min, stack_sw->start_threshold);
    b.handle = handle;
    b.rate = rate;
    b.blocking = argc > 2 && !strcmp(argv[2], "block");
    b.samples = (short *)malloc(rate * 4);
    if (!b.samples) return 1;
    for (i = 0; i < rate; ++i) {
        short sample = (short)(8000.0 * sin(6.283185307179586 * 440.0 * i / rate));
        b.samples[2*i] = b.samples[2*i+1] = sample;
    }
    thread = CreateThread(NULL, 0, play, &b, 0, NULL);
    if (!thread) return 1;
    while (WaitForSingleObject(thread, 200) == WAIT_TIMEOUT) {
        lx_sframes delay;
        int result = snd_pcm_delay(handle, &delay);
        printf("main delay=%ld frames rc=%d\n", (long)delay, result);
        if (result < 0) ++query_errors;
    }
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    b.errors += query_errors;
    if (argc > 2 && !strcmp(argv[2], "edges")) edges(handle, b.samples);
    if ((argc > 2 && !strcmp(argv[2], "hold")) || (argc > 3 && !strcmp(argv[3], "hold"))) {
        printf("holding drained stream open for diagnostics\n");
        Sleep(5200);
    }
    CHECK(snd_pcm_close(handle));
    free(b.samples);
    FreeLibrary(dll);
    printf("tone=%u frames (1 second), writes=%u waits=%u errors=%u thread_exit=%lu\n",
        rate, b.writes, b.waits, b.errors, exit_code);
    return exit_code || b.errors ? 1 : 0;
}
