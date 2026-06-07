#include "CHyprBorderDecoration.hpp"
#include "CollapsedBorderGraph.hpp"
#include "../../Compositor.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../layout/target/Target.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../pass/BorderPassElement.hpp"
#include "../pass/RectPassElement.hpp"
#include "../Renderer.hpp"
#include <cmath>
#include <vector>

static bool rangesOverlap(const double startA, const double endA, const double startB, const double endB) {
    return std::min(endA, endB) - std::max(startA, startB) > 1.0;
}

static bool edgesTouch(const double edgeA, const double edgeB) {
    return std::abs(edgeA - edgeB) < 1.0;
}

static bool windowDrawsBorder(PHLWINDOW pWindow) {
    return validMapped(pWindow) && !pWindow->m_X11DoesntWantBorders && pWindow->getRealBorderSize() > 0 && pWindow->m_ruleApplicator->decorate().valueOrDefault();
}

static bool canCollapseBorders(PHLWINDOW pWindow) {
    return windowDrawsBorder(pWindow) && !pWindow->m_isFloating && pWindow->m_fullscreenState.internal == FSMODE_NONE;
}

static bool borderCollapseEnabled() {
    static auto PBORDERCOLLAPSE = CConfigValue<Config::INTEGER>("general:border_collapse");
    return *PBORDERCOLLAPSE;
}

static double logicalBorderSize(PHLWINDOW pWindow) {
    const auto BORDERSIZE = pWindow->getRealBorderSize();

    if (BORDERSIZE <= 0)
        return 0.0;

    const auto PMONITOR = pWindow->m_monitor.lock();
    const auto SCALE    = PMONITOR ? PMONITOR->m_scale : 1.0;

    if (SCALE <= 0.0)
        return sc<double>(BORDERSIZE);

    return std::round(BORDERSIZE * SCALE) / SCALE;
}

static bool edgesShareReservedBorder(PHLWINDOW pWindow, PHLWINDOW pOtherWindow, const double edgeA, const double edgeB) {
    return std::abs(edgeA - edgeB) <= std::max(logicalBorderSize(pWindow), logicalBorderSize(pOtherWindow)) + 1.0;
}

static CBox windowSurfaceBox(PHLWINDOW pWindow) {
    return {pWindow->m_realPosition->value(), pWindow->m_realSize->value()};
}

static bool boxComesFirst(const CBox& box, const CBox& otherBox) {
    if (!edgesTouch(box.y, otherBox.y))
        return box.y < otherBox.y;

    if (!edgesTouch(box.x, otherBox.x))
        return box.x < otherBox.x;

    return false;
}

static bool ownsCollapsedBorder(PHLWINDOW pWindow, PHLWINDOW pOtherWindow, const CBox& box, const CBox& otherBox) {
    if (boxComesFirst(box, otherBox))
        return true;

    if (boxComesFirst(otherBox, box))
        return false;

    return rc<uintptr_t>(pWindow.get()) < rc<uintptr_t>(pOtherWindow.get());
}

static bool visuallyOwnsCollapsedBorder(PHLWINDOW pWindow, PHLWINDOW pOtherWindow, const CBox& box, const CBox& otherBox) {
    return ownsCollapsedBorder(pWindow, pOtherWindow, box, otherBox);
}

