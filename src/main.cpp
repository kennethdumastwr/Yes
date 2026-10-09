#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace {
    // The dot that is currently being held (null when nothing is pressed).
    Ref<CCDrawNode> s_dot;
    CCPoint s_lastDotPosition = CCPointZero;
    bool s_hasLastDotPosition = false;
    int s_touchCount = 0;

    float randomOffset(float maxOffset) {
        if (maxOffset <= 0.f) return 0.f;
        return ((static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.f - 1.f) * maxOffset;
    }

    // Called when the button / screen is pressed: dot appears and STAYS.
    void showDot() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene) return;

        // Already holding a dot in this scene (e.g. dual mode pushes twice).
        if (s_dot) {
            if (s_dot->getParent() == scene) return;
            s_dot = nullptr;
        }

        const auto winSize = director->getWinSize();
        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const float shake = static_cast<float>(mod->getSettingValue<int64_t>("shakiness"));
        const float xPercent = static_cast<float>(mod->getSettingValue<int64_t>("x-position"));
        const float yPercent = static_cast<float>(mod->getSettingValue<int64_t>("y-position"));
        if (opacityPct <= 0.f || radius <= 0.f) return;

        // X/Y are percentages of the screen. Cocos origin is bottom-left.
        CCPoint p = ccp(winSize.width * std::clamp(xPercent, 0.f, 100.f) / 100.f,
                        winSize.height * std::clamp(yPercent, 0.f, 100.f) / 100.f);

        if (shake > 0.f) {
            p.x += randomOffset(shake);
            p.y += randomOffset(shake);
            p.x = std::clamp(p.x, radius, std::max(radius, winSize.width - radius));
            p.y = std::clamp(p.y, radius, std::max(radius, winSize.height - radius));

            // Avoid landing on the exact same spot as the previous tap.
            if (s_hasLastDotPosition && std::abs(p.x - s_lastDotPosition.x) < 0.01f &&
                std::abs(p.y - s_lastDotPosition.y) < 0.01f) {
                p.x = std::clamp(p.x + std::max(1.f, shake * 0.1f), radius,
                                 std::max(radius, winSize.width - radius));
            }
        }
        s_lastDotPosition = p;
        s_hasLastDotPosition = true;

        auto dot = CCDrawNode::create();
        if (!dot) return;
        ccColor4F color = {1.f, 1.f, 1.f, std::clamp(opacityPct / 100.f, 0.f, 1.f)};
        dot->drawDot(CCPointZero, radius, color);
        dot->setPosition(p);
        dot->setZOrder(999999);
        scene->addChild(dot);
        s_dot = dot;
    }

    // Called when the button / screen is released: dot shrinks away and is removed.
    void hideDot() {
        if (!s_dot) return;
        CCDrawNode* dot = s_dot.data();
        s_dot = nullptr;
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

    bool shouldTrack(PlayerButton button) {
        // Only react to real gameplay (level or editor playtest), not icon previews.
        if (!PlayLayer::get() && !LevelEditorLayer::get()) return false;
        if (Mod::get()->getSettingValue<bool>("only-on-jump") && button != PlayerButton::Jump) return false;
        return true;
    }
}

// Same detection method as the Click Sounds mod: the game's own button press/release events.
class $modify(ShowTapPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);
        if (shouldTrack(button)) showDot();
        return result;
    }

    bool releaseButton(PlayerButton button) {
        bool result = PlayerObject::releaseButton(button);
        if (shouldTrack(button)) hideDot();
        return result;
    }
};

// Optional "Show Everywhere" mode: raw screen touches outside of levels (menus etc).
class $modify(ShowTapTouch, CCTouchDispatcher) {
    void touches(CCSet* set, CCEvent* event, unsigned int type) {
        CCTouchDispatcher::touches(set, event, type);
        if (!set || PlayLayer::get()) return;
        if (!Mod::get()->getSettingValue<bool>("show-everywhere")) return;

        const int n = static_cast<int>(set->count());
        if (type == 0) {            // began
            s_touchCount += n;
            showDot();
        } else if (type == 2 || type == 3) {   // ended / cancelled
            s_touchCount = std::max(0, s_touchCount - n);
            if (s_touchCount == 0) hideDot();
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
