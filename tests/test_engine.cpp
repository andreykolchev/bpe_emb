#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "engine.h"
#include "process_data.h"

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

using core::ProcessStatus;

// Fake worker pool: accepts every task as "task-N" and answers it on deliver().
class FakeHost : public core::TaskDispatcher, public core::ProcessObserver {
 public:
  bool verbose = true;             // print the engine's log lines
  bool noWorker = false;           // no worker has the capability
  bool failing = false;            // workers answer with an error
  bool silent = false;             // workers never answer
  std::vector<std::string> tasks;  // ids of every task sent

  core::DispatchResult sendTask(const core::Node&, std::string& taskId) override {
    if (noWorker) {
      return core::DispatchResult::NoWorker;
    }
    taskId = "task-" + std::to_string(tasks.size() + 1);
    tasks.push_back(taskId);
    if (!silent) {
      pending_.push_back(taskId);
    }
    return core::DispatchResult::Sent;
  }

  // Cancelled tasks still answer, so tests can check that late results are ignored.
  void cancelTask(const std::string&) override {}

  // Hands the engine the answers to every task sent since the last call.
  void deliver(core::Engine& engine) {
    std::vector<std::string> pending;
    pending.swap(pending_);
    for (const std::string& taskId : pending) {
      engine.onTaskResult(taskId, !failing, failing ? "worker_error" : R"({"value":21.5})");
    }
  }

  void processStarted(const core::Process&) override {}
  void nodeActivated(const core::Node&, uint32_t) override {}
  void taskCompleted(const core::Node&, const std::string&) override {}
  void variableSet(const std::string&, const std::string&) override {}
  void processFinished(core::ProcessStatus, const std::string&) override {}

  void log(const std::string& message) override {
    if (verbose) {
      std::cout << message << "\n";
    }
  }

 private:
  std::vector<std::string> pending_;
};

// Runs `engine` on a simulated clock (1 ms steps) until it stops running.
// Returns the simulated time spent.
static uint32_t runToEnd(core::Engine& engine, FakeHost& host, uint32_t limitMs = 60000) {
  uint32_t now = 0;
  while (engine.status() == ProcessStatus::Running && now < limitMs) {
    engine.tick(now);
    host.deliver(engine);
    now++;
  }
  return now;
}

static core::Process load(const std::string& json) {
  core::Process p;
  std::string error;
  if (!core::parseProcess(json.c_str(), p, error)) {
    std::cout << "bad test process: " << error << "\n";
    std::exit(EXIT_FAILURE);
  }
  return p;
}

static void testEmbeddedProcessRunsThreeTimes() {
  core::Process p = load(PROCESS_JSON);
  FakeHost host;
  host.verbose = false;
  core::Engine engine(host, &host);
  CHECK(engine.start(p));
  uint32_t elapsed = runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Completed);
  CHECK(host.tasks.size() == 3);    // 3 iterations of the whole process
  CHECK(elapsed >= 3 * 2000);       // 3 delays of 2000 ms
  CHECK(elapsed < 3 * 2000 + 100);  // and nothing else slow
}

static void testTaskFailures() {
  core::Process p = load(PROCESS_JSON);

  {
    FakeHost host;
    host.verbose = false;
    host.noWorker = true;
    core::Engine engine(host, &host);
    engine.start(p);
    runToEnd(engine, host);
    CHECK(engine.status() == ProcessStatus::Failed);
  }
  {
    FakeHost host;
    host.verbose = false;
    host.failing = true;
    core::Engine engine(host, &host);
    engine.start(p);
    runToEnd(engine, host);
    CHECK(engine.status() == ProcessStatus::Failed);
    CHECK(host.tasks.size() == 1);
  }
  {
    FakeHost host;  // worker never answers: times out after 5000 ms
    host.verbose = false;
    host.silent = true;
    core::Engine engine(host, &host);
    engine.start(p);
    uint32_t elapsed = runToEnd(engine, host);
    CHECK(engine.status() == ProcessStatus::Failed);
    CHECK(elapsed >= 5000 && elapsed < 5100);
  }
}

static void testStopAndRestart() {
  core::Process p = load(PROCESS_JSON);
  FakeHost host;
  host.verbose = false;
  core::Engine engine(host, &host);
  CHECK(!engine.stop());  // nothing running
  CHECK(engine.start(p));
  CHECK(!engine.start(p));  // already running
  engine.tick(0);
  CHECK(engine.stop());
  CHECK(engine.status() == ProcessStatus::Stopped);
  host.deliver(engine);  // a late result is ignored
  CHECK(engine.status() == ProcessStatus::Stopped);

  CHECK(engine.start(p));  // a fresh execution starts from scratch
  runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Completed);
}

