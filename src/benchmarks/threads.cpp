// Thread lifecycle latencies: how long it takes for a newly created thread to
// start running, and for join() to return once a thread has finished.

#include <thread>
#include <vector>

#include "olt/benchmark.hpp"

namespace {

struct Stamps {
    uint64_t before_create = 0;
    uint64_t entered = 0;
    uint64_t exiting = 0;
    uint64_t joined = 0;
};

}  // namespace

OLT_BENCHMARK("thread.create",
              "std::thread spawn: time from just before construction to the first instruction in the "
              "new thread (thread.create_to_start), and from its last instruction to join() returning "
              "(thread.exit_to_join)")
{
    const size_t total = ctx.config().warmup + ctx.config().samples;
    std::vector<Stamps> s(total);
    for (size_t i = 0; i < total; ++i) {
        Stamps& st = s[i];
        st.before_create = olt::now_ns();
        std::thread th([&st] {
            st.entered = olt::now_ns();
            st.exiting = olt::now_ns();
        });
        th.join();
        st.joined = olt::now_ns();
    }

    std::vector<double> start, join;
    start.reserve(ctx.config().samples);
    join.reserve(ctx.config().samples);
    for (size_t i = ctx.config().warmup; i < total; ++i) {
        start.push_back(static_cast<double>(s[i].entered - s[i].before_create));
        join.push_back(static_cast<double>(s[i].joined - s[i].exiting));
    }
    const char* note = "new threads inherit the main thread's affinity";
    ctx.add_result("thread.create_to_start", {}, std::move(start), note);
    ctx.add_result("thread.exit_to_join", {}, std::move(join), note);
}
