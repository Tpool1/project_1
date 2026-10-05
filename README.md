# Project 1: Process Scheduler

PLEASE MAKE A FORK OF THIS REPO

From the src directory, ``make run`` builds and runs the simulator

Make sure to work on a utcs lab machine
For any questions, please post on Ed

## Energy-first scheduler

The scheduler runs jobs on up to four small cores at P3, uses P4 for a final
interval when it can finish at lower energy, and puts unused cores in C6.
It preserves FIFO order, handles asynchronous wakes, and checks that all jobs
have completed before reporting energy. See the [design plan](docs/energy-aware-scheduler-design.md)
and [measured results](docs/scheduler-results.md).

On a Linux lab machine, run `make test` from `src` for synthetic cases and
paired generator seeds 0–5. The tests independently check context ownership,
remaining-work accounting, completion counts, and the full-quantum energy
minimum. Run `make benchmark` to compare 17 policy configurations.

On an Apple Silicon Mac with the `gcc:13` container image installed, run:

```sh
docker run --rm --platform linux/amd64 --network none --read-only \
  --mount type=bind,source="$PWD",target=/work,readonly \
  --tmpfs /tmp:rw,exec,size=128m gcc:13 \
  make -C /work/src test
```

Energy uses the starter's display conversion; the design document explains
the simulator's inconsistent physical-unit labels. Raw counters are included
in the results for reproducible comparison.
