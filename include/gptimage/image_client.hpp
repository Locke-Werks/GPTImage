#pragma once

#include <gptimage/config.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace gptimage {

// One image returned by the API. `b64` is the base64 payload exactly as the API
// produced it (data[i].b64_json) — the MCP image block wants base64, so it goes
// straight through with no decode/re-encode. `mime` is derived from the request
// format.
struct GeneratedImage {
    std::string b64;
    std::string mime;   // "image/png" | "image/jpeg" | "image/webp"
};

// Token usage as reported by the API, for cost accounting/logging. -1 = absent.
// The two detail fields come from usage.input_tokens_details and are what make
// a cost figure exact rather than approximate: image input tokens bill at a
// different rate from text, and an edit carrying reference images is mostly
// image input.
struct ImageUsage {
    long input_tokens        = -1;
    long output_tokens       = -1;
    long total_tokens        = -1;
    long input_text_tokens   = -1;
    long input_image_tokens  = -1;
};

struct ImageResponse {
    std::vector<GeneratedImage> images;
    ImageUsage                  usage;
};

// Request parameters shared by generate and edit. Empty strings fall back to the
// ImageConfig defaults at the call site and are omitted from the wire request
// when still empty (letting the API's own default apply).
struct ImageRequest {
    std::string prompt;
    // Model id for this one request. Empty falls back to ImageConfig::model, so
    // a caller that does not care keeps working; the tools each pin their own.
    std::string model;
    std::string size;        // WxH | "auto"
    std::string quality;     // auto|low|medium|high|xhigh|max (top two are 2.5 only)
    std::string background;  // transparent|opaque|auto
    std::string format;      // png|jpeg|webp
    int         n = 1;
    int         compression = -1;  // 0..100 for jpeg/webp; -1 => omit
};

// Any failure the tool surfaces to the caller as an isError result: a missing
// key, network/5xx/429 after retries, a 4xx (including moderation/policy
// rejections, which are never retried), or a malformed response. The message is
// the API's own error text when available.
struct ImageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Standard-base64 decode (tolerates whitespace and a leading data: URI prefix).
// Returns an empty vector on invalid input. Used to turn the base64 image args
// of the edit tools back into the raw bytes the multipart upload needs.
std::vector<unsigned char> base64_decode(const std::string& in);

// Ordering of the quality tiers, for the max_quality cost guard. "auto" ranks 0
// because the model chooses; an unrecognized tier ranks -1 so a typo cannot
// clamp to something expensive.
int quality_rank(const std::string& q);

// Clamp `requested` down to `ceiling` when it ranks higher. "auto" and anything
// unrecognized pass through untouched, and the API validates those itself.
std::string clamp_quality(const std::string& requested, const std::string& ceiling);

// Dollar cost of one API call from the usage it reported, at the configured
// per-1M rates. Returns -1 when the API reported no usage at all. When usage
// arrives without input_tokens_details the whole input is billed at the text
// rate, which understates an edit carrying reference images; `exact` says
// which happened so the caller can mark the figure as approximate.
double usage_cost_usd(const ImageUsage& u, const ImageConfig& cfg, bool* exact = nullptr);

class ImageClient {
public:
    explicit ImageClient(ImageConfig cfg);

    // POST <generations_endpoint> as JSON.
    ImageResponse generate(const ImageRequest& req);

    // POST <edits_endpoint> as multipart/form-data. `images` are raw input bytes
    // (PNG/JPEG/WebP); `mask` is optional (raw bytes; its alpha channel marks
    // the region to edit).
    ImageResponse edit(const ImageRequest& req,
                       const std::vector<std::vector<unsigned char>>& images,
                       const std::optional<std::vector<unsigned char>>& mask);

private:
    ImageConfig cfg_;
};

}  // namespace gptimage
