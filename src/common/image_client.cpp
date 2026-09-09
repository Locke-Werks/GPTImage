#include <gptimage/image_client.hpp>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace gptimage {

using nlohmann::json;

namespace {

std::string mime_for_format(const std::string& f) {
    if (f == "jpeg" || f == "jpg") return "image/jpeg";
    if (f == "webp")               return "image/webp";
    return "image/png";
}

// Pull a human-readable message out of an API error body, falling back to a
// truncated raw body so moderation/validation reasons still reach the caller.
//
// A moderation block carries an optional moderation_details object naming the
// stage (the prompt or the finished image) and coarse categories. Both go into
// the message: the model on the other end of this tool result is the one that
// has to decide whether to rewrite the prompt or give up, and "blocked" alone
// does not tell it which.
std::string error_message(const cpr::Response& r) {
    try {
        auto j = json::parse(r.text);
        if (j.contains("error") && j["error"].is_object()) {
            const auto& e = j["error"];
            if (e.contains("message") && e["message"].is_string()) {
                std::string msg = e["message"].get<std::string>();
                if (e.contains("code") && e["code"].is_string()) {
                    msg += " [" + e["code"].get<std::string>() + "]";
                }
                if (e.contains("moderation_details") && e["moderation_details"].is_object()) {
                    const auto& d = e["moderation_details"];
                    std::string detail;
                    if (d.contains("moderation_stage") && d["moderation_stage"].is_string()) {
                        detail = "blocked at the " +
                                 d["moderation_stage"].get<std::string>() + " stage";
                    }
                    if (d.contains("categories") && d["categories"].is_array() &&
                        !d["categories"].empty()) {
                        std::string cats;
                        for (const auto& c : d["categories"]) {
                            if (!c.is_string()) continue;
                            if (!cats.empty()) cats += ", ";
                            cats += c.get<std::string>();
                        }
                        if (!cats.empty()) {
                            detail += (detail.empty() ? "categories: " : "; categories: ") + cats;
                        }
                    }
                    if (!detail.empty()) msg += " (" + detail + ")";
                }
                return msg;
            }
        }
    } catch (...) {
        // fall through to the raw body
    }
    std::string body = r.text.substr(0, 300);
    return "status " + std::to_string(r.status_code) +
           (body.empty() ? "" : (": " + body));
}

ImageResponse parse_response(const cpr::Response& r, const std::string& mime) {
    json j;
    try {
        j = json::parse(r.text);
    } catch (...) {
        throw ImageError("malformed response from image API");
    }
    if (!j.contains("data") || !j["data"].is_array()) {
        throw ImageError("image API response missing data[]");
    }
    ImageResponse out;
    for (const auto& item : j["data"]) {
        if (item.contains("b64_json") && item["b64_json"].is_string()) {
            GeneratedImage g;
            g.b64  = item["b64_json"].get<std::string>();
            g.mime = mime;
            out.images.push_back(std::move(g));
        }
    }
    if (out.images.empty()) {
        throw ImageError("image API returned no image data");
    }
    if (j.contains("usage") && j["usage"].is_object()) {
        const auto& u = j["usage"];
        out.usage.input_tokens  = u.value("input_tokens",  static_cast<long>(-1));
        out.usage.output_tokens = u.value("output_tokens", static_cast<long>(-1));
        out.usage.total_tokens  = u.value("total_tokens",  static_cast<long>(-1));
        if (u.contains("input_tokens_details") && u["input_tokens_details"].is_object()) {
            const auto& d = u["input_tokens_details"];
            out.usage.input_text_tokens  = d.value("text_tokens",  static_cast<long>(-1));
            out.usage.input_image_tokens = d.value("image_tokens", static_cast<long>(-1));
        }
    }
    return out;
}

}  // namespace

int quality_rank(const std::string& q) {
    if (q == "auto")   return 0;
    if (q == "low")    return 1;
    if (q == "medium") return 2;
    if (q == "high")   return 3;
    if (q == "xhigh")  return 4;
    if (q == "max")    return 5;
    return -1;
}

