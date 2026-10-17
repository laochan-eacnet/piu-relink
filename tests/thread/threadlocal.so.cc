#include <pthread.h>
static volatile int destroyed;
struct counted {
    int value;
    counted() : value(42) {}
    ~counted() { if (value == 42) __sync_fetch_and_add(&destroyed, 1); }
};
extern "C" int touch_tls()
{
    static thread_local counted object;
    return object.value;
}
extern "C" int tls_destroyed() { return __atomic_load_n(&destroyed, __ATOMIC_ACQUIRE); }
