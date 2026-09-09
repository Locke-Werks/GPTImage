#include "tools.hpp"
#include "tool_common.hpp"

#include <gptimage/image_client.hpp>

#include <string>

namespace gptimage {

using nlohmann::json;

namespace {

// Quality tiers up to the configured ceiling, so a client is never offered a
// tier the server would silently clamp away. "auto" always rides along: it asks
// the model to pick, which the ceiling does not constrain.
json quality_enum_up_to(const std::string& ceiling) {
    static const char* kTiers[] = {"low", "medium", "high", "xhigh", "max"};
    const int cap = quality_rank(ceiling);
    json out = json::array({"auto"});
    for (const char* t : kTiers) {
        if (quality_rank(t) <= cap) out.push_back(t);
    }
    return out;
}

// Every image tool shares these argument shapes. They are built once per
// tools/list rather than written out five times, so the two model variants of a
// tool cannot drift apart in anything but their model and their prose.
struct SharedProps {
    json size;
    json quality;
    json background;
    json format;
    json n;
    json compression;
};

SharedProps shared_props(const ImageConfig& ic) {
    SharedProps p;
    p.size = {
        {"type", "string"},
        {"default", ic.default_size},
        {"description",
            "Output size as WIDTHxHEIGHT, or \"auto\" to let the model pick from "
            "the prompt. Use 1024x1024 (square), 1536x1024 (landscape) or "
            "1024x1536 (portrait) unless the user needs a particular shape; square "
            "is typically fastest. Custom sizes are allowed if every rule holds: "
            "both edges multiples of 16, aspect ratio between 1:3 and 3:1, neither "
            "edge over 3840, total pixels between 655,360 and 8,294,400. Breaking "
            "one is a 4xx naming the rule, not a silent resize. Above 2560x1440 is "
            "experimental, markedly slower, and costs proportionally more since "
            "output tokens scale with pixel count."},
    };
    p.quality = {
        {"type", "string"},
        {"enum", quality_enum_up_to(ic.max_quality)},
        {"default", ic.default_quality},
        {"description",
            "Rendering quality. This is the single biggest lever on both wait and "
            "cost, so choose it deliberately. Measured per image at 1024x1024, "
            "identical on both models: low $0.006, medium $0.013, high $0.053, "
            "xhigh $0.094, max about $0.21. Cost scales with pixel count too, so a "
            "4K render costs several times these. Pick low for a throwaway check, "
            "medium for ordinary work, high when the image carries small text such "
            "as a diagram or slide, and xhigh or max only for a final asset the "
            "user has asked to finish. \"auto\" hands the choice to the model, "
            "which makes cost unpredictable; prefer naming a tier. Asking above "
            "the server's ceiling is clamped down to it rather than refused, and "
            "the caption reports the tier that actually ran."},
    };
    p.background = {
        {"type", "string"},
        {"enum", json::array({"auto", "opaque", "transparent"})},
        {"default", ic.default_background},
        {"description",
            "Background handling. \"transparent\" requires format png or webp and "
            "is silently ignored with jpeg, which has no alpha channel. Use it for "
            "logos, icons, product cut-outs and anything to be composited later; on "
            "an edit it is how you strip a background off an existing image. "
            "\"opaque\" forces a filled background, \"auto\" lets the model decide "
            "and is the safe choice otherwise."},
    };
    p.format = {
        {"type", "string"},
        {"enum", json::array({"png", "jpeg", "webp"})},
        {"default", ic.default_format},
        {"description",
            "Output image format, which changes the payload size and nothing about "
            "the render or its cost. Keep webp: a full-resolution png runs 1.5-3 MB, "
            "and an inline image travels as base64, so a png is heavy enough that a "
            "remote connector may drop the whole result and the user sees nothing. "
            "Choose png only when the user needs lossless output or a clean alpha "
            "channel and can accept that weight; jpeg is the fastest to produce but "
            "has no transparency."},
    };
    p.n = {
        {"type", "integer"}, {"default", 1}, {"minimum", 1}, {"maximum", ic.max_n},
        {"description",
            "How many images to generate in this one call, 1 to " +
            std::to_string(ic.max_n) + ". Each is billed in full, so n:4 at high "
            "quality costs four high-quality renders. Leave it at 1 unless the user "
            "has actually asked for variations to choose between; do not raise it "
            "to hedge against a bad result, since a re-run costs the same and only "
            "when needed."},
    };
    p.compression = {
        {"type", "integer"}, {"minimum", 0}, {"maximum", 100},
        {"default", ic.default_compression},
        {"description",
            "Compression quality for the lossy formats webp and jpeg, 0-100, where "
            "higher means better looking and a larger payload. Ignored for png. The "
            "server default is tuned to keep the picture light enough to render "
            "inline; raise it toward 100 only if the user complains about artifacts, "
            "and remember that a heavier payload is likelier to be dropped in "
            "transit than to look better."},
    };
    return p;
}

// Appended to every generate/edit description. The async contract, the
// render-the-link step and the error contract are identical across all four, and
// a caller that misses any of them either stalls on a pending job, hands back an
// invisible image, or reports a fixable rejection as a broken server. Spelled
// out once here rather than half-said four times.
const char* kDeliveryNote =
    "\n\nWHAT COMES BACK. One of three shapes:\n"
    "1. The finished image, as an image content block you can see, plus a caption "
    "naming the model, the quality that actually ran, the tokens billed, the "
    "dollar cost, and the file path if the server saves renders to disk. If a "
    "markdown image link is present, reproduce it verbatim in your reply: that "
    "link is the only thing that puts the picture in the conversation body rather "
    "than inside the collapsed tool-call widget. If a \"Saved to\" path is "
    "present, tell the user that path; it is the copy that outlives the link.\n"
    "2. JSON with status \"pending\" and a job_id, when the render outran this "
    "call's wait window. This is normal, not a failure, and not something to "
    "report to the user. Call gptimage_result with that exact job_id and keep "
    "calling until the image arrives. Each poll waits up to 25 seconds, so one to "
    "three polls is typical. Do not start a second render and do not tell the "
    "user to check back.\n"
    "3. An error, with OpenAI's own message. A moderation block names the stage "
    "(prompt or generated image) and a category; rephrase and retry once before "
    "giving up. A size or argument complaint tells you exactly what to fix. Do "
    "not conclude the server is broken.\n\n"
    "COST. Every call spends real money on the operator's OpenAI account, and the "
    "quality argument decides how much. Do not raise quality or n to hedge.";

json generate_tool(const std::string& name, const std::string& title,
                   const std::string& description, const SharedProps& p) {
    return {
        {"name", name},
        {"description", description + kDeliveryNote},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"prompt", {{"type", "string"},
                            {"description",
                                "What to draw. Name the subject, the composition, the "
                                "lighting, the medium and the mood; vague prompts get "
                                "generic results. Put any wording that must appear in "
                                "the image inside quotation marks and say where it sits, "
                                "spelling brand names out letter by letter if they are "
                                "unusual. For a fixed layout, send a JSON object as the "
                                "whole prompt with a zone and an exact string per "
                                "element rather than describing it in a sentence."}}},
                {"size",        p.size},
                {"quality",     p.quality},
                {"background",  p.background},
                {"format",      p.format},
                {"n",           p.n},
                {"compression", p.compression},
            }},
            {"required", json::array({"prompt"})},
        }},
        {"annotations", {
            {"title", title},
            {"readOnlyHint", false},
            {"destructiveHint", false},
            {"idempotentHint", false},  // same prompt -> a different image each call
            {"openWorldHint", true},    // calls the OpenAI image API
        }},
    };
}

