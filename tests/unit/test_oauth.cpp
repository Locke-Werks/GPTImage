#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "oauth.hpp"

#include <memory>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("PKCE S256 matches the RFC 7636 appendix B vector") {
    // Verifier and its published S256 transform, straight from the RFC.
    const std::string verifier  = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string challenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
    CHECK(gptimage::oauth_s256_challenge(verifier) == challenge);
}

TEST_CASE("PKCE string validation: length and charset bounds") {
    const std::string ok(43, 'a');
    CHECK(gptimage::oauth_valid_pkce_string(ok));
    CHECK(gptimage::oauth_valid_pkce_string(std::string(128, 'A')));
    CHECK(gptimage::oauth_valid_pkce_string("abcDEF123-._~" + std::string(30, 'x')));

    CHECK_FALSE(gptimage::oauth_valid_pkce_string(std::string(42, 'a')));   // too short
    CHECK_FALSE(gptimage::oauth_valid_pkce_string(std::string(129, 'a')));  // too long
    CHECK_FALSE(gptimage::oauth_valid_pkce_string(std::string(43, '+')));   // bad charset
    CHECK_FALSE(gptimage::oauth_valid_pkce_string(std::string(40, 'a') + "a=b"));
}

TEST_CASE("https host extraction") {
    CHECK(gptimage::oauth_https_host_of("https://claude.ai/api/mcp/auth_callback") == "claude.ai");
    CHECK(gptimage::oauth_https_host_of("https://ChatGPT.com/callback") == "chatgpt.com");
    CHECK(gptimage::oauth_https_host_of("https://api.openai.com:8443/cb") == "api.openai.com");
    CHECK(gptimage::oauth_https_host_of("https://host.example") == "host.example");

    CHECK(gptimage::oauth_https_host_of("http://claude.ai/cb").empty());       // not https
    CHECK(gptimage::oauth_https_host_of("claude.ai/cb").empty());              // no scheme
    CHECK(gptimage::oauth_https_host_of("https://user@evil.com/cb").empty());  // userinfo
    CHECK(gptimage::oauth_https_host_of("").empty());
}

