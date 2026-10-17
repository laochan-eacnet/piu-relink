// M3 acceptance: iostream, std::string, std::regex, thread_local, cross-module exceptions,
// std::thread, locale-dependent formatting. Deterministic output.
#include <atomic>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct ModuleError : std::runtime_error {
    int code;
    ModuleError(const std::string &m, int c) : std::runtime_error(m), code(c) {}
};
void throw_from_so(int depth);
void throw_std_from_so();

int tl_add(int v);
int tl_value();
void tl_set_name(const std::string &n);
std::string tl_get_name();
int tl_dtors();

int main()
{
    std::string s = "Pump It Up";
    s += " Phoenix";
    std::cout << "string: " << s << " len=" << s.size() << '\n';

    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << 3.14159 << ' ' << std::hex << 255 << ' ' << std::dec << -17;
    std::cout << "sstream: " << os.str() << '\n';

    std::regex re(R"((\w+)-(\d+))");
    std::string text = "song-1001 chart-42 bad- x-7";
    std::vector<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        found.push_back((*it)[1].str() + ":" + (*it)[2].str());
    std::cout << "regex:";
    for (auto &f : found)
        std::cout << ' ' << f;
    std::cout << '\n';
    std::cout << "regex_replace: " << std::regex_replace(std::string("a1b22c333"), std::regex("[0-9]+"), "#") << '\n';

    try {
        throw_from_so(5);
    } catch (const ModuleError &e) {
        std::cout << "caught ModuleError: " << e.what() << " code=" << e.code << '\n';
    }
    try {
        throw_std_from_so();
    } catch (const std::out_of_range &e) {
        std::cout << "caught out_of_range from libstdc++\n";
    }
    try {
        throw 7;
    } catch (int v) {
        std::cout << "caught int " << v << '\n';
    }

    std::mutex m;
    std::map<int, int> results;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++)
        threads.emplace_back([i, &m, &results] {
            int v = tl_add(i);
            tl_set_name("worker" + std::to_string(i));
            std::lock_guard<std::mutex> g(m);
            results[i] = v * 10 + (tl_get_name() == "worker" + std::to_string(i));
        });
    for (auto &t : threads)
        t.join();
    std::cout << "thread_local:";
    for (auto &kv : results)
        std::cout << ' ' << kv.first << '=' << kv.second;
    std::cout << " main=" << tl_value() << ' ' << tl_get_name() << " dtors=" << tl_dtors() << '\n';
    std::cerr << "stderr ok" << std::endl;
    return 0;
}
