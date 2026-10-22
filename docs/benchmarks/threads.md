# Threads: spawn or pool?

**Benchmark:** `thread.create` → results `thread.create_to_start`, `thread.exit_to_join` ·
source: [src/benchmarks/threads.cpp](../../src/benchmarks/threads.cpp)

## Goal

Find out what starting a thread costs on this machine, typically and in the
tail, so you can decide whether work on the hot path may spawn threads.

## What is measured

Two timestamps around the life of a `std::thread`:

```cpp
st.before_create = olt::now_ns();
std::thread th([&st] {
    st.entered = olt::now_ns();      // create_to_start = entered - before_create
    st.exiting = olt::now_ns();
});
th.join();
st.joined = olt::now_ns();           // exit_to_join = joined - exiting
```

`create_to_start` includes `clone()`, stack allocation (mmap + guard page),
TLS setup and the scheduler placing the new thread on a CPU.

## Reading the result

* p50 is usually a few microseconds; p99 and max are often 10 to 100x higher
  (page faults on a fresh stack, the scheduler choosing a busy CPU, a CPU
  waking from idle).
* Compare with your latency budget: if one request must finish in 20 us, a
  thread start with a p99 of 50 us cannot be on its path.

## Using it in C++

Create threads once, at start-up, and hand them work:

```cpp
// Hot path: no thread creation, just a queue push.
class Worker {
public:
    Worker() : thread_([this] { run(); }) {}
    ~Worker() { queue_.push(nullptr); thread_.join(); }
    void submit(Task* t) { queue_.push(t); }   // e.g. an SPSC ring, see pattern-spsc.md
private:
    void run() { while (Task* t = queue_.pop()) t->execute(); }
    SpscQueue<Task*> queue_;
    std::thread thread_;
};
```

* Size the pool to the cores you actually dedicate, pin each worker
  (`pthread_setaffinity_np`), and create it before the latency-critical phase.
* `std::async` and `std::thread` per task are fine off the hot path, where the
  simplicity is worth more than microseconds.
