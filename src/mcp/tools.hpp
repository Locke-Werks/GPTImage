#pragma once

#include <nlohmann/json.hpp>

#include <gptimage/config.hpp>

#include <string>

namespace gptimage {

struct ToolContext;

// Schemas for tools/list. Each entry is {name, description, inputSchema,
// annotations}. annotations carries the MCP tool hints (title, readOnlyHint,
// destructiveHint, idempotentHint, openWorldHint) — read-only tools set
// readOnlyHint:true so write-gating clients (e.g. ChatGPT dev mode) don't
// gate them as writes.
//
// Takes the image config because the advertised enums are built from it: the
// quality list stops at max_quality and n stops at max_n, so a client is never
// shown a value the server would clamp behind its back.
nlohmann::json mcp_tool_schemas(const ImageConfig& ic);

// Handle a tools/call. Returns the MCP tool-result shape:
//   { "content": [ {"type": "text", "text": "..."}, ... ], "isError": bool? }
nlohmann::json mcp_tool_call(const std::string& name,
                             const nlohmann::json& arguments,
                             ToolContext& ctx);

}  // namespace gptimage
