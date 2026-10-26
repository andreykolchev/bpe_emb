// Desktop demo of the engine: runs the embedded PROCESS_JSON with simulated
// workers and prints every step the engine reports to the observer.
#include "engine.h"
#include "process.h"
#include "process_data.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>


using Clock = std::chrono::steady_clock;

// Milliseconds since the first call; the clock the engine runs on.
static uint32_t now()
{
    static const Clock::time_point start = Clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

namespace
{
    // Simulates the workers. Each capability has one worker; a task is "sent" to
    // a free worker, and its answer is handed to the engine by deliverAnswer().
    class WsTaskDispatcher : public core::TaskDispatcher
    {
    public:
        WsTaskDispatcher() : workers_{
            {"measure_temperature", false, ""},
            {"measure_humidity", false, ""}
        }
        {
        }

        // TaskDispatcher: the engine asks us to run a task on a worker.
        core::DispatchResult sendTask(const core::Node& node, std::string& taskId) override
        {
            Worker* worker = nullptr;
            bool hasCapability = false;
            for (Worker& candidate : workers_)
            {
                if (candidate.capability != node.externalTaskType)
                {
                    continue;
                }
                hasCapability = true;
                if (!candidate.busy)
                {
                    worker = &candidate;
                    break;
                }
            }
            // No worker has the capability at all, versus all of them being busy.
            if (!hasCapability)
            {
                return core::DispatchResult::NoWorker;
            }
            if (!worker)
            {
                return core::DispatchResult::Busy;
            }

            // Mark the worker busy and queue its answer for later delivery.
            taskId = "task-" + std::to_string(++sent_);
            worker->busy = true;
            worker->taskId = taskId;
            answers_.push_back({taskId, fakeResult(node.externalTaskType)});
            std::cout << "sent " << node.externalTaskType << " as " << taskId << "\n";
            return core::DispatchResult::Sent;
        }

        // TaskDispatcher: the engine no longer needs a task's answer.
        void cancelTask(const std::string& taskId) override
        {
            freeWorker(taskId);
            // Drop the queued answer, so it is never delivered.
            for (auto it = answers_.begin(); it != answers_.end(); ++it)
            {
                if (it->taskId == taskId)
                {
                    answers_.erase(it);
                    return;
                }
            }
        }

        // Hand the engine every answer that is waiting.
        void deliverAnswer(core::Engine& engine)
        {
            // Take the answers out first: onTaskResult() may send new tasks,
            // which would change answers_ while we loop over it.
            std::vector<WorkerResponse> ready;
            ready.swap(answers_);
            for (const auto& [taskId, resultJson] : ready)
            {
                freeWorker(taskId);
                engine.onTaskResult(taskId, true, resultJson);
            }
        }

    private:
        struct Worker
        {
            std::string capability;
            bool busy = false;
            std::string taskId; // the task this worker is running, if busy
        };

        struct WorkerResponse
        {
            std::string taskId;
            std::string resultJson;
        };

        // A fixed JSON answer for each capability.
        static std::string fakeResult(const std::string& capability)
        {
            if (capability == "measure_temperature")
            {
                return R"({"temperature_c":21.5})";
            }
            return R"({"humidity_pct":50})";
        }

        // Make the worker that ran taskId free again.
        void freeWorker(const std::string& taskId)
        {
            for (Worker& w : workers_)
            {
                if (w.busy && w.taskId == taskId)
                {
                    w.busy = false;
                    w.taskId.clear();
                }
            }
        }

        std::vector<Worker> workers_;
        std::vector<WorkerResponse> answers_;
        int sent_ = 0;
    };

    // Prints each engine event to stdout, prefixed with DASHBOARD.
    class DashboardProcessObserver : public core::ProcessObserver
    {
    public:
        void log(const std::string& message) override
        {
            std::cout << "LOG: " << message << "\n";
        }

        void processStarted(const core::Process& process) override
        {
            std::cout << "DASHBOARD -> processStarted " << process.id << "\n";
        }

        void nodeActivated(const core::Node& node, uint32_t runs) override
        {
            std::cout << "DASHBOARD -> nodeActivated " << node.id << " runs: " << runs << "\n";
        }

        void taskCompleted(const core::Node& node, const std::string& json) override
        {
            std::cout << "DASHBOARD -> taskCompleted " << node.id << ": " << json << "\n";
        }

        void variableSet(const std::string& name, const std::string& message) override
        {
            std::cout << "DASHBOARD -> variableSet " << name << ": " << message << "\n";
        }

        void processFinished(core::ProcessStatus, const std::string& message) override
        {
            std::cout << "DASHBOARD -> processFinished " << ": " << message << "\n";
        }
    };
}

int main()
{
    // Parse the embedded process; stop if its JSON is invalid.
    core::Process process;
    std::string error;

    if (!core::parseProcess(PROCESS_JSON, process, error))
    {
        std::cout << "Embedded process is invalid: " << error << "\n";
        return EXIT_FAILURE;
    }

    WsTaskDispatcher dispatcher;
    DashboardProcessObserver observer;
    core::Engine engine(dispatcher, &observer);
    if (!engine.start(process))
    {
        return EXIT_FAILURE;
    }

    // Main loop: advance the engine, hand it finished answers, then yield briefly.
    while (engine.status() == core::ProcessStatus::Running)
    {
        engine.tick(now());
        //emulation of periodical listening
        dispatcher.deliverAnswer(engine);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "process ended: "
        << (engine.status() == core::ProcessStatus::Completed
                ? "completed"
                : "failed")
        << "\n";
    return engine.status() == core::ProcessStatus::Completed
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