json edit_tool(const std::string& name, const std::string& title,
               const std::string& description, const SharedProps& p,
               int max_input_images) {
    return {
        {"name", name},
        {"description", description + kDeliveryNote},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"images", {{"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"minItems", 1},
                            {"maxItems", max_input_images},
                            {"description",
                                "Input image(s) as base64-encoded bytes (png/jpeg/webp), "
                                "1 to " + std::to_string(max_input_images) + ". A data: "
                                "URI is accepted and its prefix stripped. Order matters "
                                "only in that a mask must match the first image. When "
                                "you pass more than one, say in the prompt what each is "
                                "for (subject, style, background, garment) or the model "
                                "has to guess. Downscale before encoding: base64 is "
                                "about 4/3 the byte size and has to be emitted as text, "
                                "and each reference also bills roughly $0.008 in input "
                                "tokens, so 512px at jpeg quality 60-70 is usually "
                                "plenty."}}},
                {"prompt", {{"type", "string"},
                            {"description",
                                "How to edit or combine the input image(s). State what "
                                "must be preserved as explicitly as what changes, since "
                                "the common failure is the model rewriting more of the "
                                "frame than you meant. Restate those constraints on each "
                                "pass if results start drifting."}}},
                {"mask",   {{"type", "string"},
                            {"description",
                                "Optional base64 PNG mask selecting the region to edit. "
                                "Its TRANSPARENT pixels are what gets redrawn; opaque "
                                "pixels are kept. It must have an alpha channel and "
                                "match the first input image's format and dimensions "
                                "exactly, under 50MB. Omit it to let the model rewrite "
                                "the whole frame."}}},
                {"size",        p.size},
                {"quality",     p.quality},
                {"background",  p.background},
                {"format",      p.format},
                {"n",           p.n},
                {"compression", p.compression},
            }},
            {"required", json::array({"images", "prompt"})},
        }},
        {"annotations", {
            {"title", title},
            {"readOnlyHint", false},
            {"destructiveHint", false},
            {"idempotentHint", false},
            {"openWorldHint", true},
        }},
    };
}

}  // namespace

