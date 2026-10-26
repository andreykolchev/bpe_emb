#pragma once

#include <cstdint>

// Time a worker has to answer
constexpr uint32_t DEFAULT_TASK_TIMEOUT_MS = 5000;

// A node that runs this many times in one execution fails it, so a loop whose
// condition never turns false cannot run forever.
constexpr uint32_t MAX_NODE_RUNS = 1000;

// Steps the engine takes in one tick();
// the rest wait for the next tick,
// so a loop of instant nodes cannot block the main loop().
constexpr int STEPS_PER_TICK = 32;