static void testRouterAndVariables() {
  // set x=5; route to "big" if x > 3, else to "small"
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"set","type":"InternalTask","internalTaskType":"SetVariable",
     "variable":"x","value":5},
    {"id":"r","type":"Router"},
    {"id":"big","type":"Stop"},
    {"id":"small","type":"Stop"}],
   "routes":[{"from":"s","to":"set"},{"from":"set","to":"r"},
    {"from":"r","to":"small"},
    {"from":"r","to":"big",
     "condition":{"variable":"x","op":">","value":3}}]}})");
  FakeHost host;
  host.verbose = false;
  core::Engine engine(host, &host);
  engine.start(p);
  runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Completed);

  // a router with no matching route and no default fails
  core::Process q = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},{"id":"r","type":"Router"},{"id":"e","type":"Stop"}],
   "routes":[{"from":"s","to":"r"},
    {"from":"r","to":"e","condition":{"variable":"nope","op":"==","value":1}}]}})");
  engine.start(q);
  runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Failed);
}

static void testRunawayLoopFails() {
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},{"id":"r","type":"Router"}],
   "routes":[{"from":"s","to":"r"},{"from":"r","to":"r"}]}})");
  FakeHost host;
  host.verbose = false;
  core::Engine engine(host, &host);
  engine.start(p);
  runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Failed);
}

static void testNoStartNode() {
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"e","type":"Stop"}],"routes":[]}})");
  FakeHost host;
  host.verbose = false;
  core::Engine engine(host, &host);
  CHECK(!engine.start(p));
  CHECK(engine.status() == ProcessStatus::Idle);
}

// Remembers the nodes the engine went to, the variables it set and its log lines.
class Recorder : public core::ProcessObserver {
 public:
  std::vector<std::string> nodes;
  std::vector<std::string> variables;  // "name = json"
  std::vector<std::string> logs;

  void log(const std::string& message) override { logs.push_back(message); }

  void processStarted(const core::Process&) override {}

  void nodeActivated(const core::Node& node, uint32_t) override { nodes.push_back(node.id); }

  void taskCompleted(const core::Node&, const std::string&) override {}

  void variableSet(const std::string& name, const std::string& valueJson) override {
    variables.push_back(name + " = " + valueJson);
  }

  void processFinished(core::ProcessStatus, const std::string&) override {}
};

static bool logged(const Recorder& rec, const char* part) {
  for (const std::string& line : rec.logs) {
    if (line.find(part) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Every worker is busy, so a task can never be sent.
class BusyDispatcher : public core::TaskDispatcher {
 public:
  core::DispatchResult sendTask(const core::Node&, std::string&) override { return core::DispatchResult::Busy; }

  void cancelTask(const std::string&) override {}
};

// Sets x to valueJson, then routes on `condition` (a JSON condition object).
// Returns "yes" or "no", the Stop node the engine reached, or "failed".
static std::string routeOf(const std::string& valueJson, const std::string& condition) {
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"set","type":"InternalTask","internalTaskType":"SetVariable",
     "variable":"x","value":)" +
                         valueJson + R"(},
    {"id":"r","type":"Router"},
    {"id":"no","type":"Stop"},{"id":"yes","type":"Stop"}],
   "routes":[{"from":"s","to":"set"},{"from":"set","to":"r"},
    {"from":"r","to":"no"},
    {"from":"r","to":"yes","condition":)" +
                         condition + R"(}]}})");
  FakeHost host;
  host.verbose = false;
  Recorder rec;
  core::Engine engine(host, &rec);
  engine.start(p);
  runToEnd(engine, host);
  if (engine.status() != ProcessStatus::Completed) {
    return "failed";
  }
  return rec.nodes.back();
}

static void testConditionOperators() {
  // numbers compare as numbers
  CHECK(routeOf("5", R"({"variable":"x","op":">","value":3})") == "yes");
  CHECK(routeOf("5", R"({"variable":"x","op":">=","value":5})") == "yes");
  CHECK(routeOf("5", R"({"variable":"x","op":"<","value":5})") == "no");
  CHECK(routeOf("5", R"({"variable":"x","op":"!=","value":5.0})") == "no");

  // text and booleans can only be equal or not equal
  CHECK(routeOf(R"("abc")", R"({"variable":"x","op":"==","value":"abc"})") == "yes");
  CHECK(routeOf(R"("abc")", R"({"variable":"x","op":"==","value":"ABC"})") == "no");
  CHECK(routeOf(R"("abc")", R"({"variable":"x","op":"!=","value":"ABC"})") == "yes");
  CHECK(routeOf(R"("abc")", R"({"variable":"x","op":"<","value":"abd"})") == "no");
  CHECK(routeOf("true", R"({"variable":"x","op":"==","value":true})") == "yes");

  // a number is never equal to the text of a number
  CHECK(routeOf("5", R"({"variable":"x","op":"==","value":"5"})") == "no");

  // a missing variable, or a path that does not exist, never holds
  CHECK(routeOf("5", R"({"variable":"missing","op":"!=","value":1})") == "no");
  CHECK(routeOf("5", R"({"variable":"x.nope","op":"==","value":5})") == "no");

  // a dotted path reads into an object
  CHECK(routeOf(R"({"temperature_c":21.5})", R"({"variable":"x.temperature_c","op":">","value":21})") == "yes");
}

static void testIncrement() {
  // a missing variable starts at 0
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"inc","type":"InternalTask","internalTaskType":"Increment",
     "variable":"n","increment":5},
    {"id":"e","type":"Stop"}],
   "routes":[{"from":"s","to":"inc"},{"from":"inc","to":"e"}]}})");
  FakeHost host;
  host.verbose = false;
  Recorder rec;
  core::Engine engine(host, &rec);
  engine.start(p);
  runToEnd(engine, host);
  CHECK(engine.status() == ProcessStatus::Completed);
  CHECK(rec.variables.size() == 1);
  CHECK(!rec.variables.empty() && rec.variables[0].find("n = 5") == 0);

  // a variable that is not a number fails the process
  core::Process q = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"set","type":"InternalTask","internalTaskType":"SetVariable",
     "variable":"n","value":"text"},
    {"id":"inc","type":"InternalTask","internalTaskType":"Increment","variable":"n"},
    {"id":"e","type":"Stop"}],
   "routes":[{"from":"s","to":"set"},{"from":"set","to":"inc"},{"from":"inc","to":"e"}]}})");
  Recorder rec2;
  core::Engine engine2(host, &rec2);
  engine2.start(q);
  runToEnd(engine2, host);
  CHECK(engine2.status() == ProcessStatus::Failed);
  CHECK(logged(rec2, "not_a_number"));
}