static SBoxExtents collapsedBorderExtents(PHLWINDOW pWindow) {
    const auto  BORDERSIZE = logicalBorderSize(pWindow);
    SBoxExtents extents    = {{BORDERSIZE, BORDERSIZE}, {BORDERSIZE, BORDERSIZE}};

    if (!borderCollapseEnabled() || !canCollapseBorders(pWindow) || !pWindow->layoutTarget())
        return extents;

    const CBox BOX    = pWindow->layoutTarget()->position();
    const auto RIGHT  = BOX.x + BOX.width;
    const auto BOTTOM = BOX.y + BOX.height;
    const auto WSID   = pWindow->workspaceID();

    for (const auto& other : g_pCompositor->m_windows) {
        if (other == pWindow || other->workspaceID() != WSID || !canCollapseBorders(other) || !other->layoutTarget())
            continue;

        const CBox OTHERBOX    = other->layoutTarget()->position();
        const auto OTHERRIGHT  = OTHERBOX.x + OTHERBOX.width;
        const auto OTHERBOTTOM = OTHERBOX.y + OTHERBOX.height;

        if (edgesTouch(BOX.x, OTHERRIGHT) && rangesOverlap(BOX.y, BOTTOM, OTHERBOX.y, OTHERBOTTOM) && !ownsCollapsedBorder(pWindow, other, BOX, OTHERBOX))
            extents.topLeft.x = 0;

        if (edgesTouch(RIGHT, OTHERBOX.x) && rangesOverlap(BOX.y, BOTTOM, OTHERBOX.y, OTHERBOTTOM) && !ownsCollapsedBorder(pWindow, other, BOX, OTHERBOX))
            extents.bottomRight.x = 0;

        if (edgesTouch(BOX.y, OTHERBOTTOM) && rangesOverlap(BOX.x, RIGHT, OTHERBOX.x, OTHERRIGHT) && !ownsCollapsedBorder(pWindow, other, BOX, OTHERBOX))
            extents.topLeft.y = 0;

        if (edgesTouch(BOTTOM, OTHERBOX.y) && rangesOverlap(BOX.x, RIGHT, OTHERBOX.x, OTHERRIGHT) && !ownsCollapsedBorder(pWindow, other, BOX, OTHERBOX))
            extents.bottomRight.y = 0;
    }

    return extents;
}

static CHyprColor borderRectColor(const Config::CGradientValueData& gradient, const float alpha) {
    auto color = gradient.m_colors.empty() ? Colors::WHITE : gradient.m_colors.front();
    return color.modifyA(color.a * alpha);
}

static bool isInBorderGraph(PHLWINDOW pWindow, PHLMONITOR pMonitor, const WORKSPACEID workspaceID) {
    return pWindow->workspaceID() == workspaceID && pWindow->m_monitor.lock() == pMonitor && pWindow->layoutTarget() && canCollapseBorders(pWindow);
}

static PHLWINDOW borderGraphLeader(PHLWINDOW pWindow, PHLMONITOR pMonitor) {
    const auto WSID = pWindow->workspaceID();

    for (const auto& other : g_pCompositor->m_windows) {
        if (!isInBorderGraph(other, pMonitor, WSID))
            continue;

        return other;
    }

    return pWindow;
}

static PHLWINDOW borderGraphStyleWindow(PHLWINDOW fallback, PHLMONITOR pMonitor) {
    const auto FOCUSED = Desktop::focusState()->window();

    if (FOCUSED && isInBorderGraph(FOCUSED, pMonitor, fallback->workspaceID()))
        return FOCUSED;

    return fallback;
}

static CBox scaledWindowBox(PHLWINDOW pWindow, PHLMONITOR pMonitor) {
    CBox box = {pWindow->m_realPosition->value(), pWindow->m_realSize->value()};
    box.translate(-pMonitor->m_position + pWindow->m_floatingOffset).scale(pMonitor->m_scale).round();
    return box;
}

static CBox scaledLayoutBox(PHLWINDOW pWindow, PHLMONITOR pMonitor) {
    CBox box = pWindow->layoutTarget()->position();
    box.translate(-pMonitor->m_position).scale(pMonitor->m_scale).round();
    return box;
}

