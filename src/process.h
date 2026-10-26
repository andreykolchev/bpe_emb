#pragma once

#include "config.h"
#include <string>
#include <vector>

namespace core
{
    enum class NodeType
    {
        Unknown,
        Start, // where an execution begins
        Stop, // where an execution ends
        ExternalTask, // runs on a worker
        InternalTask, // runs on the coordinator
        Router // picks one of its outgoing routes
    };

    // What an internal task does.
    enum class InternalTaskType { Unknown, Delay, Increment, SetVariable };

    // Enum for compare operations
    enum class Compare
    {
        Equal,
        NotEqual,
        Less,
        LessOrEqual,
        Greater,
        GreaterOrEqual
    };

    // A test on a variable, written on a route that leaves a Router.
    struct Condition
    {
        std::string variable;
        Compare op = Compare::Equal;
        std::string value = "null"; // JSON: 5, "text", true, ...
    };

    // Route
    struct Route
    {
        int to = -1; // index of next Node in Process::nodes
        bool hasCondition = false;
        Condition condition;
    };

    // One node of the process.
    struct Node
    {
        std::string id;
        NodeType type = NodeType::Unknown;

        // Routes leaving this node, in the order written.
        std::vector<Route> routes;

        // ExternalTask
        std::string externalTaskType;
        std::string context = "{}";
        uint32_t timeoutMs = DEFAULT_TASK_TIMEOUT_MS;
        std::string resultVariable; // store the worker's result here

        // InternalTask
        InternalTaskType internalTaskType = InternalTaskType::Unknown;
        uint32_t delayMs = DEFAULT_TASK_TIMEOUT_MS; // Delay ms
        int32_t increment = 1; // Increment by value
        std::string variable; // Process data variable name
        std::string valueJson; // Default value to add to process aggregated data
    };

    // Process graph
    struct Process
    {
        std::string id;
        std::vector<Node> nodes;
    };

    // Parse `json` into a `process`.
    // Returns false, with `error` set, if not valid JSON.
    bool parseProcess(const char* json, Process& process, std::string& error);
} // namespace core
