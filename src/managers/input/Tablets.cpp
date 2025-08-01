#include "InputManager.hpp"
#include "../../desktop/Window.hpp"
#include "../../protocols/Tablet.hpp"
#include "../../devices/Tablet.hpp"
#include "../../managers/PointerManager.hpp"
#include "../../managers/SeatManager.hpp"
#include "../../protocols/PointerConstraints.hpp"

static void unfocusTool(SP<CTabletTool> tool) {
    if (!tool->getSurface())
        return;

    tool->setSurface(nullptr);
    if (tool->m_isDown)
        PROTO::tablet->up(tool);
    for (auto const& b : tool->m_buttonsDown) {
        PROTO::tablet->buttonTool(tool, b, false);
    }
    PROTO::tablet->proximityOut(tool);
}

static void focusTool(SP<CTabletTool> tool, SP<CTablet> tablet, SP<CWLSurfaceResource> surf) {
    if (tool->getSurface() == surf || !surf)
        return;

    if (tool->getSurface() && tool->getSurface() != surf)
        unfocusTool(tool);

    tool->setSurface(surf);
    PROTO::tablet->proximityIn(tool, tablet, surf);
    if (tool->m_isDown)
        PROTO::tablet->down(tool);
    for (auto const& b : tool->m_buttonsDown) {
        PROTO::tablet->buttonTool(tool, b, true);
    }
}

static void refocusTablet(SP<CTablet> tab, SP<CTabletTool> tool, bool motion = false) {
    const auto LASTHLSURFACE = CWLSurface::fromResource(g_pSeatManager->m_state.pointerFocus.lock());

    if (!LASTHLSURFACE || !tool->m_active) {
        if (tool->getSurface())
            unfocusTool(tool);

        return;
    }

    const auto BOX = LASTHLSURFACE->getSurfaceBoxGlobal();

    if (!BOX.has_value()) {
        if (tool->getSurface())
            unfocusTool(tool);

        return;
    }

    const auto CURSORPOS = g_pInputManager->getMouseCoordsInternal();

    focusTool(tool, tab, g_pSeatManager->m_state.pointerFocus.lock());

    if (!motion)
        return;

    if (LASTHLSURFACE->constraint() && tool->aq()->type != Aquamarine::ITabletTool::AQ_TABLET_TOOL_TYPE_MOUSE) {
        // cursor logic will completely break here as the cursor will be locked.
        // let's just "map" the desired position to the constraint area.

        Vector2D local;

        // yes, this technically ignores any regions set by the app. Too bad!
        if (LASTHLSURFACE->getWindow())
            local = tool->m_absolutePos * LASTHLSURFACE->getWindow()->m_realSize->goal();
        else
            local = tool->m_absolutePos * BOX->size();

        if (LASTHLSURFACE->getWindow() && LASTHLSURFACE->getWindow()->m_isX11)
            local = local * LASTHLSURFACE->getWindow()->m_X11SurfaceScaledBy;

        PROTO::tablet->motion(tool, local);
        return;
    }

    auto local = CURSORPOS - BOX->pos();

    if (LASTHLSURFACE->getWindow() && LASTHLSURFACE->getWindow()->m_isX11)
        local = local * LASTHLSURFACE->getWindow()->m_X11SurfaceScaledBy;

    PROTO::tablet->motion(tool, local);
}

static Vector2D transformToActiveRegion(const Vector2D pos, const CBox activeArea) {
    auto newPos = pos;

    //Calculate transformations if active area is set
    if (!activeArea.empty()) {
        if (!std::isnan(pos.x))
            newPos.x = (pos.x - activeArea.x) / (activeArea.w - activeArea.x);
        if (!std::isnan(pos.y))
            newPos.y = (pos.y - activeArea.y) / (activeArea.h - activeArea.y);
    }

    return newPos;
}

void CInputManager::onTabletAxis(CTablet::SAxisEvent e) {
    const auto PTAB  = e.tablet;
    const auto PTOOL = ensureTabletToolPresent(e.tool);

    if (PTOOL->m_active && (e.updatedAxes & (CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_X | CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_Y))) {
        double   x  = (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_X) ? e.axis.x : NAN;
        double   dx = (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_X) ? e.axisDelta.x : NAN;
        double   y  = (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_Y) ? e.axis.y : NAN;
        double   dy = (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_Y) ? e.axisDelta.y : NAN;

        Vector2D delta = {std::isnan(dx) ? 0.0 : dx, std::isnan(dy) ? 0.0 : dy};

        switch (e.tool->type) {
            case Aquamarine::ITabletTool::AQ_TABLET_TOOL_TYPE_MOUSE: {
                g_pPointerManager->move(delta);
                break;
            }
            default: {
                if (!std::isnan(x))
                    PTOOL->m_absolutePos.x = x;
                if (!std::isnan(y))
                    PTOOL->m_absolutePos.y = y;

                if (PTAB->m_relativeInput)
                    g_pPointerManager->move(delta);
                else
                    g_pPointerManager->warpAbsolute(transformToActiveRegion({x, y}, PTAB->m_activeArea), PTAB);

                break;
            }
        }

        m_lastInputTouch = false;
        if (!PTOOL->m_isDown)
            simulateMouseMovement();
        refocusTablet(PTAB, PTOOL, true);
        m_lastCursorMovement.reset();
    }

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_PRESSURE)
        PROTO::tablet->pressure(PTOOL, e.pressure);

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_DISTANCE)
        PROTO::tablet->distance(PTOOL, e.distance);

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_ROTATION)
        PROTO::tablet->rotation(PTOOL, e.rotation);

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_SLIDER)
        PROTO::tablet->slider(PTOOL, e.slider);

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_WHEEL)
        PROTO::tablet->wheel(PTOOL, e.wheelDelta);

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_TILT_X)
        PTOOL->m_tilt.x = e.tilt.x;

    if (e.updatedAxes & CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_TILT_Y)
        PTOOL->m_tilt.y = e.tilt.y;

    if (e.updatedAxes & (CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_TILT_X | CTablet::eTabletToolAxes::HID_TABLET_TOOL_AXIS_TILT_Y))
        PROTO::tablet->tilt(PTOOL, PTOOL->m_tilt);
}