static bool drawCollapsedBorderGraph(PHLWINDOW pWindow, PHLMONITOR pMonitor, const float alpha) {
    if (!borderCollapseEnabled() || !canCollapseBorders(pWindow) || pWindow->rounding() != 0)
        return false;

    const auto LEADER = borderGraphLeader(pWindow, pMonitor);

    if (LEADER != pWindow)
        return true;

    const auto STYLEWINDOW = borderGraphStyleWindow(pWindow, pMonitor);
    const auto SCALE       = pMonitor->m_scale;
    const auto BORDERPX    = sc<int>(std::round(STYLEWINDOW->getRealBorderSize() * SCALE));

    if (BORDERPX <= 0)
        return true;

    const auto        WSID  = pWindow->workspaceID();
    const auto        COLOR = borderRectColor(STYLEWINDOW->m_realBorderColor, alpha);
    std::vector<CBox> boxes;

    for (const auto& other : g_pCompositor->m_windows) {
        if (!isInBorderGraph(other, pMonitor, WSID))
            continue;

        const auto BOX = scaledLayoutBox(other, pMonitor);

        if (BOX.width < 1 || BOX.height < 1)
            continue;

        boxes.emplace_back(BOX);
    }

    const auto GRAPH = Render::buildCollapsedBorderGraph(boxes, BORDERPX);

    GRAPH.region.forEachRect([&](const auto& rect) {
        const CBox box = {rect.x1, rect.y1, rect.x2 - rect.x1, rect.y2 - rect.y1};
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{.box = box, .color = COLOR}));
    });

    return true;
}

static std::vector<CBox> collapsedBorderHiddenSegments(PHLWINDOW pWindow, PHLMONITOR pMonitor, const CBox& windowBox) {
    if (!borderCollapseEnabled() || !canCollapseBorders(pWindow) || !pWindow->layoutTarget())
        return {};

    std::vector<CBox> hiddenSegments;
    const CBox        SURFACEBOX = windowSurfaceBox(pWindow);
    const auto        RIGHT      = SURFACEBOX.x + SURFACEBOX.width;
    const auto        BOTTOM     = SURFACEBOX.y + SURFACEBOX.height;
    const auto        WSID       = pWindow->workspaceID();
    const auto        SCALE      = pMonitor->m_scale;
    const auto        BORDERPX   = std::round(pWindow->getRealBorderSize() * SCALE);
    const CBox        LAYOUTBOX  = pWindow->layoutTarget()->position();

    const auto        scaledCoord = [&](const double coord, const bool xAxis) {
        const auto MONITORPOS = xAxis ? pMonitor->m_position.x : pMonitor->m_position.y;
        return std::round((coord - MONITORPOS) * SCALE);
    };

    const auto addVerticalSegment = [&](const bool left, const double from, const double to) {
        const auto Y1 = scaledCoord(from, false);
        const auto Y2 = scaledCoord(to, false);

        if (Y2 <= Y1)
            return;

        hiddenSegments.emplace_back(left ? windowBox.x - BORDERPX : windowBox.x + windowBox.width, Y1, BORDERPX, Y2 - Y1);
    };

    const auto addHorizontalSegment = [&](const bool top, const double from, const double to) {
        const auto X1 = scaledCoord(from, true);
        const auto X2 = scaledCoord(to, true);

        if (X2 <= X1)
            return;

        hiddenSegments.emplace_back(X1, top ? windowBox.y - BORDERPX : windowBox.y + windowBox.height, X2 - X1, BORDERPX);
    };

    const auto addHorizontalJunctionMask = [&](const bool top, const double x) {
        hiddenSegments.emplace_back(x, top ? windowBox.y - BORDERPX : windowBox.y + windowBox.height, BORDERPX, BORDERPX);
    };

    for (const auto& other : g_pCompositor->m_windows) {
        if (other == pWindow || other->workspaceID() != WSID || !canCollapseBorders(other) || !other->layoutTarget())
            continue;

        const CBox OTHERBOX     = other->layoutTarget()->position();
        const CBox OTHERSURFACE = windowSurfaceBox(other);
        const auto OTHERRIGHT   = OTHERSURFACE.x + OTHERSURFACE.width;
        const auto OTHERBOTTOM  = OTHERSURFACE.y + OTHERSURFACE.height;
        const auto OWNSVISUALLY = visuallyOwnsCollapsedBorder(pWindow, other, LAYOUTBOX, OTHERBOX);

        if (edgesShareReservedBorder(pWindow, other, SURFACEBOX.x, OTHERRIGHT) && rangesOverlap(SURFACEBOX.y, BOTTOM, OTHERSURFACE.y, OTHERBOTTOM) && !OWNSVISUALLY) {
            addVerticalSegment(true, std::max(SURFACEBOX.y, OTHERSURFACE.y), std::min(BOTTOM, OTHERBOTTOM));
            addHorizontalJunctionMask(true, windowBox.x - BORDERPX);
            addHorizontalJunctionMask(false, windowBox.x - BORDERPX);
        }

        if (edgesShareReservedBorder(pWindow, other, RIGHT, OTHERSURFACE.x) && rangesOverlap(SURFACEBOX.y, BOTTOM, OTHERSURFACE.y, OTHERBOTTOM) && !OWNSVISUALLY) {
            addVerticalSegment(false, std::max(SURFACEBOX.y, OTHERSURFACE.y), std::min(BOTTOM, OTHERBOTTOM));
            addHorizontalJunctionMask(true, windowBox.x + windowBox.width);
            addHorizontalJunctionMask(false, windowBox.x + windowBox.width);
        }

        if (edgesShareReservedBorder(pWindow, other, SURFACEBOX.y, OTHERBOTTOM) && rangesOverlap(SURFACEBOX.x, RIGHT, OTHERSURFACE.x, OTHERRIGHT) && !OWNSVISUALLY)
            addHorizontalSegment(true, std::max(SURFACEBOX.x, OTHERSURFACE.x), std::min(RIGHT, OTHERRIGHT));

        if (edgesShareReservedBorder(pWindow, other, BOTTOM, OTHERSURFACE.y) && rangesOverlap(SURFACEBOX.x, RIGHT, OTHERSURFACE.x, OTHERRIGHT) && !OWNSVISUALLY)
            addHorizontalSegment(false, std::max(SURFACEBOX.x, OTHERSURFACE.x), std::min(RIGHT, OTHERRIGHT));
    }

    return hiddenSegments;
}

