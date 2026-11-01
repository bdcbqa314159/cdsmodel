#include <cdsmodel/version.hpp>

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Section 4(b) of the upstream licence requires any application built on the
// model to display this notice. A test guards it because a silently dropped
// attribution is a licence breach, not a cosmetic regression.
TEST(Attribution, NoticeIsPresentAndNamesTheUpstreamVersion) {
  EXPECT_NE(cdsmodel::attribution_notice().find("ISDA CDS Standard Model"), std::string_view::npos);
  EXPECT_NE(cdsmodel::attribution_notice().find("Markit"), std::string_view::npos);
  EXPECT_NE(cdsmodel::attribution_notice().find(cdsmodel::upstream_version()),
            std::string_view::npos);
}

TEST(Attribution, VersionsAreNonEmpty) {
  EXPECT_FALSE(cdsmodel::version().empty());
  EXPECT_EQ(cdsmodel::upstream_version(), "1.8.3");
}

} // namespace
