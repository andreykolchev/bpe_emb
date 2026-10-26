#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "process.h"

namespace core {
  enum class ProcessStatus { Idle, Running, Completed, Failed, Stopped };

  // What the coordinator did with a task the engine asked to send.
  enum class DispatchResult {
    Sent,      // on its way to a worker; taskId identifies it
    NoWorker,  // no worker has the capability
    Busy       // workers have it, but none is idle right now
  };

  // Process task dispatcher interface
  class TaskDispatcher {
   public:
    virtual ~TaskDispatcher() = default;
    virtual DispatchResult sendTask(const Node& node, std::string& taskId) = 0;
    virtual void cancelTask(const std::string& taskId) = 0;
  };

  // Process status observer interface
  class ProcessObserver {
   public:
    virtual ~ProcessObserver() = default;
    virtual void processStarted(const Process& process) = 0;
    virtual void nodeActivated(const Node& node, uint32_t runs) = 0;
    virtual void taskCompleted(const Node& node, const std::string& resultJson) = 0;
    virtual void variableSet(const std::string& name, const std::string& valueJson) = 0;
    virtual void processFinished(ProcessStatus status, const std::string& message) = 0;
    virtual void log(const std::string& message) = 0;
  };

  // Active node phase
  enum class Phase { Ready, Waiting };

  class Engine {
   public:
    // `observer` may be null.
    explicit Engine(TaskDispatcher& dispatcher, ProcessObserver* observer = nullptr)
        : taskDispatcher_(dispatcher), processObserver_(observer) {}

    // Init a new process
    bool start(const Process& process);

    // Stop running process execution
    bool stop();

    // Run the active node; call from the main loop
    void tick(uint32_t now);

    // Callback for task result. Worker answer with 'taskId' and 'resultData'
    void onTaskResult(const std::string& taskId, bool ok, const std::string& resultData);

    // Getter for process status
    ProcessStatus status() const { return status_; }

   private:
    // callbacks
    TaskDispatcher& taskDispatcher_;
    ProcessObserver* processObserver_;
    // process
    const Process* process_ = nullptr;
    ProcessStatus status_ = ProcessStatus::Idle;
    // active node
    int activeNodeIdx_ = -1;                // position of the active node in process->nodes
    Phase activeNodePhase_ = Phase::Ready;  // execution phase
    uint32_t activeNodeDeadline_ = 0;       // when the 'Waiting' node gives up
    std::string activeNodeTaskId_;          // the task id the 'Waiting' node waits for
    bool activeNodeQueued_ = false;         // the task waits for an idle worker
    // process data layer
    std::vector<uint32_t> runs_;                      // times each node was reached
    std::map<std::string, std::string> processData_;  // aggregated process data

    void activateNode(int idx);
    bool executeActiveNode(uint32_t now);
    void runInternalTask(const Node& node, uint32_t now);
    int chooseRoute(const Node& router) const;
    bool conditionHolds(const Condition& condition) const;
    bool incrementVariable(const Node& node);
    void setVariable(const std::string& name, const std::string& valueJson);
    void moveOn(int to = -1);
    void wait(uint32_t deadline);
    void finish(ProcessStatus status, const std::string& message);
    void fail(const std::string& error);
    void log(const std::string& message) const;
  };
}  // namespace core
