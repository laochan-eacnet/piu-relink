// M3: exception thrown inside a relinked shared object, caught in the executable; thread_local
// objects with dynamic initialization and destructors (general-dynamic TLS + __cxa_thread_atexit_impl).
// thread_local lives here because guest *executables* may not use TLS: local-exec %gs:-N accesses
// cannot be rewritten (piu has none).
#include <atomic>
#include <stdexcept>
#include <string>

static thread_local int tl_counter = 5;
static thread_local std::string tl_name = "main";

struct Tracker {
    static std::atomic<int> dtors;
    ~Tracker() { dtors++; }
};
std::atomic<int> Tracker::dtors{0};
static thread_local Tracker tl_tracker;

int tl_add(int v) { (void)tl_tracker; return tl_counter += v; }
int tl_value() { return tl_counter; }
void tl_set_name(const std::string &n) { tl_name = n; }
std::string tl_get_name() { return tl_name; }
int tl_dtors() { return Tracker::dtors.load(); }

struct ModuleError : std::runtime_error {
    int code;
    ModuleError(const std::string &m, int c) : std::runtime_error(m), code(c) {}
};

void throw_from_so(int depth)
{
    if (depth == 0)
        throw ModuleError("thrown in libthrow.so", 42);
    throw_from_so(depth - 1);
}

void throw_std_from_so()
{
    std::string s("abc");
    (void)s.at(10); // std::out_of_range from libstdc++ itself
}
