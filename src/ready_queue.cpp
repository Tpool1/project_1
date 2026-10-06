#include "ready_queue.hpp"

#include "interfaces.h"

#include <algorithm>
#include <stdexcept>

void ReadyQueue::Enqueue(ProcessId_t pid) {
    const Time_t remaining = GetRemaining(pid);
    // Queue processes with less time remaining higher in the queue
    const auto position = std::find_if(processes.begin(), processes.end(), [remaining](ProcessId_t queued_pid) {
        return GetRemaining(queued_pid) > remaining;
    });
    processes.insert(position, pid);
}

ProcessId_t ReadyQueue::PopNext() {
    if (processes.empty())
        throw std::out_of_range("Cannot pop from an empty ready queue");

    const ProcessId_t pid = processes.front();
    processes.pop_front();
    return pid;
}

bool ReadyQueue::Empty() const {
    return processes.empty();
}

std::size_t ReadyQueue::Size() const {
    return processes.size();
}
