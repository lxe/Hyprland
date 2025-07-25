#include "InputCapture.hpp"

#include "managers/SeatManager.hpp"
#include "render/Renderer.hpp"
#include <fcntl.h>

CInputCaptureProtocol::CInputCaptureProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CInputCaptureProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto& RESOURCE = m_vManagers.emplace_back(makeUnique<CHyprlandInputCaptureManagerV1>(client, ver, id));

    RESOURCE->setOnDestroy([this](CHyprlandInputCaptureManagerV1* p) { std::erase_if(m_vManagers, [&](const auto& other) { return other->resource() == p->resource(); }); });

    RESOURCE->setCapture([this](CHyprlandInputCaptureManagerV1* p) {
        Debug::log(LOG, "[input-capture] Input captured");
        active = true;
        g_pHyprRenderer->ensureCursorRenderingMode();
    });
    RESOURCE->setRelease([this](CHyprlandInputCaptureManagerV1* p) {
        Debug::log(LOG, "[input-capture] Input released");
        active = false;
        g_pHyprRenderer->ensureCursorRenderingMode();
    });

    sendKeymap(g_pSeatManager->m_keyboard.lock(), RESOURCE);
}

bool CInputCaptureProtocol::isCaptured() {
    return active;
}

void CInputCaptureProtocol::updateKeymap() {
    for (const auto& manager : m_vManagers)
        sendKeymap(g_pSeatManager->m_keyboard.lock(), manager);
}

void CInputCaptureProtocol::sendMotion(const Vector2D& absolutePosition, const Vector2D& delta) {
    for (const auto& manager : m_vManagers) {
        manager->sendMotion(wl_fixed_from_double(absolutePosition.x), wl_fixed_from_double(absolutePosition.y), wl_fixed_from_double(delta.x), wl_fixed_from_double(delta.y));
    }
}

void CInputCaptureProtocol::sendKeymap(SP<IKeyboard> keyboard, const UP<CHyprlandInputCaptureManagerV1>& manager) {
    if (!keyboard)
        return;
    manager->sendKeymap(HYPRLAND_INPUT_CAPTURE_MANAGER_V1_KEYMAP_FORMAT_XKB_V1, keyboard->m_xkbKeymapFD.get(), keyboard->m_xkbKeymapString.length() + 1);
}

void CInputCaptureProtocol::forceRelease() {
    Debug::log(LOG, "[input-capture] Force Input released");
    active = false;

    for (const auto& manager : m_vManagers)
        manager->sendForceRelease();
}

void CInputCaptureProtocol::sendKey(uint32_t keyCode, hyprlandInputCaptureManagerV1KeyState state) {
    for (const auto& manager : m_vManagers)
        manager->sendKey(keyCode, state);
}

void CInputCaptureProtocol::sendModifiers(uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    for (const auto& manager : m_vManagers)
        manager->sendModifiers(mods_depressed, mods_locked, mods_locked, group);
}

void CInputCaptureProtocol::sendButton(uint32_t button, hyprlandInputCaptureManagerV1ButtonState state) {
    for (const auto& manager : m_vManagers)
        manager->sendButton(button, state);
}

void CInputCaptureProtocol::sendAxis(hyprlandInputCaptureManagerV1Axis axis, double value) {
    for (const auto& manager : m_vManagers)
        manager->sendAxis(axis, value);
}

void CInputCaptureProtocol::sendAxisValue120(hyprlandInputCaptureManagerV1Axis axis, int32_t value120) {
    for (const auto& manager : m_vManagers)
        manager->sendAxisValue120(axis, value120);
}

void CInputCaptureProtocol::sendAxisStop(hyprlandInputCaptureManagerV1Axis axis) {
    for (const auto& manager : m_vManagers)
        manager->sendAxisStop(axis);
}

void CInputCaptureProtocol::sendFrame() {
    for (const auto& manager : m_vManagers)
        manager->sendFrame();
}