json mcp_tool_schemas(const ImageConfig& ic) {
    const SharedProps p = shared_props(ic);

    return json::array({
        generate_tool(
            "gptimage_generate_flare", "Generate Image (Flare)",
            "Generate an image from a text prompt with OpenAI GPT Image 2.5 Flare. "
            "Reach for this one by default. It is the fast everyday model: about "
            "11 s at medium quality and 18 s at high for a 1024x1024, which is "
            "roughly half what Sunburst takes and fast enough that most renders "
            "come back inside this single call instead of as a job to poll. Suits "
            "drafts, iteration, social and creator content, visual search, and "
            "anything high-volume.\n\n"
            "Describe what you want in plain language and be specific about "
            "subject, composition, lighting, medium and mood; the model handles the "
            "rest and renders legible text. For anything with exact wording or a "
            "fixed layout (a poster, a slide, a label, a UI mock) send a JSON "
            "object as the entire prompt, giving each element a zone and an exact "
            "string, which holds position and spelling far better than a sentence.\n\n"
            "Prefer gptimage_generate_sunburst only when the image is a final asset "
            "and the extra wait is worth it. It is not cheaper or dearer, just "
            "slower and more exacting.",
            p),
        generate_tool(
            "gptimage_generate_sunburst", "Generate Image (Sunburst)",
            "Generate an image from a text prompt with OpenAI GPT Image 2.5 "
            "Sunburst, the precision model. Same arguments, same price and the same "
            "token count per image as Flare: the whole trade is wall-clock time "
            "against exactness. Reckon on about 18 s at medium and 33 s at high for "
            "a 1024x1024, so expect a job_id and a poll rather than an inline "
            "return.\n\n"
            "Use it for work that has to be right the first time: production "
            "campaign creative, polished product imagery, complex or layout-"
            "sensitive compositions, a series of on-brand assets that must hold a "
            "given visual direction. Its advantage is holding a detailed brief "
            "without drifting as the instructions get more specific.\n\n"
            "Do not use it to explore. Draft on gptimage_generate_flare, settle the "
            "composition with the user, then render the keeper here once.",
            p),
        edit_tool(
            "gptimage_edit_flare", "Edit Image (Flare)",
            "Edit, extend or combine existing images with OpenAI GPT Image 2.5 "
            "Flare. Pass the input images as base64 strings plus a prompt saying "
            "what to change. Three modes, chosen by what you send:\n"
            "- one image, no mask: rewrites the whole frame guided by the original\n"
            "- one image plus a mask: targeted inpainting, only the mask's "
            "transparent pixels are redrawn\n"
            "- several images: composites them into one scene; say in the prompt "
            "what each input is for (subject, style, background, garment)\n\n"
            "Every input is processed at full fidelity automatically. There is no "
            "input_fidelity argument and the API rejects one. That costs input "
            "tokens: about $0.008 per 1024x1024 reference, which can exceed the "
            "output cost of the render itself, so send the smallest inputs that "
            "still carry the composition.\n\n"
            "This is the fast path, right for iterating on a previous result. When "
            "the edit must change one thing and leave everything else untouched, "
            "use gptimage_edit_sunburst.",
            p, ic.max_input_images),
        edit_tool(
            "gptimage_edit_sunburst", "Edit Image (Sunburst)",
            "Edit, extend or combine existing images with OpenAI GPT Image 2.5 "
            "Sunburst. Editing is what Sunburst is built for and the place its "
            "extra time actually earns itself. Choose it when the job is to change "
            "one element (a product, a background, a line of copy) while the "
            "subject, composition, lighting and surrounding detail stay exactly as "
            "they were, or when a run of successive edits has to stay consistent "
            "without the picture degrading pass over pass.\n\n"
            "Same three modes and the same arguments as gptimage_edit_flare: one "
            "image alone rewrites the frame, one image plus a mask inpaints just "
            "the masked region, several images composite into one scene. Every "
            "input is processed at full fidelity, which costs about $0.008 per "
            "1024x1024 reference in input tokens.\n\n"
            "State what must be preserved as explicitly as what changes, and "
            "restate those constraints on each pass if the result starts drifting. "
            "Slower than Flare, so expect a job_id.",
            p, ic.max_input_images),
        {
            {"name", "gptimage_result"},
            {"description",
                "Fetch an image that a generate or edit tool started but did not "
                "return inline, because the render outran that call's wait window "
                "and it handed back a job_id with status \"pending\" instead. This "
                "is the only way to collect a slow render.\n\n"
                "Pass the job_id exactly as given. The call blocks up to 25 seconds "
                "waiting, so a poll is not a busy-wait and needs no delay between "
                "attempts. If the render finished you get the image, the caption "
                "and any markdown link, identical to what the original tool would "
                "have returned. If it is still working you get \"pending\" again, "
                "and the correct response is to call this tool again with the same "
                "job_id. Keep going until the image arrives: one to three polls is "
                "typical, and Sunburst at a high quality can take a few more.\n\n"
                "Pending is not a failure and not an outcome to report. Do not "
                "start a fresh render, do not switch models, and do not tell the "
                "user to check back later; there is nothing for them to do. Only "
                "after roughly ten fruitless polls should you say the render looks "
                "stalled and offer to retry at a lower quality.\n\n"
                "A finished render stays fetchable by the same job_id for 24 hours, "
                "so re-fetching one you already showed returns the same image and "
                "the same link rather than costing another render. This tool is "
                "read-only: it starts no work and spends nothing."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"job_id", {{"type", "string"},
                                {"description",
                                    "The job_id handed back by gptimage_generate_flare, "
                                    "gptimage_generate_sunburst, gptimage_edit_flare or "
                                    "gptimage_edit_sunburst, copied verbatim. Looks like "
                                    "\"job_1f0c3854f156117c84a9204b\"."}}},
                }},
                {"required", json::array({"job_id"})},
            }},
            {"annotations", {
                {"title", "Get Image Result"},
                {"readOnlyHint", true},   // just fetches; never starts new work
                {"openWorldHint", false},
            }},
        },
    });
}

json mcp_tool_call(const std::string& name, const json& arguments, ToolContext& ctx) {
    const ImageConfig& ic = ctx.cfg.image;
    if (name == "gptimage_generate_flare")    return tool_generate(arguments, ctx, ic.model_flare);
    if (name == "gptimage_generate_sunburst") return tool_generate(arguments, ctx, ic.model_sunburst);
    if (name == "gptimage_edit_flare")        return tool_edit(arguments, ctx, ic.model_flare);
    if (name == "gptimage_edit_sunburst")     return tool_edit(arguments, ctx, ic.model_sunburst);
    if (name == "gptimage_result")            return tool_result(arguments, ctx);
    return text_result("unknown tool: '" + name + "'", /*is_error=*/true);
}

}  // namespace gptimage
