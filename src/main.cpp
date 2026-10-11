#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace {
    // Each slot is one dot that can be held independently.
    enum Slot { SLOT_MAIN = 0, SLOT_LEFT = 1, SLOT_RIGHT = 2, SLOT_COUNT = 3 };

    struct DotSlot {
        Ref<CCDrawNode> dot;
        CCPoint lastPos = CCPointZero;
        bool hasLast = false;
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

    // Called when the button / screen is pressed: dot appears and STAYS.
    void showDot(int slotIndex) {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene) return;

        auto& slot = s_slots[slotIndex];

        // Already holding this dot in this scene.
        if (slot.dot) {
            if (slot.dot->getParent() == scene) return;
            slot.dot = nullptr;
        }

        const auto winSize = director->getWinSize();
        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const float shake = static_cast<float>(mod->getSettingValue<int64_t>("shakiness"));
        float xPercent = static_cast<float>(mod->getSettingValue<int64_t>("x-position"));
        const float yPercent = static_cast<float>(mod->getSettingValue<int64_t>("y-position"));
        const ccColor3B rgb = mod->getSettingValue<ccColor3B>("dot-color");
        if (opacityPct <= 0.f || radius <= 0.f) return;

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

        auto dot = CCDrawNode::create();
        if (!dot) return;
        ccColor4F color = {
            rgb.r / 255.f,
            rgb.g / 255.f,
            rgb.b / 255.f,
            std::clamp(opacityPct / 100.f, 0.f, 1.f)
        };
        drawCircle(dot, radius, color);
        dot->setPosition(p);
        dot->setZOrder(999999);
        scene->addChild(dot);
        slot.dot = dot;
    }

    // Called when the button / screen is released: dot shrinks away and is removed.
    void hideDot(int slotIndex) {
        auto& slot = s_slots[slotIndex];
        if (!slot.dot) return;
        CCDrawNode* dot = slot.dot.data();
        slot.dot = nullptr;
        if (!dot->getParent()) return;

        const float fade = static_cast<float>(Mod::get()->getSettingValue<int64_t>("fade-out")) / 1000.f;
        dot->stopAllActions();
        if (fade <= 0.f) {
            dot->removeFromParent();
            return;
        }
        dot->runAction(CCSequence::create(
            CCEaseOut::create(CCScaleTo::create(fade, 0.f), 2.f),
            CCRemoveSelf::create(),
            nullptr
        ));
    }

    // Should a jump/left/right button PRESS show a dot?
    bool shouldShowForButton(PlayerButton button) {
        auto mod = Mod::get();
        // "Only on jump" ON  -> jump only.  OFF -> jump + screen taps (taps handled below).
        if (mod->getSettingValue<bool>("only-on-jump") && button != PlayerButton::Jump) return false;
        if (mod->getSettingValue<bool>("only-when-playing") && !isPlayingLevel()) return false;
        return true;
    }
}

// Same detection method as the Click Sounds mod: the game's own button press/release events.
class $modify(ShowTapPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);
        if (shouldShowForButton(button)) showDot(slotFor(this));
        return result;
    }

    bool releaseButton(PlayerButton button) {
        bool result = PlayerObject::releaseButton(button);
        // Always allow hiding so a dot can never get stuck.
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("only-on-jump") || button == PlayerButton::Jump) {
            hideDot(slotFor(this));
        }
        return result;
    }
};

// Screen taps. Only used when "Only on jump" is OFF.
class $modify(ShowTapTouch, CCTouchDispatcher) {
    void touches(CCSet* set, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(set, event, type);
        if (!set) return;

        auto mod = Mod::get();
        if (mod->getSettingValue<bool>("only-on-jump")) {
            s_touchCount = 0;
            return;
        }

        const int n = static_cast<int>(set->count());
        if (type == 0) {            // began
            if (mod->getSettingValue<bool>("only-when-playing") && !isPlayingLevel()) return;
            // In 2-player levels the per-player button events draw the dots instead.
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
