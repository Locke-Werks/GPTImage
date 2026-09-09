#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace gptimage {

struct DatabaseConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 5432;
    std::string dbname;
    std::string user;
    std::string password_env;
    std::string schema = "gptimage";
    // libpq sslmode, passed through verbatim when non-empty. Empty leaves it
    // unset (libpq defaults to "prefer"). Validated against the libpq value set
    // at load time.
    std::string sslmode;

    // Resolved from environment at load time. Empty if password_env unset.
    std::string password;
};

// [image] — the OpenAI image-generation backend. The whole point of the server:
// the gptimage_* tools call these endpoints with the caller's prompt and hand
// the base64 result straight back as an MCP image block.
struct ImageConfig {
    // Fallback model for a request that does not pin one. Each tool pins its own
    // (model_flare / model_sunburst), so this only applies to a bare request.
    std::string model = "gpt-image-2.5-flare";
    // The two models the tools pin, one tool pair each. Both are "ChatGPT Images
    // 2.5": Flare is the fast everyday model, Sunburst trades latency for tighter
    // control across edits. Set a dated snapshot ("gpt-image-2.5-flare-2026-09-08")
    // to freeze behaviour across OpenAI's releases.
    std::string model_flare    = "gpt-image-2.5-flare";
    std::string model_sunburst = "gpt-image-2.5-sunburst";
    // Env var holding the OpenAI key. The key itself never lives in the TOML.
    std::string api_key_env = "OPENAI_API_KEY";
    std::string generations_endpoint = "https://api.openai.com/v1/images/generations";
    std::string edits_endpoint       = "https://api.openai.com/v1/images/edits";

    // Directory finished renders are written to, as <job_id>-<index>.<ext>.
    // Empty disables saving and the server keeps nothing.
    //
    // The in-memory cache drops a render after job_ttl_seconds, which is enough
    // to deliver an image and not enough to keep one: a render nobody downloaded
    // in time is gone. A save directory is the fix at both ends. Locally it puts
    // the file on the user's own disk without them having to think about it; on
    // a hosted deployment it is what stops /i/<job_id> 404ing a day later, since
    // the route falls back to this directory once memory has let go.
    //
    // "~" and a leading "~/" are expanded at load time. On the VPS this wants a
    // path systemd actually grants (StateDirectory=gptimage gives
    // /var/lib/gptimage), because ProtectSystem=strict makes everything else
    // read-only.
    std::string save_dir;
    // Oldest files above this count are deleted after each save, so an agent in
    // a loop cannot fill the disk out from under everything else on the box.
    // 0 = keep everything, and mind the disk yourself.
    int save_max_files = 2000;

    // Public origin the HTTP transport is reachable at, e.g.
    // "https://gptimage.specterpoint.com" (no trailing slash). When set, a
    // finished render is also served at <public_base_url>/i/<job_id>-<index>.<ext>
    // and the tools hand the client a markdown image link, so the picture renders
    // in the conversation body instead of only inside the collapsed tool-call
    // block. Left empty it defaults to auth.oauth.issuer when OAuth is on; empty
    // with no issuer ⇒ inline base64 only (the stdio/local path — nothing hosted).
    std::string public_base_url;

    // Defaults applied when a tool call omits the field.
    std::string default_size       = "1024x1024";  // WxH (div by 16, 1:3..3:1) or "auto"
    // medium, not low: Flare renders 2-4x faster than gpt-image-2 did, so the
    // tier OpenAI recommends starting from now lands inside the poll window
    // instead of falling through to a job id.
    std::string default_quality    = "medium";      // auto|low|medium|high|xhigh|max
    std::string default_background = "auto";        // transparent|opaque|auto
    // webp, not png: an inline image round-trips as base64 in the tool result,
    // and a full-res png (1.5-3 MB) is large enough that a remote connector
    // (claude.ai) drops it and the picture never renders. webp+compression keeps
    // the payload light so it lands inline every time. Override to png only when
    // you need lossless output and can accept a large payload.
    std::string default_format     = "webp";        // png|jpeg|webp
    // Compression level for the lossy formats (webp/jpeg), 0-100; higher is
    // better quality and a bigger payload. Ignored for png. -1 omits it (OpenAI
    // then uses 100 = largest). 80 is visually near-lossless yet a fraction of a
    // png's size.
    int         default_compression = 80;
    std::string moderation         = "low";         // auto|low

    // Ceiling on the quality tier a caller may ask for: low < medium < high <
    // xhigh < max. The quality-side companion to max_n, and the one that matters
    // more, because output tokens climb steeply at the top of the range and a
    // "max" render runs several times a "high" one. A request above the ceiling
    // is clamped down to it, not rejected. Set "max" to lift the cap. "auto" is
    // always allowed and is the one way past it, since the model picks the tier.
    std::string max_quality = "xhigh";

    // Published OpenAI rates per 1M tokens, used only to turn the usage the API
    // reports back into a dollar figure in the result caption. Both GPT Image
    // 2.5 models bill identically. Nothing depends on these being right except
    // the accuracy of that caption; update them when the pricing page moves.
    double price_text_input_per_m   = 5.00;
    double price_image_input_per_m  = 8.00;
    double price_image_output_per_m = 30.00;

