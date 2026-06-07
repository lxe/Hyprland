#include "CollapsedBorderGraph.hpp"

using namespace Render;

SCollapsedBorderGraph Render::buildCollapsedBorderGraph(const std::vector<Hyprutils::Math::CBox>& boxes, int borderPx) {
    SCollapsedBorderGraph graph;
    graph.borderPx = borderPx;

    if (borderPx <= 0)
        return graph;

    const auto BEFORE = borderPx / 2;

    for (const auto& box : boxes) {
        if (box.width <= 0 || box.height <= 0)
            continue;

        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y - BEFORE, borderPx, box.height + borderPx});
        graph.region.add(Hyprutils::Math::CBox{box.x + box.width - BEFORE, box.y - BEFORE, borderPx, box.height + borderPx});
        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y - BEFORE, box.width + borderPx, borderPx});
        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y + box.height - BEFORE, box.width + borderPx, borderPx});
    }

    return graph;
}
