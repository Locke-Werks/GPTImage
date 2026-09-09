#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "job_store.hpp"
#include "tool_common.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// A trivial render: one tiny webp "image" and a caption, returned synchronously.
gptimage::JobOutput one_webp() {
    return gptimage::JobOutput{{{"QUJD", "image/webp"}}, "Generated 1 image."};
}

}  // namespace

TEST_CASE("JobStore serves a finished image and rejects bad lookups") {
    gptimage::JobStore store(900, 4);
    const std::string id = store.submit("generate", "tester", [](const std::string&) { return one_webp(); });

    auto snap = store.wait_for(id, 2s);
    REQUIRE(snap.has_value());
    CHECK(snap->status == gptimage::ImageJob::Status::Done);

    // This accessor is what the /i/ HTTP route serves from.
    auto img = store.get_image(id, 0);
    REQUIRE(img.has_value());
    CHECK(img->b64  == "QUJD");
    CHECK(img->mime == "image/webp");

    CHECK_FALSE(store.get_image(id, 1).has_value());            // index out of range
    CHECK_FALSE(store.get_image("job_missing", 0).has_value()); // unknown id
}

TEST_CASE("JobStore marks a throwing render as Error and serves no image") {
    gptimage::JobStore store(900, 4);
    const std::string id = store.submit(
        "generate", "tester",
        [](const std::string&) -> gptimage::JobOutput { throw std::runtime_error("boom"); });

    auto snap = store.wait_for(id, 2s);
    REQUIRE(snap.has_value());
    CHECK(snap->status == gptimage::ImageJob::Status::Error);
    CHECK(snap->error  == "boom");
    CHECK_FALSE(store.get_image(id, 0).has_value());
}

TEST_CASE("JobStore evicts a finished render after its TTL") {
    gptimage::JobStore store(1, 4);  // 1-second retention window
    const std::string id = store.submit("generate", "tester", [](const std::string&) { return one_webp(); });
    REQUIRE(store.wait_for(id, 2s).has_value());
    CHECK(store.get_image(id, 0).has_value());

    std::this_thread::sleep_for(1200ms);
    CHECK_FALSE(store.get_image(id, 0).has_value());  // dropped; nothing is persisted
}

TEST_CASE("the disk tier outlives the in-memory TTL") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "gptimage_test_renders";
    fs::remove_all(dir);

    const std::vector<gptimage::GeneratedImage> imgs{{"QUJD", "image/webp"}};
    const auto paths = gptimage::save_render(dir, "job_deadbeef", imgs, 0);
    REQUIRE(paths.size() == 1);
    // Named to match the hosted URL, so the route needs no index to find it.
    CHECK(paths[0].filename().string() == "job_deadbeef-0.webp");

    auto got = gptimage::load_render(dir, "job_deadbeef", 0, "webp");
    REQUIRE(got.has_value());
    CHECK(std::string(got->bytes.begin(), got->bytes.end()) == "ABC");
    CHECK(got->mime == "image/webp");

    SUBCASE("misses are misses, not throws") {
        CHECK_FALSE(gptimage::load_render(dir, "job_deadbeef", 1, "webp").has_value());
        CHECK_FALSE(gptimage::load_render(dir, "job_nothere", 0, "webp").has_value());
        CHECK_FALSE(gptimage::load_render(dir, "job_deadbeef", 0, "png").has_value());
        // An unset save_dir disables the tier rather than resolving to CWD.
        CHECK_FALSE(gptimage::load_render("", "job_deadbeef", 0, "webp").has_value());
    }
    fs::remove_all(dir);
}

TEST_CASE("saving is best-effort and never costs the caller a paid render") {
    namespace fs = std::filesystem;
    const std::vector<gptimage::GeneratedImage> imgs{{"QUJD", "image/webp"}};
    // No save_dir configured: saving is off, and that is not an error.
    CHECK(gptimage::save_render("", "job_deadbeef", imgs, 0).empty());
    // Base64 that decodes to nothing is skipped, not thrown on.
    const auto dir = fs::temp_directory_path() / "gptimage_test_renders_bad";
    fs::remove_all(dir);
    const std::vector<gptimage::GeneratedImage> junk{{"!!!not base64!!!", "image/png"}};
    CHECK(gptimage::save_render(dir, "job_deadbeef", junk, 0).empty());
    fs::remove_all(dir);
}

TEST_CASE("the file cap prunes oldest so a loop cannot fill the disk") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "gptimage_test_renders_cap";
    fs::remove_all(dir);

    const std::vector<gptimage::GeneratedImage> imgs{{"QUJD", "image/webp"}};
    for (int i = 0; i < 5; ++i) {
        gptimage::save_render(dir, "job_0000000000000000000000" + std::to_string(i),
                              imgs, /*max_files=*/3);
        // Distinct mtimes, so "oldest" is well defined on a coarse clock.
        std::this_thread::sleep_for(30ms);
    }
    int count = 0;
    for (const auto& e : fs::directory_iterator(dir)) { (void)e; ++count; }
    CHECK(count == 3);
    // The survivors are the newest three.
    CHECK(fs::exists(dir / "job_00000000000000000000004-0.webp"));
    CHECK_FALSE(fs::exists(dir / "job_00000000000000000000000-0.webp"));
    fs::remove_all(dir);
}

TEST_CASE("render_job embeds a hosted markdown link only when a base URL is set") {
    gptimage::ImageJob job;
    job.id      = "job_deadbeef";
    job.status  = gptimage::ImageJob::Status::Done;
    job.caption = "Generated 1 image.";
    job.images.push_back({"QUJD", "image/webp"});

    SUBCASE("hosted: inline block plus a markdown link for the chat body") {
        auto res = gptimage::render_job(job, "job_deadbeef", "https://h.example");
        const auto& content = res.at("content");
        REQUIRE(content.is_array());
        CHECK(content[0].at("type").get<std::string>() == "image");      // fallback block
        CHECK(content[0].at("mimeType").get<std::string>() == "image/webp");

        bool found_link = false;
        for (const auto& c : content) {
            if (c.at("type").get<std::string>() == "text" &&
                c.at("text").get<std::string>().find(
                    "https://h.example/i/job_deadbeef-0.webp") != std::string::npos) {
                found_link = true;
            }
        }
        CHECK(found_link);
    }

    SUBCASE("local/stdio: no base URL, so no hosted link") {
        auto res = gptimage::render_job(job, "job_deadbeef", "");
        for (const auto& c : res.at("content")) {
            if (c.at("type").get<std::string>() == "text") {
                CHECK(c.at("text").get<std::string>().find("/i/") == std::string::npos);
            }
        }
    }
}

TEST_CASE("render_job reports pending and unknown jobs") {
    SUBCASE("unknown id is an error result naming the id") {
        auto res = gptimage::render_job(std::nullopt, "job_gone", "https://h.example");
        CHECK(res.value("isError", false) == true);
        CHECK(res.at("content")[0].at("text").get<std::string>().find("job_gone") !=
              std::string::npos);
    }
    SUBCASE("pending id tells the caller to poll gptimage_result again") {
        gptimage::ImageJob job;
        job.status = gptimage::ImageJob::Status::Pending;
        auto res = gptimage::render_job(job, "job_wait", "");
        const std::string text = res.at("content")[0].at("text").get<std::string>();
        CHECK(text.find("pending") != std::string::npos);
        CHECK(text.find("job_wait") != std::string::npos);
    }
}
