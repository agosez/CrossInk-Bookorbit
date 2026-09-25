#include <gtest/gtest.h>

#include "HttpRedirectPolicy.h"
#include "UrlUtils.h"

namespace {
HttpRedirectPolicy::Url parse(const char* url) {
  HttpRedirectPolicy::Url parsed;
  EXPECT_TRUE(HttpRedirectPolicy::parseUrl(url, parsed));
  return parsed;
}
}  // namespace

TEST(HttpRedirectPolicy, SendsCredentialsToTheConfiguredServerOrigin) {
  const auto server = parse("https://catalog.example.test/opds");
  const auto download = parse("https://catalog.example.test/books/title.epub");

  EXPECT_TRUE(HttpRedirectPolicy::shouldSendAuthorization(download, server, true));
}

TEST(HttpRedirectPolicy, MatchesHostnameWithoutCaseSensitivity) {
  const auto server = parse("https://Catalog.Example.Test/opds");
  const auto download = parse("https://catalog.example.test/books/title.epub");

  EXPECT_TRUE(HttpRedirectPolicy::shouldSendAuthorization(download, server, true));
}

TEST(HttpRedirectPolicy, TreatsExplicitDefaultPortAsTheSameOrigin) {
  const auto server = parse("https://catalog.example.test:443/opds");
  const auto download = parse("https://catalog.example.test/books/title.epub");

  EXPECT_TRUE(HttpRedirectPolicy::shouldSendAuthorization(download, server, true));
}

TEST(HttpRedirectPolicy, NormalizesSchemeLessServerOriginBeforeScopingCredentials) {
  const std::string authorizationOrigin = UrlUtils::ensureProtocol("catalog.example.test/opds");
  const auto server = parse(authorizationOrigin.c_str());
  const auto download = parse("http://catalog.example.test/books/title.epub");

  EXPECT_TRUE(HttpRedirectPolicy::shouldSendAuthorization(download, server, true));
}

TEST(HttpRedirectPolicy, AllowsCrossOriginHttpsRedirectWithoutCredentials) {
  const auto server = parse("https://mayberry.pub/opds");
  const auto branch = parse("https://reader.branch.pub/download/token");

  EXPECT_TRUE(HttpRedirectPolicy::isAllowedRedirect(server, branch));
  EXPECT_FALSE(HttpRedirectPolicy::shouldSendAuthorization(branch, server, true));
}

TEST(HttpRedirectPolicy, ResolvesProtocolRelativeRedirectAcrossOriginsWithoutCredentials) {
  const auto server = parse("https://mayberry.pub/opds");
  const std::string redirectUrl =
      HttpRedirectPolicy::buildRedirectUrl("https://mayberry.pub/download", "//reader.branch.pub/download/token");
  const auto branch = parse(redirectUrl.c_str());

  EXPECT_EQ(redirectUrl, "https://reader.branch.pub/download/token");
  EXPECT_TRUE(HttpRedirectPolicy::isAllowedRedirect(server, branch));
  EXPECT_FALSE(HttpRedirectPolicy::shouldSendAuthorization(branch, server, true));
}

TEST(HttpRedirectPolicy, ResolvesQueryOnlyRedirectAgainstTheCurrentResource) {
  EXPECT_EQ(HttpRedirectPolicy::buildRedirectUrl("https://catalog.example.test/opds/books?id=old#previous",
                                                 "?token=new#response-fragment"),
            "https://catalog.example.test/opds/books?token=new");
}

TEST(HttpRedirectPolicy, RemovesFragmentsFromRedirectRequestUrls) {
  EXPECT_EQ(HttpRedirectPolicy::buildRedirectUrl("https://catalog.example.test/opds/books?id=old#previous",
                                                 "#response-fragment"),
            "https://catalog.example.test/opds/books?id=old");
}

TEST(HttpRedirectPolicy, RejectsHttpsToHttpRedirect) {
  const auto secure = parse("https://catalog.example.test/download");
  const auto insecure = parse("http://catalog.example.test/download");

  EXPECT_FALSE(HttpRedirectPolicy::isAllowedRedirect(secure, insecure));
}

TEST(HttpRedirectPolicy, DoesNotSendCredentialsToAbsoluteCrossOriginAcquisitionUrl) {
  const auto server = parse("https://catalog.example.test/opds");
  const auto acquisition = parse("https://files.example.test/books/title.epub");

  EXPECT_FALSE(HttpRedirectPolicy::shouldSendAuthorization(acquisition, server, true));
}

TEST(HttpRedirectPolicy, RejectsUnsupportedRedirectSchemes) {
  EXPECT_TRUE(HttpRedirectPolicy::buildRedirectUrl("https://catalog.example.test/opds", "ftp://files.example.test/book")
                  .empty());
}
