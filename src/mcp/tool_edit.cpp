#include "tool_common.hpp"

#include <gptimage/image_client.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gptimage {

using nlohmann::json;

json tool_edit(const json& args, ToolContext& ctx, const std::string& model) {
    const ImageConfig& ic = ctx.cfg.image;
    if (ic.api_key.empty()) {
        return text_result(
            "image editing is unavailable: the server has no OPENAI_API_KEY set", true);
    }

    if (!args.contains("images") || !args["images"].is_array() || args["images"].empty()) {
        return text_result("images (a non-empty array of base64 strings) is required", true);
    }
    if (static_cast<int>(args["images"].size()) > ic.max_input_images) {
        return text_result(
            "too many input images: " + std::to_string(args["images"].size()) +
            " given, the edits endpoint accepts at most " +
            std::to_string(ic.max_input_images) + ". Drop the least important "
            "references and try again.", true);
    }
    const std::string prompt = args.value("prompt", std::string());
    if (prompt.empty()) {
        return text_result("prompt is required", true);
    }

    std::vector<std::vector<unsigned char>> inputs;
    inputs.reserve(args["images"].size());
    for (const auto& v : args["images"]) {
        if (!v.is_string()) {
            return text_result("each entry in images must be a base64 string", true);
        }
        auto bytes = base64_decode(v.get<std::string>());
        if (bytes.empty()) {
            return text_result("an input image was empty or not valid base64", true);
        }
        inputs.push_back(std::move(bytes));
    }

    std::optional<std::vector<unsigned char>> mask;
    if (args.contains("mask") && args["mask"].is_string() &&
        !args["mask"].get<std::string>().empty()) {
        auto mb = base64_decode(args["mask"].get<std::string>());
        if (mb.empty()) {
            return text_result("mask was not valid base64", true);
        }
        mask = std::move(mb);
    }

    ImageRequest req;
    req.prompt  = prompt;
    req.model   = model;
    req.size    = args.value("size",    ic.default_size);
    req.quality = args.value("quality", ic.default_quality);
    req.background = args.value("background", ic.default_background);
    req.format  = args.value("format",  ic.default_format);
    req.n       = args.value("n",        1);
    req.compression = args.value("compression", ic.default_compression);
    if (req.n < 1) req.n = 1;
    if (req.n > ic.max_n) req.n = ic.max_n;
    req.quality = clamp_quality(req.quality, ic.max_quality);

    const size_t input_count = inputs.size();
    const std::string id = ctx.jobs.submit(
        "edit", ctx.grant.principal,
        [ic, req, inputs = std::move(inputs), mask = std::move(mask)]
        (const std::string& job_id) -> JobOutput {
            ImageClient client(ic);
            ImageResponse resp = client.edit(req, inputs, mask);
            std::string caption = "Edited into " + std::to_string(resp.images.size()) +
                (resp.images.size() == 1 ? " image" : " images") +
                " with " + req.model + " (" + req.quality + ").";
            caption += usage_caption(resp.usage, ic);
            caption += saved_caption(
                save_render(ic.save_dir, job_id, resp.images, ic.save_max_files));
            return JobOutput{std::move(resp.images), std::move(caption)};
        });

    spdlog::info("gptimage_edit job={} principal={} model={} inputs={} quality={}",
                 id, ctx.grant.principal, model, input_count, req.quality);

    auto snap = ctx.jobs.wait_for(id, std::chrono::seconds(ic.job_poll_seconds));
    return render_job(snap, id, ic.public_base_url);
}

}  // namespace gptimage
