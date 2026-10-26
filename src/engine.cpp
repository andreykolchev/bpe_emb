#include "engine.h"
#include "config.h"

#include "process.h"
#include <ArduinoJson.h>

namespace core
{
    namespace
    {
        bool timeReached(const uint32_t now, const uint32_t deadline)
        {
            return static_cast<int32_t>(now - deadline) >= 0;
        }

        bool isNumber(const JsonVariantConst value)
        {
            return value.is<double>() && !value.is<bool>();
        }

        // Follow "field.sub" through a JSON value; an empty path is the value itself.
        JsonVariantConst lookupPath(JsonVariantConst value, const std::string& path)
        {
            size_t begin = 0;
            while (begin < path.size() && !value.isNull())
            {
                size_t end = path.find('.', begin);
                if (end == std::string::npos)
                {
                    end = path.size();
                }
                value = value[path.substr(begin, end - begin)];
                begin = end + 1;
            }
            return value;
        }
    } // namespace

    // Initiate the process
    bool Engine::start(const Process& process)
    {
        if (status_ == ProcessStatus::Running)
        {
            log("Already running");
            return false;
        }
        // find the start node
        int startNodeIdx = -1;
        for (size_t i = 0; i < process.nodes.size() && startNodeIdx < 0; i++)
        {
            if (process.nodes[i].type == NodeType::Start)
            {
                startNodeIdx = static_cast<int>(i);
            }
        }
        if (startNodeIdx < 0)
        {
            log("Process has no start event");
            return false;
        }

        // Forget everything from the previous execution.
        process_ = &process;
        runs_.assign(process.nodes.size(), 0);
        processData_.clear();
        status_ = ProcessStatus::Running;

        log("Execution of '" + process.id + "' started");
        if (processObserver_)
        {
            processObserver_->processStarted(process);
        }

        activateNode(startNodeIdx);
        return true;
    }


    // Stop the process (called from the coordinator)
    bool Engine::stop()
    {
        if (status_ != ProcessStatus::Running)
        {
            return false;
        }
        if (!activeNodeTaskId_.empty())
        {
            taskDispatcher_.cancelTask(activeNodeTaskId_);
        }
        finish(ProcessStatus::Stopped, "Execution stopped");
        return true;
    }

    // Called from the coordinator main loop
    void Engine::tick(const uint32_t now)
    {
        // The chain of instant nodes execution cannot block the main loop()
        for (int i = 0; i < STEPS_PER_TICK && status_ == ProcessStatus::Running; i++)
        {
            if (!executeActiveNode(now))
            {
                break;
            }
        }
    }

    // Callback for task result. Worker answer with 'taskId' and 'resultData'
    void Engine::onTaskResult(const std::string& taskId, const bool ok, const std::string& resultData)
    {
        if (status_ != ProcessStatus::Running || activeNodeTaskId_.empty() || taskId != activeNodeTaskId_)
        {
            log("   ignored result of " + taskId);
            return;
        }
        if (!ok)
        {
            fail("task_failed " + resultData);
            return;
        }
        JsonDocument check;
        if (deserializeJson(check, resultData))
        {
            fail("bad_result " + resultData); // conditions would never match it
            return;
        }
        log("   result " + resultData);
        {
            const Node& node = process_->nodes[activeNodeIdx_];
            if (processObserver_)
            {
                processObserver_->taskCompleted(node, resultData);
            }
            if (!node.resultVariable.empty())
            {
                setVariable(node.resultVariable, resultData);
            }
            moveOn();
        }
    }

    // Make the node ready to start.
    void Engine::activateNode(const int idx)
    {
        activeNodeIdx_ = idx;
        activeNodePhase_ = Phase::Ready;
        activeNodeTaskId_.clear();
        activeNodeQueued_ = false;
        const Node& node = process_->nodes[idx];
        const uint32_t runs = ++runs_[idx];
        log("-> " + node.id +
            (runs > 1 ? " (run " + std::to_string(runs) + ")" : ""));
        if (processObserver_)
        {
            processObserver_->nodeActivated(node, runs);
        }
        if (runs > MAX_NODE_RUNS)
        {
            fail("too_many_runs");
        }
    }

    // Advance the active node by one phase. Returns true if anything changed.
    bool Engine::executeActiveNode(const uint32_t now)
    {
        const Node& node = process_->nodes[activeNodeIdx_];

        if (activeNodePhase_ == Phase::Waiting)
        {
            // if deadline not reached return execution to the main loop
            if (!timeReached(now, activeNodeDeadline_))
            {
                return false;
            }
            if (node.type == NodeType::ExternalTask)
            {
                fail("timeout");
            }
            else
            {
                moveOn(); // the delay is over, so move process to the next node
            }
            return true;
        }

        // Phase::Ready: begin the node's work.
        switch (node.type)
        {
        case NodeType::Start:
            moveOn();
            break;
        case NodeType::Stop:
            finish(ProcessStatus::Completed, "Execution completed");
            break;
        case NodeType::Router:
            {
                const int to = chooseRoute(node);
                if (to < 0)
                {
                    fail("no_route");
                }
                else
                {
                    moveOn(to);
                }
                break;
            }
        case NodeType::InternalTask:
            runInternalTask(node, now);
            break;
        case NodeType::ExternalTask:
            switch (taskDispatcher_.sendTask(node, activeNodeTaskId_))
            {
            case DispatchResult::Sent:
                wait(now + node.timeoutMs);
                break;
            case DispatchResult::NoWorker:
                fail("no_worker");
                break;
            case DispatchResult::Busy:
                // Stay Ready and try again on a later tick. The task's timeout is how
                // long it may wait for an idle worker.
                if (!activeNodeQueued_)
                {
                    activeNodeQueued_ = true;
                    activeNodeDeadline_ = now + node.timeoutMs;
                }
                else if (timeReached(now, activeNodeDeadline_))
                {
                    fail("dispatch_timeout");
                }
                return false;
            }
            break;
        default:
            fail("unsupported_node");
        }
        return true;
    }

