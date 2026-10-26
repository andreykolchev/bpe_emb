# bpe

A tiny business-process engine for embedded devices (ESP32 and similar).
A process is a graph described in JSON. The engine walks it one step at a time
from your `loop()`, sends tasks to workers, waits for their answers, and
branches on process variables. It does no I/O of its own and never blocks.

- Requires C++17 (the library uses structured bindings and `if` initializers);
  uses `std::string`, `std::vector`, `std::map`
- Only dependency: [ArduinoJson](https://arduinojson.org) 7.x
- Transport-agnostic: you implement `core::TaskDispatcher` (WebSocket, MQTT, UART, ...)
  and, optionally, `core::ProcessObserver` to see what the process is doing

## Layout

| Path | What |
|---|---|
| `src/` | **The library**: `process.*` (JSON parser), `engine.*` (executor), `config.h` (limits) |
| `examples/coordinator` | Desktop demo: `main.cpp` (coordinator), `process_runner.h` (fake workers used by the tests, plus a WebSocket worker interface sketch), sample process |
| `tests/` | Unit tests (host only) |
| `third_party/` | Vendored ArduinoJson 7.2.1 (MIT), used only by the host build |

## Process format

```json
{
  "process": {
    "id": "temperature-monitor",
    "nodes": [
      { "id": "start", "type": "Start" },
      { "id": "temp", "type": "ExternalTask", "externalTaskType": "temperature",
        "resultVariable": "temperature", "timeoutMs": 5000, "context": {} },
      { "id": "wait", "type": "InternalTask", "internalTaskType": "Delay", "delayMs": 2000 },
      { "id": "count", "type": "InternalTask", "internalTaskType": "Increment",
        "variable": "iteration", "increment": 1 },
      { "id": "router", "type": "Router" },
      { "id": "stop", "type": "Stop" }
    ],
    "routes": [
      { "from": "start", "to": "temp" },
      { "from": "temp", "to": "wait" },
      { "from": "wait", "to": "count" },
      { "from": "count", "to": "router" },
      { "from": "router", "to": "temp",
        "condition": { "variable": "iteration", "op": "<", "value": 3 } },
      { "from": "router", "to": "stop" }
    ]
  }
}
```

Node types:

- `Start`, `Stop`: where an execution begins and ends.
- `ExternalTask`: runs on a worker. Its `resultVariable` (optional) receives the worker's JSON result.
- `InternalTask`: runs on the coordinator. `Delay` waits `delayMs`; `Increment` adds `increment` (default 1) to a number variable, starting from 0 if it is missing; `SetVariable` stores `value` (any JSON) in `variable`.
- `Router`: takes the first outgoing route whose condition holds. A route without a condition is the fallback, taken only if no other route matches.

### Conditions

Conditions are tested only on routes that leave a `Router`. `value` is JSON.

- Numbers compare with `==  !=  <  <=  >  >=`.
- Text and booleans compare only with `==` and `!=`. A number is never equal to the text of a number.
- A missing variable, or a path that does not exist, never holds.
- A dotted variable name reads into an object: `"temperature.temperature_c"` is the `temperature_c` field of the variable `temperature`.

## Usage

```cpp
#include "engine.h"

class MyDispatcher : public core::TaskDispatcher, public core::ProcessObserver {
public:
  // TaskDispatcher: run node.externalTaskType on a free worker and set taskId.
  // Return NoWorker if no worker has the capability, Busy if all of them are busy.
  // The answer comes later via engine.onTaskResult(), never from inside sendTask().
  core::DispatchResult sendTask(const core::Node &node, std::string &taskId) override {
    // ... send node.externalTaskType and node.context to a free worker, set taskId, then:
    // return core::DispatchResult::Sent;
    return core::DispatchResult::NoWorker;
  }

  // TaskDispatcher: the engine no longer needs this task's answer.
  void cancelTask(const std::string &taskId) override { /* tell the worker to drop it */ }

  // ProcessObserver: every callback is optional; empty bodies are fine.
  void log(const std::string &m) override { Serial.println(m.c_str()); }
  void processStarted(const core::Process &) override {}
  void nodeActivated(const core::Node &, uint32_t) override {}
  void taskCompleted(const core::Node &, const std::string &) override {}
  void variableSet(const std::string &, const std::string &) override {}
  void processFinished(core::ProcessStatus, const std::string &) override {}
};

core::Process process;          // must outlive the engine
MyDispatcher dispatcher;
core::Engine engine(dispatcher, &dispatcher);  // the observer pointer may be nullptr

void setup() {
  std::string error;
  if (!core::parseProcess(PROCESS_JSON, process, error)) {
    Serial.println(error.c_str());   // invalid process JSON
  } else {
    engine.start(process);           // false if already running or no Start node
  }
}

void loop() {
  engine.tick(millis());        // never blocks; runs at most STEPS_PER_TICK steps
  // when a worker answers: engine.onTaskResult(taskId, ok, resultJson);
  if (engine.status() == core::ProcessStatus::Completed) { /* done */ }
}
```

When a worker answers, call `engine.onTaskResult()`. An answer for a task the
engine is no longer waiting for is ignored and logged.

### Failures

A failed process has status `ProcessStatus::Failed`. The engine logs the reason
through `ProcessObserver::log()` as `Execution failed at '<node>': <reason>`.
The reasons are:

- `timeout`: a worker did not answer within the node's `timeoutMs`.
- `dispatch_timeout`: every worker with the capability stayed busy for `timeoutMs`.
- `no_worker`: no worker has the capability.
- `task_failed`: the worker answered with an error.
- `bad_result`: the worker's answer is not valid JSON.
- `not_a_number`: an `Increment` target is not a number.
- `too_many_runs`: a node was reached more than `MAX_NODE_RUNS` times.
- `no_route`: a Router has no route whose condition holds and no fallback.
- `dead_end`: a node that needs an outgoing route has none.
- `unsupported_node`, `unknown_handler`: a node type the engine cannot run.

Limits live in `src/config.h` (`DEFAULT_TASK_TIMEOUT_MS`, `MAX_NODE_RUNS`,
`STEPS_PER_TICK`).

## Using it in a project

- **PlatformIO**: copy or symlink this repo into `lib/bpe`, or reference it in
  `lib_deps`. `library.json` declares the ArduinoJson dependency.
- **Arduino IDE**: install ArduinoJson 7 from the Library Manager, then add this
  folder as a library (`library.properties` is included).
- Make sure the board's toolchain is set to C++17 (for example `-std=gnu++17`).
- ArduinoJson must be built with `std::string` support (the default on Arduino/ESP32).
- Memory: the engine uses the heap (`std::string`, `std::vector`, `std::map`).
  Parse the process once at boot and keep it.

## Build and test on a PC

```
make test    # unit tests (bin/test_process and bin/test_engine)
make run     # run the demo process with simulated workers
make debug   # -O0 -g build for gdb
```

Needs g++ with C++17.
