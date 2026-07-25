#include "chronos/api/http_server.h"

#include <gtest/gtest.h>

namespace chronos::api {
namespace {

TEST(HttpParsing, ParsesRequestLineHeadersAndBody) {
    const auto request = detail::parse_request(
        "POST /api/jobs?limit=5&state=Queued HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        R"({"name":"x"})");

    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, "POST");
    EXPECT_EQ(request->path, "/api/jobs");
    EXPECT_EQ(request->query_param("limit"), "5");
    EXPECT_EQ(request->query_param("state"), "Queued");
    EXPECT_FALSE(request->query_param("missing").has_value());
    EXPECT_EQ(request->headers.at("content-type"), "application/json");
    EXPECT_EQ(request->body, R"({"name":"x"})");
}

TEST(HttpParsing, DecodesUrlEncoding) {
    EXPECT_EQ(detail::url_decode("a%20b+c%2Fd"), "a b c/d");
    EXPECT_EQ(detail::url_decode("plain"), "plain");
    EXPECT_EQ(detail::url_decode("bad%2"), "bad%2");  // Truncated: literal.

    const auto request =
        detail::parse_request("GET /api/jobs?name=hello%20world HTTP/1.1\r\n\r\n");
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->query_param("name"), "hello world");
}

TEST(HttpParsing, RejectsGarbage) {
    EXPECT_FALSE(detail::parse_request("not http at all").has_value());
    EXPECT_FALSE(detail::parse_request("GARBAGE\r\n\r\n").has_value());
}

TEST(Router, MatchesLiteralAndCapturePatterns) {
    Router router;
    router.add("GET", "/api/jobs",
               [](const HttpRequest&) { return HttpResponse::text("list"); });
    router.add("GET", "/api/jobs/{id}",
               [](const HttpRequest& r) {
                   return HttpResponse::text("job " + r.path_params.at("id"));
               });
    router.add("POST", "/api/jobs/{id}/cancel",
               [](const HttpRequest& r) {
                   return HttpResponse::text("cancel " + r.path_params.at("id"));
               });

    HttpRequest request;
    request.method = "GET";
    request.path = "/api/jobs";
    EXPECT_EQ(router.route(request)->body, "list");

    request.path = "/api/jobs/42";
    EXPECT_EQ(router.route(request)->body, "job 42");

    request.method = "POST";
    request.path = "/api/jobs/7/cancel";
    EXPECT_EQ(router.route(request)->body, "cancel 7");

    request.path = "/api/nope";
    EXPECT_FALSE(router.route(request).has_value());

    request.method = "DELETE";  // Right path, wrong method.
    request.path = "/api/jobs/7/cancel";
    EXPECT_FALSE(router.route(request).has_value());
}

TEST(Router, TrailingSlashesNormalize) {
    Router router;
    router.add("GET", "/api/state",
               [](const HttpRequest&) { return HttpResponse::text("ok"); });
    HttpRequest request;
    request.method = "GET";
    request.path = "/api/state/";
    EXPECT_TRUE(router.route(request).has_value());
}

}  // namespace
}  // namespace chronos::api