    void Engine::runInternalTask(const Node& node, const uint32_t now)
    {
        switch (node.internalTaskType)
        {
        case InternalTaskType::Delay:
            wait(now + node.delayMs);
            break;
        case InternalTaskType::Increment:
            if (incrementVariable(node))
            {
                moveOn();
            }
            else
            {
                fail("not_a_number");
            }
            break;
        case InternalTaskType::SetVariable:
            setVariable(node.variable, node.valueJson);
            moveOn();
            break;
        default:
            fail("unknown_handler");
        }
    }

    // Check the routes in order:
    // the first whose condition holds wins;
    // if none does, the first one without a condition.
    // -1 if none.
    int Engine::chooseRoute(const Node& router) const
    {
        int otherwise = -1;
        // loop through all the routes
        for (const auto& [to, hasCondition, condition] : router.routes)
        {
            if (!hasCondition)
            {
                if (otherwise < 0)
                {
                    otherwise = to;
                }
            }
            else if (conditionHolds(condition))
            {
                return to;
            }
        }
        return otherwise;
    }

    // Check condition
    bool Engine::conditionHolds(const Condition& condition) const
    {
        // "temperature.temperature_c": variable "temperature", path "temperature_c"
        const size_t dot = condition.variable.find('.');
        const auto variable = processData_.find(condition.variable.substr(0, dot));
        if (variable == processData_.end())
        {
            return false;
        }
        JsonDocument variableDoc, expectedDoc;
        if (deserializeJson(variableDoc, variable->second))
        {
            log("   bad JSON in variable " + condition.variable + ": " + variable->second);
        }
        deserializeJson(expectedDoc, condition.value);
        const JsonVariantConst actual = lookupPath(
            variableDoc.as<JsonVariantConst>(),
            dot == std::string::npos ? "" : condition.variable.substr(dot + 1));
        const auto expected = expectedDoc.as<JsonVariantConst>();
        if (actual.isNull())
        {
            return false;
        }

        if (isNumber(actual) && isNumber(expected))
        {
            const auto a = actual.as<double>();
            const auto b = expected.as<double>();
            switch (condition.op)
            {
            case Compare::Equal:
                return a == b;
            case Compare::NotEqual:
                return a != b;
            case Compare::Less:
                return a < b;
            case Compare::LessOrEqual:
                return a <= b;
            case Compare::Greater:
                return a > b;
            case Compare::GreaterOrEqual:
                return a >= b;
            }
        }
        // Anything else (text, true/false, objects) can only be tested for equality.
        std::string a, b;
        serializeJson(actual, a);
        serializeJson(expected, b);
        if (condition.op == Compare::Equal)
        {
            return a == b;
        }
        return condition.op == Compare::NotEqual && a != b;
    }

    // Add node.increment to a number variable; a missing one starts at 0.
    bool Engine::incrementVariable(const Node& node)
    {
        double value = 0;
        const auto variable = processData_.find(node.variable);
        if (variable != processData_.end())
        {
            JsonDocument doc;
            deserializeJson(doc, variable->second);
            if (!isNumber(doc.as<JsonVariantConst>()))
            {
                return false;
            }
            value = doc.as<double>();
        }
        JsonDocument result;
        result.set(value + node.increment);
        std::string text;
        serializeJson(result, text);
        setVariable(node.variable, text);
        return true;
    }

    // Set variable in aggregate process data
    void Engine::setVariable(const std::string& name, const std::string& valueJson)
    {
        processData_[name] = valueJson;
        log("   " + name + " = " + valueJson);
        if (processObserver_)
        {
            processObserver_->variableSet(name, valueJson);
        }
    }

    // The active node is done: go and activate next node
    void Engine::moveOn(int to)
    {
        const Node& node = process_->nodes[activeNodeIdx_];
        if (to < 0)
        {
            if (node.routes.empty())
            {
                fail("dead_end");
                return;
            }
            to = node.routes[0].to; // get default next node
        }
        activateNode(to);
    }

    // set active node deadline
    void Engine::wait(const uint32_t deadline)
    {
        activeNodePhase_ = Phase::Waiting;
        activeNodeDeadline_ = deadline;
    }

    // finish the process
    void Engine::finish(const ProcessStatus status, const std::string& message)
    {
        status_ = status;
        activeNodeTaskId_.clear(); // so a late result is ignored
        log(message);
        if (processObserver_)
        {
            processObserver_->processFinished(status, message);
        }
    }

    // node execution fail with error
    void Engine::fail(const std::string& error)
    {
        finish(ProcessStatus::Failed, "Execution failed at '" +
               process_->nodes[activeNodeIdx_].id +
               "': " + error);
    }

    // Engine's own log line; goes to the observer if there is one
    void Engine::log(const std::string& message) const
    {
        if (processObserver_)
        {
            processObserver_->log(message);
        }
    }
} // namespace core
