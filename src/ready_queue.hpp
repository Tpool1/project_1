#ifndef READY_QUEUE_HPP
#define READY_QUEUE_HPP

#include "sim_types.h"

#include <cstddef>
#include <deque>
#include <optional>

class ReadyQueue {
public:
    void Enqueue(ProcessId_t pid);
    ProcessId_t PopNext();
    std::optional<ProcessId_t> PopNext(Time_t minimum_remaining);
    bool Empty() const;
    std::size_t Size() const;
    const std::deque<ProcessId_t> &Items() const;

private:
    std::deque<ProcessId_t> processes;
};

#endif