TEST_CASE("JSON nesting-depth guard") {
    CHECK(gptimage::json_within_depth("{}", 64));
    CHECK(gptimage::json_within_depth(R"({"a":{"b":[1,2,3]}})", 64));
    CHECK(gptimage::json_within_depth("", 64));
    // Brackets inside strings don't count toward depth.
    CHECK(gptimage::json_within_depth(R"({"k":"[[[[[[[["})", 4));

    // A ladder deeper than the limit is rejected.
    std::string deep(200, '[');
    CHECK_FALSE(gptimage::json_within_depth(deep, 64));
    // Exactly at the limit passes; one deeper fails.
    CHECK(gptimage::json_within_depth(std::string(64, '['), 64));
    CHECK_FALSE(gptimage::json_within_depth(std::string(65, '['), 64));
}

TEST_CASE("redirect-host allowlist: exact + subdomain, no lookalikes") {
    const std::vector<std::string> allow = {"claude.ai", "chatgpt.com"};

    CHECK(gptimage::oauth_host_allowed("claude.ai", allow));
    CHECK(gptimage::oauth_host_allowed("api.claude.ai", allow));
    CHECK(gptimage::oauth_host_allowed("chatgpt.com", allow));
    CHECK(gptimage::oauth_host_allowed("connector.chatgpt.com", allow));

    CHECK_FALSE(gptimage::oauth_host_allowed("notclaude.ai", allow));
    CHECK_FALSE(gptimage::oauth_host_allowed("claude.ai.evil.com", allow));
    CHECK_FALSE(gptimage::oauth_host_allowed("evil.com", allow));
    CHECK_FALSE(gptimage::oauth_host_allowed("", allow));

    // Empty allowlist = explicitly allow everything (config escape hatch).
    CHECK(gptimage::oauth_host_allowed("anything.example", {}));
}

TEST_CASE("loopback redirects: http to 127.0.0.1, [::1], localhost, any port") {
    using gptimage::oauth_is_loopback_redirect;

    CHECK(oauth_is_loopback_redirect("http://127.0.0.1:25179/callback"));
    CHECK(oauth_is_loopback_redirect("http://127.0.0.1/callback"));
    CHECK(oauth_is_loopback_redirect("http://[::1]:25179/callback"));
    CHECK(oauth_is_loopback_redirect("http://[::1]/callback"));
    CHECK(oauth_is_loopback_redirect("http://localhost:8080/callback"));
    CHECK(oauth_is_loopback_redirect("http://localhost/callback"));
    CHECK(oauth_is_loopback_redirect("http://LocalHost:8080/cb"));
    CHECK(oauth_is_loopback_redirect("http://127.0.0.1:1"));

    // http to anything that is not loopback stays rejected.
    CHECK_FALSE(oauth_is_loopback_redirect("http://claude.ai/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://evil.com:25179/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://127.0.0.2/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://localhost.evil.com/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://127.0.0.1.evil.com/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://[::2]/callback"));
    // Credentials, malformed ports and other schemes.
    CHECK_FALSE(oauth_is_loopback_redirect("http://user@127.0.0.1/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://evil.com@127.0.0.1/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://127.0.0.1:/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://127.0.0.1:99999/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://127.0.0.1:80a/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("http://[::1]x/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("https://127.0.0.1/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("ftp://127.0.0.1/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect("com.example.app:/callback"));
    CHECK_FALSE(oauth_is_loopback_redirect(""));

    // https to a host off the allowlist is still refused by the https rule.
    const std::vector<std::string> allow = {"claude.ai", "chatgpt.com"};
    CHECK_FALSE(gptimage::oauth_host_allowed(
        gptimage::oauth_https_host_of("https://evil.com/callback"), allow));
}

TEST_CASE("redirect matching: loopback port may differ, nothing else may") {
    using gptimage::oauth_redirect_matches;
    const std::string reg = "http://127.0.0.1:25179/callback";

    CHECK(oauth_redirect_matches(reg, reg));
    CHECK(oauth_redirect_matches(reg, "http://127.0.0.1:61000/callback"));
    CHECK(oauth_redirect_matches(reg, "http://127.0.0.1/callback"));
    CHECK(oauth_redirect_matches("http://[::1]:1/cb", "http://[::1]:2/cb"));
    CHECK(oauth_redirect_matches("http://localhost:1/cb?x=1", "http://localhost:2/cb?x=1"));

    CHECK_FALSE(oauth_redirect_matches(reg, "http://127.0.0.1:61000/other"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://127.0.0.1:61000/callback/"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://127.0.0.1:61000/callback?x=1"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://127.0.0.1:61000/callback#x"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://localhost:25179/callback"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://[::1]:25179/callback"));
    CHECK_FALSE(oauth_redirect_matches(reg, "http://evil.com:25179/callback"));
    CHECK_FALSE(oauth_redirect_matches(reg, "https://127.0.0.1:25179/callback"));

    // Non-loopback registrations keep exact matching, port included.
    const std::string web = "https://claude.ai/api/mcp/auth_callback";
    CHECK(oauth_redirect_matches(web, web));
    CHECK_FALSE(oauth_redirect_matches(web, "https://claude.ai:8443/api/mcp/auth_callback"));
    CHECK_FALSE(oauth_redirect_matches(web, "https://claude.ai/api/mcp/auth_callback/"));
    CHECK_FALSE(oauth_redirect_matches(web, "https://api.claude.ai/api/mcp/auth_callback"));
    CHECK_FALSE(oauth_redirect_matches("https://claude.ai:443/cb", "https://claude.ai/cb"));
}

TEST_CASE("registration refuses non-loopback http and off-allowlist https") {
    // Every refusal here happens before the rate limiter or the database is
    // touched, so a default config and a throwaway key are enough.
    gptimage::Config cfg;
    auto keys = std::make_shared<std::vector<gptimage::SigningKey>>();
    keys->push_back(gptimage::SigningKey::load_pem(gptimage::generate_rsa_key_pem(2048)));
    gptimage::OAuthService svc(cfg, keys);

    auto refused = [&](const std::string& uri) -> std::string {
        const nlohmann::json body{{"redirect_uris", nlohmann::json::array({uri})}};
        auto out = svc.register_client(body, "203.0.113.1");
        if (!std::holds_alternative<gptimage::OAuthError>(out)) return "accepted";
        return std::get<gptimage::OAuthError>(out).error;
    };
    CHECK(refused("http://evil.com/callback") == "invalid_redirect_uri");
    CHECK(refused("http://claude.ai/callback") == "invalid_redirect_uri");
    CHECK(refused("https://evil.com/callback") == "invalid_redirect_uri");
    CHECK(refused("ftp://127.0.0.1/callback") == "invalid_redirect_uri");
    CHECK(refused("http://user:pw@127.0.0.1/callback") == "invalid_redirect_uri");
    CHECK(refused("http://127.0.0.1:25179/callback#frag") == "invalid_redirect_uri");
}
