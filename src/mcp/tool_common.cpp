#include "tool_common.hpp"

#include <cstdio>

namespace gptimage {

using nlohmann::json;

std::string saved_caption(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return {};
    std::string out = paths.size() == 1 ? " Saved to " : " Saved to: ";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) out += ", ";
        out += paths[i].string();
    }
    out += ".";
    return out;
}

std::string usage_caption(const ImageUsage& usage, const ImageConfig& cfg) {
    if (usage.total_tokens < 0 && usage.output_tokens < 0) return {};

    std::string out = " Tokens: ";
    out += std::to_string(usage.total_tokens >= 0 ? usage.total_tokens
                                                  : usage.output_tokens);
    if (usage.output_tokens >= 0) {
        out += " (" + std::to_string(usage.output_tokens) + " out";
        if (usage.input_image_tokens > 0) {
            out += ", " + std::to_string(usage.input_image_tokens) + " image in";
        }
        out += ")";
    }
    out += ".";

    bool exact = false;
    const double cost = usage_cost_usd(usage, cfg, &exact);
    if (cost >= 0.0) {
        // Four decimals: a low-quality render is well under a cent, and rounding
        // it to "$0.01" would make the cheap path look ten times its price.
        char buf[64];
        std::snprintf(buf, sizeof(buf), " Cost: %s$%.4f.", exact ? "" : "~", cost);
        out += buf;
    }
    return out;
}

json text_result(const std::string& text, bool is_error) {
    json out{
        {"content", json::array({
            json::object({{"type", "text"}, {"text", text}}),
        })},
    };
    if (is_error) out["isError"] = true;
    return out;
}

json json_result(const json& data) {
    return {
        {"content", json::array({
            json::object({{"type", "text"}, {"text", data.dump(2)}}),
        })},
    };
}

json image_result(const std::vector<GeneratedImage>& images, const std::string& caption) {
    json content = json::array();
    for (const auto& im : images) {
        content.push_back(json::object({
            {"type", "image"},
            {"data", im.b64},
            {"mimeType", im.mime},
        }));
    }
    if (!caption.empty()) {
        content.push_back(json::object({{"type", "text"}, {"text", caption}}));
    }
    return {{"content", content}};
}

namespace {

// File extension advertised in a hosted image URL. Cosmetic (the route resolves
// by job id and serves with the stored mime), but keeps the link honest.
std::string ext_for_mime(const std::string& m) {
    if (m == "image/webp") return "webp";
    if (m == "image/jpeg") return "jpeg";
    return "png";
}

}  // namespace

json render_job(const std::optional<ImageJob>& snap, const std::string& id,
                const std::string& public_base_url) {
    if (!snap) {
        return text_result(
            "No image job with id '" + id + "' — it was already delivered and "
            "released, or it expired, or the server restarted. Start a new "
            "generation.", true);
    }
    switch (snap->status) {
        case ImageJob::Status::Done: {
            // Local/stdio (nothing hosted): the inline base64 block is all there is.
            if (public_base_url.empty() || snap->images.empty()) {
                return image_result(snap->images, snap->caption);
            }
            // Hosted: keep the (compact) inline block as a fallback, and add a text
            // block whose markdown link makes the picture render in the
            // conversation body. The caption rides along in that same block so the
            // model does not see a bare image with no context.
            json res = image_result(snap->images, /*caption=*/"");
            std::string text =
                "Show this image to the user by including the following markdown "
                "in your reply verbatim:\n\n";
            for (size_t i = 0; i < snap->images.size(); ++i) {
                text += "![generated image](" + public_base_url + "/i/" + id + "-" +
                        std::to_string(i) + "." + ext_for_mime(snap->images[i].mime) + ")\n";
            }
            if (!snap->caption.empty()) text += "\n" + snap->caption;
            res["content"].push_back(json::object({{"type", "text"}, {"text", text}}));
            return res;
        }
        case ImageJob::Status::Error:
            return text_result("image job failed: " + snap->error, true);
        case ImageJob::Status::Pending:
        default:
            return json_result({
                {"status", "pending"},
                {"job_id", id},
                {"instructions",
                    "The image is still rendering (high quality can take 1-3 minutes). "
                    "Call the gptimage_result tool again with this exact job_id to fetch "
                    "it; keep calling until it returns the image."},
            });
    }
}

}  // namespace gptimage