static void testTaskResults() {
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"temp","type":"ExternalTask","externalTaskType":"temperature",
     "resultVariable":"reading","timeoutMs":5000},
    {"id":"e","type":"Stop"}],
   "routes":[{"from":"s","to":"temp"},{"from":"temp","to":"e"}]}})");

  // an answer for another task is ignored; the real one still completes the process
  {
    FakeHost host;
    host.verbose = false;
    Recorder rec;
    core::Engine engine(host, &rec);
    engine.start(p);
    engine.tick(0);  // sends the task, which is "task-1"
    engine.onTaskResult("task-99", true, "{}");
    CHECK(engine.status() == ProcessStatus::Running);
    CHECK(logged(rec, "ignored result of task-99"));
    host.deliver(engine);
    engine.tick(0);  // the Stop node finishes on the next tick
    CHECK(engine.status() == ProcessStatus::Completed);
    CHECK(rec.variables.size() == 1);
    CHECK(!rec.variables.empty() && rec.variables[0].find("reading = {") == 0);
  }

  // an answer that is not JSON fails the process
  {
    FakeHost host;
    host.verbose = false;
    Recorder rec;
    core::Engine engine(host, &rec);
    engine.start(p);
    engine.tick(0);
    engine.onTaskResult("task-1", true, "{oops");
    CHECK(engine.status() == ProcessStatus::Failed);
    CHECK(logged(rec, "bad_result"));
  }
}

static void testBusyWorkerTimesOut() {
  core::Process p = load(R"({"process":{"id":"t","nodes":[
    {"id":"s","type":"Start"},
    {"id":"temp","type":"ExternalTask","externalTaskType":"temperature","timeoutMs":300},
    {"id":"e","type":"Stop"}],
   "routes":[{"from":"s","to":"temp"},{"from":"temp","to":"e"}]}})");
  BusyDispatcher busy;
  Recorder rec;
  core::Engine engine(busy, &rec);
  engine.start(p);
  uint32_t now = 0;
  while (engine.status() == ProcessStatus::Running && now < 1000) {
    engine.tick(now);
    now++;
  }
  CHECK(engine.status() == ProcessStatus::Failed);
  CHECK(logged(rec, "dispatch_timeout"));
  CHECK(now >= 300 && now < 310);  // waited for the timeout, not longer
}

int main() {
  testEmbeddedProcessRunsThreeTimes();
  testTaskFailures();
  testStopAndRestart();
  testRouterAndVariables();
  testRunawayLoopFails();
  testNoStartNode();
  testConditionOperators();
  testIncrement();
  testTaskResults();
  testBusyWorkerTimesOut();
  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all engine tests passed\n";
  return EXIT_SUCCESS;
}