CHyprBorderDecoration::CHyprBorderDecoration(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_window(pWindow) {
    ;
}

SDecorationPositioningInfo CHyprBorderDecoration::getPositioningInfo() {
    m_extents = collapsedBorderExtents(m_window.lock());

    if (doesntWantBorders())
        m_extents = {{}, {}};

    SDecorationPositioningInfo info;
    info.priority       = 10000;
    info.policy         = DECORATION_POSITION_STICKY;
    info.desiredExtents = m_extents;
    info.reserved       = true;
    info.edges          = DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP;

    m_reportedExtents = m_extents;
    return info;
}

void CHyprBorderDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assignedGeometry = reply.assignedGeometry;
}

CBox CHyprBorderDecoration::assignedBoxGlobal() {
    CBox box = m_assignedGeometry;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT | DECORATION_EDGE_TOP, m_window));

    const auto PWORKSPACE = m_window->m_workspace;

    if (!PWORKSPACE)
        return box;

    const auto WORKSPACEOFFSET = PWORKSPACE && !m_window->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();
    return box.translate(WORKSPACEOFFSET);
}

void CHyprBorderDecoration::draw(PHLMONITOR pMonitor, float const& a) {
    if (doesntWantBorders())
        return;

    if (m_assignedGeometry.width < m_extents.topLeft.x + 1 || m_assignedGeometry.height < m_extents.topLeft.y + 1)
        return;

    CBox windowBox = scaledWindowBox(m_window.lock(), pMonitor);

    if (windowBox.width < 1 || windowBox.height < 1)
        return;

    auto       grad     = m_window->m_realBorderColor;
    const bool ANIMATED = m_window->m_borderFadeAnimationProgress->isBeingAnimated();

    if (m_window->m_borderAngleAnimationProgress->enabled()) {
        grad.m_angle += m_window->m_borderAngleAnimationProgress->value() * M_PI * 2;
        grad.m_angle = normalizeAngleRad(grad.m_angle);

        // When borderangle is animated, it is counterintuitive to fade between inactive/active gradient angles.
        // Instead we sync the angles to avoid fading between them and additionally rotating the border angle.
        if (ANIMATED)
            m_window->m_realBorderColorPrevious.m_angle = grad.m_angle;
    }

    if (drawCollapsedBorderGraph(m_window.lock(), pMonitor, a))
        return;

    int                             borderSize       = m_window->getRealBorderSize();
    const auto                      ROUNDINGBASE     = m_window->rounding();
    const auto                      ROUNDING         = ROUNDINGBASE * pMonitor->m_scale;
    const auto                      ROUNDINGPOWER    = m_window->roundingPower();
    const auto                      CORRECTIONOFFSET = (borderSize * (M_SQRT2 - 1) * std::max(2.0 - ROUNDINGPOWER, 0.0));
    const auto                      OUTERROUND       = ((ROUNDINGBASE + borderSize) - CORRECTIONOFFSET) * pMonitor->m_scale;

    CBorderPassElement::SBorderData data;
    data.box           = windowBox;
    data.grad1         = grad;
    data.round         = ROUNDING;
    data.outerRound    = OUTERROUND;
    data.roundingPower = ROUNDINGPOWER;
    data.a             = a;
    data.borderSize    = borderSize;
    data.window        = m_window;

    data.hiddenBorderSegments = collapsedBorderHiddenSegments(m_window.lock(), pMonitor, windowBox);

    if (ANIMATED) {
        data.hasGrad2 = true;
        data.grad1    = m_window->m_realBorderColorPrevious;
        data.grad2    = grad;
        data.lerp     = m_window->m_borderFadeAnimationProgress->value();
    }

    g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(data));
}

