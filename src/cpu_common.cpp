// Per-thread CPU stack, shared by both backends.
#include "cpu.h"

#include <pthread.h>

#include <mutex>
#include <vector>

namespace {
// No thread_local here: on libhybris the Android GL driver writes bionic TLS
// slots that overlap the executable's static TLS block.
struct ThreadCpus {
    pthread_t thread;
    std::vector<Cpu *> stack;
};
std::mutex g_threads_mutex;
std::vector<ThreadCpus *> g_threads;

std::vector<Cpu *> &cpu_stack() {
    pthread_t self = pthread_self();
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (ThreadCpus *t : g_threads)
        if (pthread_equal(t->thread, self)) return t->stack;
    g_threads.push_back(new ThreadCpus{self, {}});
    return g_threads.back()->stack;
}
}  // namespace

namespace cpu_thread {
void push(Cpu *cpu) { cpu_stack().push_back(cpu); }
void pop() { cpu_stack().pop_back(); }
Cpu *top() {
    std::vector<Cpu *> &stack = cpu_stack();
    return stack.empty() ? nullptr : stack.back();
}
}  // namespace cpu_thread

Cpu &Cpu::current() {
    Cpu *cpu = cpu_thread::top();
    if (!cpu) fatal("no guest CPU on this thread");
    return *cpu;
}
