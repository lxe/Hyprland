#include <render/decorations/CollapsedBorderGraph.hpp>

#include <gtest/gtest.h>

static bool regionContains(const Hyprutils::Math::CRegion& region, const int x, const int y) {
    return region.containsPoint({static_cast<double>(x), static_cast<double>(y)});
}

TEST(Render, collapsedBorderGraphKeepsTJunctionPixelExact) {
    const std::vector<Hyprutils::Math::CBox> boxes = {
        {0, 48, 3290, 2107},
        {3290, 48, 999, 915},
        {3290, 963, 999, 1189},
        {4289, 48, 3369, 2104},
    };

    const auto graph = Render::buildCollapsedBorderGraph(boxes, 3);

    ASSERT_EQ(graph.borderPx, 3);

    for (int x = 4288; x <= 4290; ++x) {
        EXPECT_TRUE(regionContains(graph.region, x, 960));
        EXPECT_TRUE(regionContains(graph.region, x, 1000));
    }

    EXPECT_FALSE(regionContains(graph.region, 4287, 960));
    EXPECT_FALSE(regionContains(graph.region, 4291, 960));
    EXPECT_FALSE(regionContains(graph.region, 4287, 1000));
    EXPECT_FALSE(regionContains(graph.region, 4291, 1000));

    for (int y = 962; y <= 964; ++y) {
        EXPECT_TRUE(regionContains(graph.region, 4240, y));
        EXPECT_TRUE(regionContains(graph.region, 4287, y));
        EXPECT_TRUE(regionContains(graph.region, 4288, y));
        EXPECT_TRUE(regionContains(graph.region, 4290, y));
        EXPECT_FALSE(regionContains(graph.region, 4291, y));
    }

    EXPECT_FALSE(regionContains(graph.region, 4240, 961));
    EXPECT_FALSE(regionContains(graph.region, 4240, 965));
}

TEST(Render, collapsedBorderGraphRejectsEmptyAndZeroBorderInputs) {
    EXPECT_TRUE(Render::buildCollapsedBorderGraph({Hyprutils::Math::CBox{0, 0, 100, 100}}, 0).region.empty());
    EXPECT_TRUE(Render::buildCollapsedBorderGraph({Hyprutils::Math::CBox{0, 0, 0, 100}}, 3).region.empty());
    EXPECT_TRUE(Render::buildCollapsedBorderGraph({Hyprutils::Math::CBox{0, 0, 100, 0}}, 3).region.empty());
}
