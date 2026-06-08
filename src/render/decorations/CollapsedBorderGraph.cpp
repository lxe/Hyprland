#include "CollapsedBorderGraph.hpp"

#include <hyprutils/memory/Casts.hpp>

using namespace Render;

SCollapsedBorderGraph Render::buildCollapsedBorderGraph(const std::vector<Hyprutils::Math::CBox>& boxes, int borderPx) {
    SCollapsedBorderGraph graph;
    graph.borderPx = borderPx;

    if (borderPx <= 0)
        return graph;

    const auto BEFORE = Hyprutils::Memory::sc<double>(borderPx / 2);
    const auto BORDER = Hyprutils::Memory::sc<double>(borderPx);

    for (const auto& box : boxes) {
        if (box.width <= 0 || box.height <= 0)
            continue;

        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y - BEFORE, BORDER, box.height + BORDER});
        graph.region.add(Hyprutils::Math::CBox{box.x + box.width - BEFORE, box.y - BEFORE, BORDER, box.height + BORDER});
        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y - BEFORE, box.width + BORDER, BORDER});
        graph.region.add(Hyprutils::Math::CBox{box.x - BEFORE, box.y + box.height - BEFORE, box.width + BORDER, BORDER});
    }

    return graph;
}
