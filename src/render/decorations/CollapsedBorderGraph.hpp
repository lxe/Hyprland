#pragma once

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <vector>

namespace Render {
    struct SCollapsedBorderGraph {
        Hyprutils::Math::CRegion region;
        int                      borderPx = 0;
    };

    SCollapsedBorderGraph buildCollapsedBorderGraph(const std::vector<Hyprutils::Math::CBox>& boxes, int borderPx);
}
