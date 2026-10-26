#include "process.h"
#include "process_data.h"
#include <cstdlib>
#include <iostream>

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n";   \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

// Wraps `nodes` and `routes` (JSON array bodies) into a process document.
static std::string doc(const std::string& nodes, const std::string& routes)
{
    return R"({"process":{"id":"t","nodes":[)" + nodes + R"(],"routes":[)" +
        routes + "]}}";
}

// Parses `json`; returns true on success, otherwise `error` is set.
static bool parse(const std::string& json, core::Process& p,
                  std::string& error)
{
    return core::parseProcess(json.c_str(), p, error);
}

static bool contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

static void testEmbeddedProcess()
{
    core::Process p;
    std::string error;
    CHECK(parse(PROCESS_JSON, p, error));
    CHECK(p.id == "temperature-monitor");
    CHECK(p.nodes.size() == 6);

    // the loop: router -> temperature while iteration < 3, otherwise -> stop
    const core::Node* router = nullptr;
    for (const auto& n : p.nodes)
        if (n.id == "router")
            router = &n;
    CHECK(router && router->type == core::NodeType::Router);
    CHECK(router && router->routes.size() == 2);
    if (router && router->routes.size() == 2)
    {
        const auto& loop = router->routes[0];
        CHECK(loop.hasCondition);
        CHECK(loop.condition.variable == "iteration");
        CHECK(loop.condition.op == core::Compare::Less);
        CHECK(loop.condition.value == "3");
        CHECK(p.nodes[loop.to].id == "temperature");
        CHECK(!router->routes[1].hasCondition);
        CHECK(p.nodes[router->routes[1].to].id == "stop");
    }
}

static void testInvalidJson()
{
    core::Process p;
    std::string error;
    CHECK(!parse("{not json", p, error));
    CHECK(contains(error, "invalid JSON"));
    CHECK(!parse("{}", p, error));
    CHECK(contains(error, "process"));
}

static void testDuplicateId()
{
    core::Process p;
    std::string error;
    CHECK(!parse(doc(R"({"id":"a","type":"Start"},{"id":"a","type":"Stop"})", ""),
        p, error));
    CHECK(contains(error, "duplicate"));
}

static void testUnknownNode()
{
    core::Process p;
    std::string error;
    CHECK(!parse(doc(R"({"id":"a","type":"Bogus"})", ""), p, error));
    CHECK(contains(error, "unknown type"));
    CHECK(!parse(
        doc(R"({"id":"a","type":"Start"})", R"({"from":"a","to":"missing"})"), p,
        error));
    CHECK(contains(error, "unknown node"));
}

static void testInternalTasks()
{
    core::Process p;
    std::string error;
    const std::string start = R"({"id":"s","type":"Start"},)";
    CHECK(
        !parse(doc(start + R"({"id":"x","type":"InternalTask"})", ""), p, error));
    CHECK(!parse(doc(start + R"({"id":"x","type":"InternalTask",
        "internalTaskType":"Increment"})",
            ""),
        p, error)); // no variable

    CHECK(parse(doc(start + R"({"id":"x","type":"InternalTask",
        "internalTaskType":"Increment","variable":"n"})",
            ""),
        p, error));
    CHECK(p.nodes[1].increment == 1); // default
    CHECK(p.nodes[1].variable == "n");

    CHECK(parse(doc(R"({"id":"d","type":"InternalTask",
        "internalTaskType":"Delay","delayMs":250})",
            ""),
        p, error));
    CHECK(p.nodes[0].delayMs == 250);
}

static void testConditions()
{
    core::Process p;
    std::string error;
    const std::string nodes =
        R"({"id":"r","type":"Router"},{"id":"e","type":"Stop"})";
    auto route = [](const char* op, const char* value)
    {
        return std::string(R"({"from":"r","to":"e","condition":{"variable":"v",)") +
            R"("op":")" + op + R"(","value":)" + value + "}}";
    };

    CHECK(parse(doc(nodes, route(">=", "5")), p, error));
    CHECK(p.nodes[0].routes[0].condition.op == core::Compare::GreaterOrEqual);
    CHECK(p.nodes[0].routes[0].condition.value == "5");

    CHECK(parse(doc(nodes, route("!=", R"("abc")")), p, error));
    CHECK(p.nodes[0].routes[0].condition.value == R"("abc")");

    CHECK(parse(doc(nodes, route("==", "true")), p, error));
    CHECK(p.nodes[0].routes[0].condition.value == "true");

    CHECK(!parse(doc(nodes, route("=>", "1")), p, error));
    CHECK(contains(error, "invalid compare op"));
}

static void testExternalTaskFields()
{
    core::Process p;
    std::string error;
    CHECK(parse(doc(R"({"id":"x","type":"ExternalTask","externalTaskType":"temperature",
        "resultVariable":"t","timeoutMs":1234,"context":{"unit":"c"}})", ""),
        p, error));
    const core::Node& node = p.nodes[0];
    CHECK(node.externalTaskType == "temperature");
    CHECK(node.resultVariable == "t");
    CHECK(node.timeoutMs == 1234);
    CHECK(node.context == R"({"unit":"c"})");
}

static void testInternalTaskErrors()
{
    core::Process p;
    std::string error;
    const std::string start = R"({"id":"s","type":"Start"},)";
    CHECK(!parse(doc(start + R"({"id":"x","type":"InternalTask",
        "internalTaskType":"Bogus"})", ""), p, error));
    CHECK(contains(error, "is invalid"));
    CHECK(!parse(doc(start + R"({"id":"x","type":"InternalTask",
        "internalTaskType":"SetVariable"})", ""), p, error));
    CHECK(contains(error, "is invalid"));
}

static void testConditionDefaults()
{
    core::Process p;
    std::string error;
    const std::string nodes =
        R"({"id":"r","type":"Router"},{"id":"e","type":"Stop"})";
    // without "value" the condition compares with null
    CHECK(parse(doc(nodes,
        R"({"from":"r","to":"e","condition":{"variable":"v","op":"=="}})"), p, error));
    CHECK(p.nodes[0].routes[0].condition.value == "null");
    // without "op" there is no valid compare op
    CHECK(!parse(doc(nodes,
        R"({"from":"r","to":"e","condition":{"variable":"v","value":1}})"), p, error));
    CHECK(contains(error, "invalid compare op"));
}

int main()
{
    testEmbeddedProcess();
    testInvalidJson();
    testDuplicateId();
    testUnknownNode();
    testInternalTasks();
    testConditions();
    testExternalTaskFields();
    testInternalTaskErrors();
    testConditionDefaults();
    if (failures)
    {
        std::cout << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
}
