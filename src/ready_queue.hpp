#ifndef READY_QUEUE_HPP
#define READY_QUEUE_HPP

#include "sim_types.h"

#include <cstddef>
#include <deque>

class ReadyQueue {
public:
    void Enqueue(ProcessId_t pid);
    ProcessId_t PopNext();
    bool Empty() const;
    std::size_t Size() const;

private:
    std::deque<ProcessId_t> processes;
};

#endif