void CInputManager::onTabletTip(CTablet::STipEvent e) {
    const auto PTAB  = e.tablet;
    const auto PTOOL = ensureTabletToolPresent(e.tool);
    const auto POS   = e.tip;

    if (PTAB->m_relativeInput)
        g_pPointerManager->move({0, 0});
    else
        g_pPointerManager->warpAbsolute(transformToActiveRegion(POS, PTAB->m_activeArea), PTAB);

    if (e.in)
        refocus();

    refocusTablet(PTAB, PTOOL, true);

    if (e.in)
        PROTO::tablet->down(PTOOL);
    else
        PROTO::tablet->up(PTOOL);

    PTOOL->m_isDown = e.in;
}

void CInputManager::onTabletButton(CTablet::SButtonEvent e) {
    const auto PTOOL = ensureTabletToolPresent(e.tool);

    if (e.down)
        refocus();

    PROTO::tablet->buttonTool(PTOOL, e.button, e.down);

    if (e.down)
        PTOOL->m_buttonsDown.push_back(e.button);
    else
        std::erase(PTOOL->m_buttonsDown, e.button);
}

void CInputManager::onTabletProximity(CTablet::SProximityEvent e) {
    const auto PTAB  = e.tablet;
    const auto PTOOL = ensureTabletToolPresent(e.tool);

    PTOOL->m_active = e.in;

    if (!e.in) {
        if (PTOOL->getSurface())
            unfocusTool(PTOOL);
    } else {
        simulateMouseMovement();
        refocusTablet(PTAB, PTOOL);
    }
}

void CInputManager::newTablet(SP<Aquamarine::ITablet> pDevice) {
    const auto PNEWTABLET = m_tablets.emplace_back(CTablet::create(pDevice));
    m_hids.emplace_back(PNEWTABLET);

    try {
        PNEWTABLET->m_hlName = g_pInputManager->getNameForNewDevice(pDevice->getName());
    } catch (std::exception& e) {
        Debug::log(ERR, "Tablet had no name???"); // logic error
    }

    g_pPointerManager->attachTablet(PNEWTABLET);

    PNEWTABLET->m_events.destroy.listenStatic([this, tablet = PNEWTABLET.get()] {
        auto TABLET = tablet->m_self;
        destroyTablet(TABLET.lock());
    });

    setTabletConfigs();
}

SP<CTabletTool> CInputManager::ensureTabletToolPresent(SP<Aquamarine::ITabletTool> pTool) {

    for (auto const& t : m_tabletTools) {
        if (t->aq() == pTool)
            return t;
    }

    const auto PTOOL = m_tabletTools.emplace_back(CTabletTool::create(pTool));
    m_hids.emplace_back(PTOOL);

    try {
        PTOOL->m_hlName = g_pInputManager->getNameForNewDevice(pTool->getName());
    } catch (std::exception& e) {
        Debug::log(ERR, "Tablet had no name???"); // logic error
    }

    PTOOL->m_events.destroy.listenStatic([this, tool = PTOOL.get()] {
        auto TOOL = tool->m_self;
        destroyTabletTool(TOOL.lock());
    });

    return PTOOL;
}

void CInputManager::newTabletPad(SP<Aquamarine::ITabletPad> pDevice) {
    const auto PNEWPAD = m_tabletPads.emplace_back(CTabletPad::create(pDevice));
    m_hids.emplace_back(PNEWPAD);

    try {
        PNEWPAD->m_hlName = g_pInputManager->getNameForNewDevice(pDevice->getName());
    } catch (std::exception& e) {
        Debug::log(ERR, "Pad had no name???"); // logic error
    }

    PNEWPAD->m_events.destroy.listenStatic([this, pad = PNEWPAD.get()] {
        auto PAD = pad->m_self;
        destroyTabletPad(PAD.lock());
    });

    PNEWPAD->m_padEvents.button.listenStatic([pad = PNEWPAD.get()](const CTabletPad::SButtonEvent& event) {
        const auto PPAD = pad->m_self.lock();

        PROTO::tablet->mode(PPAD, 0, event.mode, event.timeMs);
        PROTO::tablet->buttonPad(PPAD, event.button, event.timeMs, event.down);
    });

    PNEWPAD->m_padEvents.strip.listenStatic([pad = PNEWPAD.get()](const CTabletPad::SStripEvent& event) {
        const auto PPAD = pad->m_self.lock();
        PROTO::tablet->strip(PPAD, event.strip, event.position, event.finger, event.timeMs);
    });

    PNEWPAD->m_padEvents.ring.listenStatic([pad = PNEWPAD.get()](const CTabletPad::SRingEvent& event) {
        const auto PPAD = pad->m_self.lock();
        PROTO::tablet->ring(PPAD, event.ring, event.position, event.finger, event.timeMs);
    });

    PNEWPAD->m_padEvents.attach.listenStatic([pad = PNEWPAD.get()](const SP<CTabletTool>& tool) { pad->m_parent = tool; });
}
