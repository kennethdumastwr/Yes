#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace {
    // Each slot is one dot that can be held independently.
    enum Slot { SLOT_MAIN = 0, SLOT_LEFT = 1, SLOT_RIGHT = 2, SLOT_COUNT = 3 };

    // Dots are created ONCE and reused (shown/hidden), so pressing never
    // allocates or re-sorts the scene - that is what caused random hitches.
    struct DotSlot {
        Ref<CCDrawNode> dot;
        CCPoint lastPos = CCPointZero;
        bool hasLast = false;
        bool held = false;
        float drawnRadius = -1.f;
        ccColor4F drawnColor = {-1.f, -1.f, -1.f, -1.f};
    };

    std::array<DotSlot, SLOT_COUNT> s_slots;
    int s_touchCount = 0;

    float randomOffset(float maxOffset) {
        if (maxOffset <= 0.f) return 0.f;
        return ((static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.f - 1.f) * maxOffset;
    }

    // True while the pause menu is open on top of the current scene.
    bool isPaused() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return false;
        for (CCNode* child : scene->getChildrenExt()) {
            if (typeinfo_cast<PauseLayer*>(child)) return true;
        }
        return false;
    }

    // Inside a level (or an editor playtest) and not paused.
    bool isPlayingLevel() {
        if (!PlayLayer::get() && !LevelEditorLayer::get()) return false;
        return !isPaused();
    }

    // True when the setting is on AND the current level has "2-Player mode" enabled.
    bool twoPlayerActive() {
        if (!Mod::get()->getSettingValue<bool>("two-player-mode")) return false;
        GJBaseGameLayer* layer = nullptr;
        if (auto pl = PlayLayer::get()) layer = pl;
        else if (auto ed = LevelEditorLayer::get()) layer = ed;
        if (!layer || !layer->m_levelSettings) return false;
        return layer->m_levelSettings->m_twoPlayerMode;
    }

    // Which dot does this player's event control?
    // Player 1 = left dot, Player 2 = right dot (unless swapped).
    int slotFor(PlayerObject* player) {
        if (!twoPlayerActive()) return SLOT_MAIN;
        bool right = player->m_isSecondPlayer;
        if (Mod::get()->getSettingValue<bool>("swap-players")) right = !right;
        return right ? SLOT_RIGHT : SLOT_LEFT;
    }

    // Draws a real filled circle (drawDot was rendering as a square).
    void drawCircle(CCDrawNode* node, float radius, const ccColor4F& color) {
        constexpr int kSegments = 40;
        constexpr float kPi = 3.14159265358979f;
        CCPoint verts[kSegments];
        for (int i = 0; i < kSegments; ++i) {
            float a = (2.f * kPi * static_cast<float>(i)) / static_cast<float>(kSegments);
            verts[i] = ccp(std::cos(a) * radius, std::sin(a) * radius);
        }
        node->drawPolygon(verts, kSegments, color, 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
    }

    bool sameColor(const ccColor4F& a, const ccColor4F& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    // Creates the dot if needed, attaches it to the current scene, and redraws
    // it only if size/color changed. Leaves it hidden. Returns null if disabled.
    CCDrawNode* prepareDot(int slotIndex, CCScene* scene) {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled") || !scene) return nullptr;

        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const ccColor3B rgb = mod->getSettingValue<ccColor3B>("dot-color");
        if (opacityPct <= 0.f || radius <= 0.f) return nullptr;

        ccColor4F color = {
            rgb.r / 255.f,
            rgb.g / 255.f,
            rgb.b / 255.f,
            std::clamp(opacityPct / 100.f, 0.f, 1.f)
        };

        auto& slot = s_slots[slotIndex];
        if (!slot.dot) {
            auto node = CCDrawNode::create();
            if (!node) return nullptr;
            node->setVisible(false);
            slot.dot = node;
            slot.drawnRadius = -1.f;
        }

        CCDrawNode* dot = slot.dot.data();
        if (dot->getParent() != scene) {
            dot->removeFromParent();
            scene->addChild(dot, 999999);
        }
        if (slot.drawnRadius != radius || !sameColor(slot.drawnColor, color)) {
            dot->clear();
            drawCircle(dot, radius, color);
            slot.drawnRadius = radius;
            slot.drawnColor = color;
        }
        return dot;
    }

    // Warm everything up when a level starts so the FIRST press is as fast as the rest.
    void prewarmDots() {
        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene) return;
        prepareDot(SLOT_MAIN, scene);
        if (twoPlayerActive()) {
            prepareDot(SLOT_LEFT, scene);
            prepareDot(SLOT_RIGHT, scene);
        }
    }

    // Called when the button / screen is pressed: dot appears INSTANTLY and STAYS.
    void showDot(int slotIndex) {
        auto mod = Mod::get();
        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene) return;

        auto& slot = s_slots[slotIndex];

        // Already holding this dot in this scene.
        if (slot.held && slot.dot && slot.dot->getParent() == scene) return;

        CCDrawNode* dot = prepareDot(slotIndex, scene);
        if (!dot) return;

        const auto winSize = director->getWinSize();
        const float radius = slot.drawnRadius;
        const float shake = static_cast<float>(mod->getSettingValue<int64_t>("shakiness"));
        float xPercent = static_cast<float>(mod->getSettingValue<int64_t>("x-position"));
        const float yPercent = static_cast<float>(mod->getSettingValue<int64_t>("y-position"));

        // The left dot (Player 1 in two-player mode) is the mirrored X position.
        if (slotIndex == SLOT_LEFT) xPercent = 100.f - xPercent;

        // X/Y are percentages of the screen. Cocos origin is bottom-left.
        CCPoint p = ccp(winSize.width * std::clamp(xPercent, 0.f, 100.f) / 100.f,
                        winSize.height * std::clamp(yPercent, 0.f, 100.f) / 100.f);

        if (shake > 0.f) {
            p.x += randomOffset(shake);
            p.y += randomOffset(shake);
            p.x = std::clamp(p.x, radius, std::max(radius, winSize.width - radius));
            p.y = std::clamp(p.y, radius, std::max(radius, winSize.height - radius));

            // Avoid landing on the exact same spot as the previous tap.
            if (slot.hasLast && std::abs(p.x - slot.lastPos.x) < 0.01f &&
                std::abs(p.y - slot.lastPos.y) < 0.01f) {
                p.x = std::clamp(p.x + std::max(1.f, shake * 0.1f), radius,
                                 std::max(radius, winSize.width - radius));
            }
        }
        slot.lastPos = p;
        slot.hasLast = true;

        dot->stopAllActions();
        dot->setScale(1.f);
        dot->setPosition(p);
        dot->setVisible(true);
        slot.held = true;
    }

    // Called when the button / screen is released.
    // Frame-Perfect Mode: the dot vanishes on the very same frame (no animation).
    void hideDot(int slotIndex) {
        auto& slot = s_slots[slotIndex];
        if (!slot.held) return;
        slot.held = false;
        if (!slot.dot) return;

        CCDrawNode* dot = slot.dot.data();
        dot->stopAllActions();

        auto mod = Mod::get();
        const float fade = static_cast<float>(mod->getSettingValue<int64_t>("fade-out")) / 1000.f;
        if (mod->getSettingValue<bool>("frame-perfect") || fade <= 0.f) {
            dot->setVisible(false);
            return;
        }
        dot->runAction(CCSequence::create(
            CCEaseOut::create(CCScaleTo::create(fade, 0.f), 2.f),
            CCHide::create(),
            nullptr
        ));
    }

    // ---------- Real Show Tap (menus + pause screen) ----------
    // Draws a dot exactly where each finger touches, follows it while it moves,
    // and removes it on release. Supports multi-touch.
    std::unordered_map<int, Ref<CCDrawNode>> s_realDots;

    // Not playing a level: menus, pause screen, etc. (editor excluded so it never clashes).
    bool realTapActive() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled") || !mod->getSettingValue<bool>("real-show-tap")) return false;
        if (LevelEditorLayer::get()) return false;
        if (PlayLayer::get()) return isPaused();
        return true;
    }

    void removeRealDot(int id) {
        auto it = s_realDots.find(id);
        if (it == s_realDots.end()) return;
        Ref<CCDrawNode> dot = it->second;
        s_realDots.erase(it);
        if (!dot || !dot->getParent()) return;

        auto mod = Mod::get();
        const float fade = static_cast<float>(mod->getSettingValue<int64_t>("fade-out")) / 1000.f;
        if (mod->getSettingValue<bool>("frame-perfect") || fade <= 0.f) {
            dot->removeFromParent();
            return;
        }
        dot->runAction(CCSequence::create(
            CCEaseOut::create(CCScaleTo::create(fade, 0.f), 2.f),
            CCRemoveSelf::create(),
            nullptr
        ));
    }

    void createRealDot(int id, const CCPoint& pos) {
        removeRealDot(id);

        auto mod = Mod::get();
        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene) return;

        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const ccColor3B rgb = mod->getSettingValue<ccColor3B>("dot-color");
        if (opacityPct <= 0.f || radius <= 0.f) return;

        auto dot = CCDrawNode::create();
        if (!dot) return;
        ccColor4F color = {
            rgb.r / 255.f,
            rgb.g / 255.f,
            rgb.b / 255.f,
            std::clamp(opacityPct / 100.f, 0.f, 1.f)
        };
        drawCircle(dot, radius, color);
        dot->setPosition(pos);
        scene->addChild(dot, 999999);
        s_realDots[id] = dot;
    }

    // `active` = real show tap is allowed right now. Ended/cancelled touches are ALWAYS
    // processed so a dot can never get stuck (e.g. you tap "Resume" on the pause menu).
    void updateRealDots(CCSet* set, unsigned int type, bool active) {
        for (auto it = set->begin(); it != set->end(); ++it) {
            auto touch = typeinfo_cast<CCTouch*>(*it);
            if (!touch) continue;
            const int id = touch->getID();
            if (type == 0) {                 // began
                if (active) createRealDot(id, touch->getLocation());
            } else if (type == 1) {          // moved
                auto found = s_realDots.find(id);
                if (found != s_realDots.end() && found->second) {
                    found->second->setPosition(touch->getLocation());
                }
            } else {                         // ended / cancelled
                removeRealDot(id);
            }
        }
    }

    // Should a jump/left/right button PRESS show a dot?
    bool shouldShowForButton(PlayerButton button) {
        auto mod = Mod::get();
        // "Only on jump" ON  -> jump only.  OFF -> jump + screen taps (taps handled below).
        if (mod->getSettingValue<bool>("only-on-jump") && button != PlayerButton::Jump) return false;
        if (mod->getSettingValue<bool>("only-when-playing") && !isPlayingLevel()) return false;
        return true;
    }

    // ---------- Alternate clicking mode (hold duration + air time) ----------
    // Instead of counting clicks over a second, every click is judged by:
    //   Hold Duration = how long the previous click was held (press -> release)
    //   Air Time      = how long the button was up before this press (release -> press)
    // If BOTH are at or below the limits for the target CPS, alternating turns on
    // immediately (it only needs one click + release + the next click).
    // Limits are in milliseconds for CPS 2..16 (edit these numbers to tune the feel).
    //                                  CPS: 2    3    4    5    6    7   8   9   10  11  12  13  14  15  16
    constexpr double kHoldMs[15] = {          300, 200, 150, 120, 100, 86, 75, 67, 60, 55, 50, 46, 43, 40, 38 };
    constexpr double kAirMs[15]  = {          300, 200, 150, 120, 100, 86, 75, 67, 60, 55, 50, 46, 43, 40, 38 };

    using Clock = std::chrono::steady_clock;

    struct HeldClick {
        int slot;
        Clock::time_point start;
    };
    std::deque<HeldClick> s_heldClicks;           // clicks currently held (FIFO)
    Clock::time_point s_lastPress = Clock::now();
    Clock::time_point s_lastRelease = Clock::now();
    bool s_hasCycle = false;                      // we have a finished click (hold + release time)
    double s_lastHoldMs = 0.0;                    // hold duration of the last finished click
    bool s_altActive = false;
    bool s_altRight = true;                       // next alternating click goes to the right dot

    double secondsBetween(Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double>(a - b).count();
    }

    // Single-player press (not two-player mode).
    void handleSinglePress() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("alt-click-mode")) {
            showDot(SLOT_MAIN);
            return;
        }

        const auto now = Clock::now();
        // Dual mode pushes Player 1 AND Player 2 for one click: count it once.
        if (secondsBetween(now, s_lastPress) < 0.002 && !s_heldClicks.empty()) return;
        s_lastPress = now;

        int target = static_cast<int>(mod->getSettingValue<int64_t>("alt-click-cps"));
        target = std::clamp(target, 2, 16);

        bool qualifies = false;
        if (s_hasCycle) {
            const double airMs = secondsBetween(now, s_lastRelease) * 1000.0;
            qualifies = s_lastHoldMs <= kHoldMs[target - 2] && airMs <= kAirMs[target - 2];
        }

        int slot = SLOT_MAIN;
        if (qualifies) {
            if (!s_altActive) {
                s_altActive = true;
                s_altRight = false;   // previous click was the normal (right-side) dot, so go LEFT now
            }
            slot = s_altRight ? SLOT_RIGHT : SLOT_LEFT;
            s_altRight = !s_altRight;
        } else {
            s_altActive = false;
            s_altRight = true;
        }

        showDot(slot);
        s_heldClicks.push_back({slot, now});
        while (s_heldClicks.size() > 8) s_heldClicks.pop_front();
    }

    // Single-player release (not two-player mode).
    void handleSingleRelease() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("alt-click-mode")) {
            hideDot(SLOT_MAIN);
            if (!s_heldClicks.empty()) {
                s_heldClicks.clear();
                hideDot(SLOT_LEFT);
                hideDot(SLOT_RIGHT);
            }
            return;
        }

        const auto now = Clock::now();
        // Ignore the duplicate release from Player 2 in dual mode.
        if (secondsBetween(now, s_lastRelease) < 0.002) return;

        if (s_heldClicks.empty()) {   // safety: never leave a dot stuck
            s_lastRelease = now;
            hideDot(SLOT_MAIN);
            hideDot(SLOT_LEFT);
            hideDot(SLOT_RIGHT);
            return;
        }

        const HeldClick click = s_heldClicks.front();
        s_heldClicks.pop_front();

        s_lastHoldMs = secondsBetween(now, click.start) * 1000.0;   // Hold Duration
        s_lastRelease = now;                                        // start of the Air Time
        s_hasCycle = true;

        hideDot(click.slot);
    }
}

