#include "CHyprBorderDecoration.hpp"
#include "../../Compositor.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../layout/target/Target.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../pass/BorderPassElement.hpp"
#include "../Renderer.hpp"
#include <cmath>

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

static CBox scaledWindowBox(PHLWINDOW pWindow, PHLMONITOR pMonitor) {
    CBox box = {pWindow->m_realPosition->value(), pWindow->m_realSize->value()};
    box.translate(-pMonitor->m_position + pWindow->m_floatingOffset).scale(pMonitor->m_scale).round();
    return box;
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

    if (borderCollapseEnabled() && canCollapseBorders(m_window.lock()) && m_window->rounding() == 0)
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