    int max_n              = 4;    // hard cap on images per call (cost guard)
    // The edits endpoint takes at most this many reference images. Rejecting an
    // over-long array here costs nothing; uploading it and being refused costs a
    // round trip carrying every image.
    int max_input_images   = 16;
    int timeout_s          = 300;  // xhigh/max at 4K is slower than 2K ever was
    int max_retries        = 4;    // on 429/5xx/network
    int backoff_initial_ms = 800;

    // Async job pattern. Generate/edit start a background render and return
    // fast; gptimage_result fetches it. Each generate/result call blocks up to
    // job_poll_seconds (kept well under a remote connector's tool-call timeout)
    // so a fast render returns in one call and a slow one is polled in chunks.
    int job_poll_seconds     = 25;
    // How long a finished render is kept in the in-memory cache after it
    // completes. This is the window its hosted URL stays live: a day, so the
    // client can fetch and render the link now and still come back to the same
    // image later without regenerating it. After that the render is dropped and
    // nothing is persisted. Pending jobs are never evicted.
    int job_ttl_seconds      = 86400;  // 24 h
    int max_concurrent_jobs  = 4;

    // Resolved from environment at load time. Empty ⇒ the tools return an error
    // result instead of calling out.
    std::string api_key;
};

struct McpConfig {
    std::string transport = "stdio";            // "stdio" | "tcp" (legacy) | "http"
    std::string tcp_bind  = "127.0.0.1:17718";
    // Streamable HTTP transport. Binds loopback — Caddy fronts it with TLS and
    // proxies https://gptimage.specterpoint.com/mcp here.
    std::string http_bind = "127.0.0.1:17718";
    // An inline image round-trips as base64 in the JSON-RPC response; generated
    // images are large, so the body cap is generous compared with a text server.
    int  http_max_body_bytes  = 32 << 20;       // 413 above this (32 MiB)
    int  http_read_timeout_s  = 30;
    int  http_write_timeout_s = 200;            // gpt-image-2 high/2K is slow
};

// One configured principal: the mint-time template for static API tokens and
// the grant source for JWT-authenticated callers. Realm names may include "*"
// (wildcard read/write-all). Realm scoping is unused by the image tools but the
// grant machinery is shared verbatim with the auth/OAuth layer.
struct AuthPrincipalConfig {
    std::string              name;
    std::string              home_realm;
    std::vector<std::string> read_realms;
    std::vector<std::string> write_realms;
    std::string              max_sensitivity;
};

// [auth.oauth] — the embedded OAuth 2.1 authorization server (the login page
// claude.ai / ChatGPT connectors land on). Off by default: a box that only runs
// stdio or static-token HTTP never loads a signing key.
struct OAuthConfig {
    bool        enabled = false;
    // Issuer/base URL, e.g. "https://gptimage.specterpoint.com" — no trailing
    // slash. Also the RFC 8414 metadata `issuer`.
    std::string issuer;
    // Canonical protected resource (RFC 8707), e.g. issuer + "/mcp". Minted
    // access tokens carry this as `aud`.
    std::string resource;
    // RS256 signing key (PEM, private). previous_key_paths keeps rotated-out
    // keys in the published JWKS until their tokens age out.
    std::string              signing_key_path;
    std::vector<std::string> previous_key_paths;
    // Registered redirect URIs must be https and their host must equal or be a
    // subdomain of an entry here. Empty list = allow any host (discouraged).
    std::vector<std::string> redirect_hosts =
        {"claude.ai", "claude.com", "chatgpt.com", "openai.com"};
    // Access tokens have no revocation store (jti is minted, not checked), so
    // the TTL is the only bound on a leaked token — keep it short. Refresh
    // tokens (30 d, sliding) carry longevity; clients refresh transparently.
    int access_token_ttl_s  = 900;      // 15 min
    int auth_code_ttl_s     = 600;      // 10 min
    int refresh_token_ttl_s = 2592000;  // 30 d, sliding on rotation
    int max_clients         = 50;       // DCR cap
};

struct AuthConfig {
    // Only consulted for transport="http"; stdio/tcp trust the OS boundary and
    // always run as the local operator.
    bool                             enabled = true;
    std::vector<AuthPrincipalConfig> principals;
    // Emitted in WWW-Authenticate and served at the
    // /.well-known/oauth-protected-resource route when set.
    std::string                      resource_metadata_url;
    // JWT validation. When [auth.oauth] is enabled and these are empty they are
    // derived from oauth.issuer/oauth.resource at load time, so the mint and
    // verify sides cannot drift.
    std::string                      jwt_issuer;
    std::string                      jwt_audience;
    std::string                      jwt_jwks_url;
    std::string                      jwt_principal_claim = "sub";
    // (claim value -> principal name) overrides, for when the JWT subject is a
    // UUID rather than a friendly principal name.
    std::vector<std::pair<std::string, std::string>> jwt_subject_map;
    OAuthConfig                      oauth;
};

struct LoggingConfig {
    std::string level = "info";
    std::string path;
    int rotate_mb = 25;
    int retain_files = 10;
};

struct Config {
    DatabaseConfig database;
    ImageConfig    image;
    McpConfig      mcp;
    AuthConfig     auth;
    LoggingConfig  logging;
};

// Parses a TOML config file and resolves *_env fields from the environment.
// Required fields: database.dbname, database.user. Missing env vars referenced
// by password_env / api_key_env are tolerated (left empty) but validated before
// use.
Config load_config(const std::filesystem::path& path);

// $GPTIMAGE_CONFIG if set, else "config/gptimage.toml" under CWD.
std::filesystem::path default_config_path();

}  // namespace gptimage