std::string clamp_quality(const std::string& requested, const std::string& ceiling) {
    const int want = quality_rank(requested);
    const int cap  = quality_rank(ceiling);
    if (want <= 0 || cap <= 0) return requested;  // auto/unknown either side
    return want > cap ? ceiling : requested;
}

double usage_cost_usd(const ImageUsage& u, const ImageConfig& cfg, bool* exact) {
    if (u.input_tokens < 0 && u.output_tokens < 0) {
        if (exact) *exact = false;
        return -1.0;
    }
    const double out_cost =
        (u.output_tokens > 0 ? u.output_tokens : 0) * cfg.price_image_output_per_m / 1e6;

    // The detail breakdown is what separates text input from image input, which
    // bill at different rates. Without it, charging the lot at the text rate is
    // the closest honest guess, and it is only wrong for edits.
    if (u.input_text_tokens >= 0 || u.input_image_tokens >= 0) {
        const double text_cost =
            (u.input_text_tokens > 0 ? u.input_text_tokens : 0) * cfg.price_text_input_per_m / 1e6;
        const double img_cost =
            (u.input_image_tokens > 0 ? u.input_image_tokens : 0) * cfg.price_image_input_per_m / 1e6;
        if (exact) *exact = true;
        return text_cost + img_cost + out_cost;
    }
    const double in_cost =
        (u.input_tokens > 0 ? u.input_tokens : 0) * cfg.price_text_input_per_m / 1e6;
    if (exact) *exact = false;
    return in_cost + out_cost;
}

