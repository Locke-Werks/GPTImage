#include "tool_common.hpp"

#include <gptimage/image_client.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <utility>

namespace gptimage {

using nlohmann::json;

json tool_generate(const json& args, ToolContext& ctx, const std::string& model) {
    const ImageConfig& ic = ctx.cfg.image;
    if (ic.api_key.empty()) {
        return text_result(
            "image generation is unavailable: the server has no OPENAI_API_KEY set", true);
    }

    const std::string prompt = args.value("prompt", std::string());
    if (prompt.empty()) {
        return text_result("prompt is required", true);
    }

    ImageRequest req;
    req.prompt      = prompt;
    req.model       = model;
    req.size        = args.value("size",       ic.default_size);
    req.quality     = args.value("quality",    ic.default_quality);
    req.background  = args.value("background",  ic.default_background);
    req.format      = args.value("format",     ic.default_format);
    req.n           = args.value("n",           1);
    req.compression = args.value("compression", ic.default_compression);
    if (req.n < 1) req.n = 1;
    if (req.n > ic.max_n) req.n = ic.max_n;

    // Clamp rather than reject: a caller asking for "max" under an "xhigh"
    // ceiling wants the best available, and a picture at the cap serves that
    // better than an error does. The caption says which tier actually ran.
    req.quality = clamp_quality(req.quality, ic.max_quality);

    // Start the render on a background thread. The lambda is fully self-contained
    // (owns copies of the config and request), so it outlives this call safely.
    const std::string id = ctx.jobs.submit(
        "generate", ctx.grant.principal,
        [ic, req](const std::string& job_id) -> JobOutput {
            ImageClient client(ic);
            ImageResponse resp = client.generate(req);
            std::string caption = "Generated " + std::to_string(resp.images.size()) +
                (resp.images.size() == 1 ? " image" : " images") +
                " with " + req.model + " (" + req.quality + ", " + req.size + ").";
            caption += usage_caption(resp.usage, ic);
            caption += saved_caption(
                save_render(ic.save_dir, job_id, resp.images, ic.save_max_files));
            return JobOutput{std::move(resp.images), std::move(caption)};
        });

    spdlog::info("gptimage_generate job={} principal={} model={} quality={} size={}",
                 id, ctx.grant.principal, model, req.quality, req.size);

    // Give a fast render (low quality / small size) the chance to land inside
    // this one call; otherwise the caller polls gptimage_result with the id.
    auto snap = ctx.jobs.wait_for(id, std::chrono::seconds(ic.job_poll_seconds));
    return render_job(snap, id, ic.public_base_url);
}

}  // namespace gptimage