eDecorationType CHyprBorderDecoration::getDecorationType() {
    return DECORATION_BORDER;
}

void CHyprBorderDecoration::updateWindow(PHLWINDOW) {
    auto borderSize = m_window->getRealBorderSize();
    auto extents    = doesntWantBorders() ? SBoxExtents{{}, {}} : collapsedBorderExtents(m_window.lock());

    if (borderSize == m_lastBorderSize && extents == m_lastExtents)
        return;

    if (borderSize <= 0 && m_lastBorderSize <= 0 && extents == m_lastExtents)
        return;

    m_lastBorderSize = borderSize;
    m_lastExtents    = extents;

    g_pDecorationPositioner->repositionDeco(this);

    if (m_window->layoutTarget())
        m_window->layoutTarget()->recalc();
}

void CHyprBorderDecoration::damageEntire() {
    if (!validMapped(m_window) || m_window->m_fullscreenState.internal == FSMODE_FULLSCREEN)
        return;

    const auto GLOBAL_BOX = assignedBoxGlobal();
    const auto ROUNDING   = m_window->rounding();
    const auto BORDERSIZE = logicalBorderSize(m_window.lock()) + 1;

    CRegion    borderRegion(GLOBAL_BOX);
    borderRegion.subtract(GLOBAL_BOX.copy().expand(-(BORDERSIZE + ROUNDING)));
    borderRegion.expand(2); // pad

    const CBox borderExtents = borderRegion.getExtents();

    for (auto const& m : g_pCompositor->m_monitors) {
        const CBox monitorBox = {m->m_position, m->m_size};
        if (borderExtents.intersection(monitorBox).empty())
            continue;

        if (!g_pHyprRenderer->shouldRenderWindow(m_window.lock(), m)) {
            const CRegion monitorRegion(monitorBox);
            borderRegion.subtract(monitorRegion);
        }
    }

    g_pHyprRenderer->damageRegion(borderRegion);
}

eDecorationLayer CHyprBorderDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

uint64_t CHyprBorderDecoration::getDecorationFlags() {
    static auto PPARTOFWINDOW = CConfigValue<Config::INTEGER>("decoration:border_part_of_window");

    return *PPARTOFWINDOW && !doesntWantBorders() ? DECORATION_PART_OF_MAIN_WINDOW : 0;
}

std::string CHyprBorderDecoration::getDisplayName() {
    return "Border";
}

bool CHyprBorderDecoration::doesntWantBorders() {
    return m_window->m_X11DoesntWantBorders || m_window->getRealBorderSize() == 0 || !m_window->m_ruleApplicator->decorate().valueOrDefault();
}
