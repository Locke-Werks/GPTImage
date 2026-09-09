#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <gptimage/config.hpp>

#include "../../src/mcp/tools.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using nlohmann::json;

namespace {

std::vector<std::string> tool_names(const json& schemas) {
    std::vector<std::string> out;
    for (const auto& t : schemas) out.push_back(t.at("name").get<std::string>());
    return out;
}

const json& tool_named(const json& schemas, const std::string& name) {
    for (const auto& t : schemas) {
        if (t.at("name") == name) return t;
    }
    FAIL("no tool named ", name);
    return schemas;  // unreachable
}

bool has(const json& arr, const std::string& v) {
    for (const auto& e : arr) {
        if (e == v) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("both models get their own generate and edit tools") {
    gptimage::ImageConfig ic;
    const json schemas = gptimage::mcp_tool_schemas(ic);
    const auto names = tool_names(schemas);

    REQUIRE(names.size() == 5);
    CHECK(names[0] == "gptimage_generate_flare");
    CHECK(names[1] == "gptimage_generate_sunburst");
    CHECK(names[2] == "gptimage_edit_flare");
    CHECK(names[3] == "gptimage_edit_sunburst");
    CHECK(names[4] == "gptimage_result");
}

TEST_CASE("the two model variants of a tool differ only in prose") {
    gptimage::ImageConfig ic;
    const json schemas = gptimage::mcp_tool_schemas(ic);

    // The point of building both from one template: the argument surface cannot
    // drift apart, so a caller can swap models without rewriting its call.
    CHECK(tool_named(schemas, "gptimage_generate_flare")["inputSchema"] ==
          tool_named(schemas, "gptimage_generate_sunburst")["inputSchema"]);
    CHECK(tool_named(schemas, "gptimage_edit_flare")["inputSchema"] ==
          tool_named(schemas, "gptimage_edit_sunburst")["inputSchema"]);
    CHECK(tool_named(schemas, "gptimage_generate_flare")["description"] !=
          tool_named(schemas, "gptimage_generate_sunburst")["description"]);
}

TEST_CASE("only gptimage_result is read-only") {
    gptimage::ImageConfig ic;
    const json schemas = gptimage::mcp_tool_schemas(ic);
    for (const auto& t : schemas) {
        const bool ro = t["annotations"].value("readOnlyHint", false);
        CHECK(ro == (t["name"] == "gptimage_result"));
    }
}

TEST_CASE("advertised quality stops at the configured ceiling") {
    SUBCASE("default ceiling hides max") {
        gptimage::ImageConfig ic;  // max_quality = "xhigh"
        const json schemas = gptimage::mcp_tool_schemas(ic);
        const json& q =
            tool_named(schemas, "gptimage_generate_flare")["inputSchema"]["properties"]["quality"]["enum"];
        CHECK(has(q, "auto"));
        CHECK(has(q, "low"));
        CHECK(has(q, "xhigh"));
        // Advertising a tier the server would clamp away would make the model
        // ask for something it can never get.
        CHECK_FALSE(has(q, "max"));
    }
    SUBCASE("a raised ceiling exposes max") {
        gptimage::ImageConfig ic;
        ic.max_quality = "max";
        const json schemas = gptimage::mcp_tool_schemas(ic);
        const json& q =
            tool_named(schemas, "gptimage_edit_sunburst")["inputSchema"]["properties"]["quality"]["enum"];
        CHECK(has(q, "max"));
    }
    SUBCASE("a lowered ceiling hides everything above it") {
        gptimage::ImageConfig ic;
        ic.max_quality = "medium";
        const json schemas = gptimage::mcp_tool_schemas(ic);
        const json& q =
            tool_named(schemas, "gptimage_generate_sunburst")["inputSchema"]["properties"]["quality"]["enum"];
        CHECK(has(q, "medium"));
        CHECK_FALSE(has(q, "high"));
        CHECK_FALSE(has(q, "xhigh"));
    }
}

TEST_CASE("the cost caps reach the advertised schema") {
    gptimage::ImageConfig ic;
    ic.max_n = 2;
    ic.max_input_images = 3;
    const json schemas = gptimage::mcp_tool_schemas(ic);

    const json& gen = tool_named(schemas, "gptimage_generate_flare")["inputSchema"]["properties"];
    CHECK(gen["n"]["maximum"] == 2);

    const json& edit = tool_named(schemas, "gptimage_edit_flare")["inputSchema"]["properties"];
    CHECK(edit["images"]["maxItems"] == 3);
    CHECK(edit["images"]["minItems"] == 1);
}

TEST_CASE("required arguments are what the endpoints actually need") {
    gptimage::ImageConfig ic;
    const json schemas = gptimage::mcp_tool_schemas(ic);

    CHECK(tool_named(schemas, "gptimage_generate_flare")["inputSchema"]["required"] ==
          json::array({"prompt"}));
    CHECK(tool_named(schemas, "gptimage_edit_sunburst")["inputSchema"]["required"] ==
          json::array({"images", "prompt"}));
    CHECK(tool_named(schemas, "gptimage_result")["inputSchema"]["required"] ==
          json::array({"job_id"}));
}
