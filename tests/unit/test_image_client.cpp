#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <gptimage/config.hpp>
#include <gptimage/image_client.hpp>

#include <string>

using gptimage::clamp_quality;
using gptimage::quality_rank;
using gptimage::usage_cost_usd;

TEST_CASE("quality tiers rank in cost order") {
    CHECK(quality_rank("low")    < quality_rank("medium"));
    CHECK(quality_rank("medium") < quality_rank("high"));
    CHECK(quality_rank("high")   < quality_rank("xhigh"));
    CHECK(quality_rank("xhigh")  < quality_rank("max"));
    // auto is not a tier: the model picks, so it cannot be ordered against one.
    CHECK(quality_rank("auto") == 0);
    // A typo must not read as "higher than the ceiling" or as a valid ceiling.
    CHECK(quality_rank("ultra") == -1);
    CHECK(quality_rank("") == -1);
}

TEST_CASE("clamp_quality holds the cost ceiling") {
    CHECK(clamp_quality("max",    "xhigh") == "xhigh");
    CHECK(clamp_quality("xhigh",  "xhigh") == "xhigh");
    CHECK(clamp_quality("low",    "xhigh") == "low");
    CHECK(clamp_quality("high",   "medium") == "medium");

    SUBCASE("a ceiling of max lifts the cap entirely") {
        CHECK(clamp_quality("max", "max") == "max");
    }
    SUBCASE("auto passes through and is left to the model") {
        CHECK(clamp_quality("auto", "low") == "auto");
    }
    SUBCASE("an unrecognized tier is passed to the API to reject") {
        // Silently rewriting a typo to the ceiling would hide the mistake and
        // bill for a render nobody asked for.
        CHECK(clamp_quality("ultra", "high") == "ultra");
        CHECK(clamp_quality("high", "ultra") == "high");
    }
}

TEST_CASE("usage_cost_usd prices a call from reported usage") {
    gptimage::ImageConfig cfg;  // $5 text in / $8 image in / $30 image out per 1M

    SUBCASE("with the input breakdown the figure is exact") {
        gptimage::ImageUsage u;
        u.input_tokens       = 1100;
        u.input_text_tokens  = 100;
        u.input_image_tokens = 1000;
        u.output_tokens      = 1756;
        u.total_tokens       = 2856;

        bool exact = false;
        const double cost = usage_cost_usd(u, cfg, &exact);
        CHECK(exact);
        // 100*5 + 1000*8 + 1756*30, per million.
        CHECK(cost == doctest::Approx(0.06118));
    }

    SUBCASE("without the breakdown the whole input bills at the text rate") {
        gptimage::ImageUsage u;
        u.input_tokens  = 1100;
        u.output_tokens = 1756;
        u.total_tokens  = 2856;

        bool exact = true;
        const double cost = usage_cost_usd(u, cfg, &exact);
        CHECK_FALSE(exact);  // understates an edit carrying reference images
        CHECK(cost == doctest::Approx(0.05818));
    }

    SUBCASE("no usage reported at all") {
        gptimage::ImageUsage u;  // every field -1
        CHECK(usage_cost_usd(u, cfg) < 0.0);
    }

    SUBCASE("a text-only generate costs output plus a little prompt") {
        gptimage::ImageUsage u;
        u.input_tokens      = 20;
        u.input_text_tokens = 20;
        u.output_tokens     = 439;   // roughly a medium 1024x1024
        u.total_tokens      = 459;
        CHECK(usage_cost_usd(u, cfg) == doctest::Approx(0.01327));
    }
}

TEST_CASE("base64_decode round-trips and rejects garbage") {
    // "hello" with and without padding, and as a data: URI.
    const auto a = gptimage::base64_decode("aGVsbG8=");
    CHECK(std::string(a.begin(), a.end()) == "hello");
    const auto b = gptimage::base64_decode("aGVsbG8");
    CHECK(std::string(b.begin(), b.end()) == "hello");
    const auto c = gptimage::base64_decode("data:image/png;base64,aGVsbG8=");
    CHECK(std::string(c.begin(), c.end()) == "hello");
    // Whitespace is tolerated; a stray character rejects the whole input rather
    // than silently producing a truncated image.
    const auto d = gptimage::base64_decode("aGVs\n bG8=");
    CHECK(std::string(d.begin(), d.end()) == "hello");
    CHECK(gptimage::base64_decode("not!valid").empty());
}