std::vector<unsigned char> base64_decode(const std::string& in) {
    // Accept a data: URI by starting after the "base64," marker if present.
    const auto marker = in.find("base64,");
    const std::string s = (marker != std::string::npos) ? in.substr(marker + 7) : in;

    auto sextet = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (unsigned char c : s) {
        if (c == '=' || std::isspace(c)) continue;
        const int d = sextet(c);
        if (d < 0) return {};  // invalid character => reject whole input
        buf = (buf << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

ImageClient::ImageClient(ImageConfig cfg) : cfg_(std::move(cfg)) {}

ImageResponse ImageClient::generate(const ImageRequest& req) {
    if (cfg_.api_key.empty()) {
        throw ImageError("OPENAI_API_KEY not set (env " + cfg_.api_key_env + ")");
    }

    json body;
    body["model"]  = req.model.empty() ? cfg_.model : req.model;
    body["prompt"] = req.prompt;
    body["n"]      = req.n;
    if (!req.size.empty())       body["size"]          = req.size;
    if (!req.quality.empty())    body["quality"]       = req.quality;
    if (!req.background.empty()) body["background"]     = req.background;
    if (!req.format.empty())     body["output_format"] = req.format;
    if (!cfg_.moderation.empty()) body["moderation"]   = cfg_.moderation;
    if (req.compression >= 0 && (req.format == "jpeg" || req.format == "webp")) {
        body["output_compression"] = req.compression;
    }
    const std::string payload = body.dump();
    const std::string mime = mime_for_format(req.format);

    int delay_ms = cfg_.backoff_initial_ms;
    for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
        cpr::Response r = cpr::Post(
            cpr::Url{cfg_.generations_endpoint},
            cpr::Header{
                {"Authorization", "Bearer " + cfg_.api_key},
                {"Content-Type",  "application/json"},
            },
            cpr::Body{payload},
            cpr::Timeout{cfg_.timeout_s * 1000});

        if (r.status_code == 200) return parse_response(r, mime);

        const bool retryable =
            r.status_code == 0 /* network */ ||
            r.status_code == 429 ||
            (r.status_code >= 500 && r.status_code < 600);
        if (!retryable || attempt == cfg_.max_retries) {
            throw ImageError(error_message(r));  // includes moderation/4xx reasons
        }

        int wait_ms = delay_ms;
        if (auto it = r.header.find("Retry-After"); it != r.header.end()) {
            try { wait_ms = std::max(wait_ms, std::stoi(it->second) * 1000); }
            catch (...) { /* malformed header */ }
        }
        spdlog::warn("image.generate: status {}, retrying in {}ms (attempt {}/{})",
                     r.status_code, wait_ms, attempt + 1, cfg_.max_retries);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        delay_ms = std::min(delay_ms * 2, 30000);
    }
    throw ImageError("image.generate: retry loop exited unexpectedly");
}

ImageResponse ImageClient::edit(const ImageRequest& req,
                                const std::vector<std::vector<unsigned char>>& images,
                                const std::optional<std::vector<unsigned char>>& mask) {
    if (cfg_.api_key.empty()) {
        throw ImageError("OPENAI_API_KEY not set (env " + cfg_.api_key_env + ")");
    }
    if (images.empty()) {
        throw ImageError("image edit requires at least one input image");
    }

    // The OpenAI SDK sends a single input under the "image" field and multiple
    // under "image[]"; match that so both shapes are accepted server-side.
    const std::string img_field = images.size() == 1 ? "image" : "image[]";

    cpr::Multipart mp{};
    mp.parts.emplace_back("model", req.model.empty() ? cfg_.model : req.model);
    mp.parts.emplace_back("prompt", req.prompt);
    mp.parts.emplace_back("n", std::to_string(req.n));
    if (!req.size.empty())        mp.parts.emplace_back("size", req.size);
    if (!req.quality.empty())     mp.parts.emplace_back("quality", req.quality);
    if (!req.background.empty())  mp.parts.emplace_back("background", req.background);
    if (!req.format.empty())      mp.parts.emplace_back("output_format", req.format);
    if (req.compression >= 0 && (req.format == "jpeg" || req.format == "webp")) {
        mp.parts.emplace_back("output_compression", std::to_string(req.compression));
    }
    if (!cfg_.moderation.empty()) mp.parts.emplace_back("moderation", cfg_.moderation);
    for (size_t i = 0; i < images.size(); ++i) {
        mp.parts.emplace_back(cpr::Part(
            img_field,
            cpr::Buffer{images[i].begin(), images[i].end(), "image" + std::to_string(i) + ".png"},
            "image/png"));
    }
    if (mask) {
        mp.parts.emplace_back(cpr::Part(
            "mask",
            cpr::Buffer{mask->begin(), mask->end(), "mask.png"},
            "image/png"));
    }

    const std::string mime = mime_for_format(req.format);

    int delay_ms = cfg_.backoff_initial_ms;
    for (int attempt = 0; attempt <= cfg_.max_retries; ++attempt) {
        // No explicit Content-Type: cpr sets multipart/form-data + boundary.
        cpr::Response r = cpr::Post(
            cpr::Url{cfg_.edits_endpoint},
            cpr::Header{{"Authorization", "Bearer " + cfg_.api_key}},
            mp,
            cpr::Timeout{cfg_.timeout_s * 1000});

        if (r.status_code == 200) return parse_response(r, mime);

        const bool retryable =
            r.status_code == 0 ||
            r.status_code == 429 ||
            (r.status_code >= 500 && r.status_code < 600);
        if (!retryable || attempt == cfg_.max_retries) {
            throw ImageError(error_message(r));
        }

        int wait_ms = delay_ms;
        if (auto it = r.header.find("Retry-After"); it != r.header.end()) {
            try { wait_ms = std::max(wait_ms, std::stoi(it->second) * 1000); }
            catch (...) {}
        }
        spdlog::warn("image.edit: status {}, retrying in {}ms (attempt {}/{})",
                     r.status_code, wait_ms, attempt + 1, cfg_.max_retries);
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        delay_ms = std::min(delay_ms * 2, 30000);
    }
    throw ImageError("image.edit: retry loop exited unexpectedly");
}

}  // namespace gptimage