// Same detection method as the Click Sounds mod: the game's own button press/release events.
class $modify(ShowTapPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);
        if (shouldShowForButton(button)) {
            const int slot = slotFor(this);
            if (slot != SLOT_MAIN) showDot(slot);   // two-player mode: per-player dot
            else handleSinglePress();               // single dot / alternate clicking
        }
        return result;
    }

    bool releaseButton(PlayerButton button) {
        bool result = PlayerObject::releaseButton(button);
        // Always allow hiding so a dot can never get stuck.
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("only-on-jump") || button == PlayerButton::Jump) {
            const int slot = slotFor(this);
            if (slot != SLOT_MAIN) hideDot(slot);
            else handleSingleRelease();
        }
        return result;
    }
};

// Screen taps. Only used when "Only on jump" is OFF.
class $modify(ShowTapTouch, CCTouchDispatcher) {
    void touches(CCSet* set, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(set, event, type);
        if (!set) return;

        // Real Show Tap: dot follows your finger in menus / pause screen.
        const bool realActive = realTapActive();
        if (realActive || !s_realDots.empty()) updateRealDots(set, type, realActive);
        if (realActive) return;

        auto mod = Mod::get();
        if (mod->getSettingValue<bool>("only-on-jump")) {
            s_touchCount = 0;
            return;
        }

        const int n = static_cast<int>(set->count());
        if (type == 0) {            // began
            if (mod->getSettingValue<bool>("only-when-playing") && !isPlayingLevel()) return;
            // While actually playing a level, the game's own jump events draw the dot.
            // (A raw touch arrives BEFORE the game processes the jump, which looked early.)
            if (PlayLayer::get() && !isPaused()) return;
            if (twoPlayerActive()) return;
            s_touchCount += n;
            showDot(SLOT_MAIN);
        } else if (type == 2 || type == 3) {   // ended / cancelled
            s_touchCount = std::max(0, s_touchCount - n);
            if (s_touchCount == 0) hideDot(SLOT_MAIN);
        }
    }
};

// "Mod menu": a button on the pause screen that opens the Show Tap settings.
class $modify(ShowTapPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (!Mod::get()->getSettingValue<bool>("settings-button")) return;

        auto spr = ButtonSprite::create("Show Tap");
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(ShowTapPause::onShowTapSettings)
        );
        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({62.f, 24.f});
        this->addChild(menu);
    }

    void onShowTapSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

// Warm up the dots at level start / respawn so the first press has zero setup cost.
class $modify(ShowTapPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        prewarmDots();
    }
};
