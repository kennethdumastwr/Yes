#include <Geode/Geode.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace geode::prelude;

namespace {
    float randomOffset(float maxOffset) {
        if (maxOffset <= 0.f) return 0.f;
        return ((static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.f - 1.f) * maxOffset;
    }

    CCPoint s_lastDotPosition = CCPointZero;
    bool s_hasLastDotPosition = false;

    void showTapDot() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        auto director = CCDirector::sharedDirector();
        auto scene = director ? director->getRunningScene() : nullptr;
        if (!scene || !director) return;

        const auto winSize = director->getWinSize();
        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const float shake = static_cast<float>(mod->getSettingValue<int64_t>("shakiness"));
        const float lifetimeMs = static_cast<float>(mod->getSettingValue<int64_t>("lifetime"));
        const float xPercent = static_cast<float>(mod->getSettingValue<int64_t>("x-position"));
        const float yPercent = static_cast<float>(mod->getSettingValue<int64_t>("y-position"));
        if (opacityPct <= 0.f || radius <= 0.f) return;

        // X/Y are percentages of the game viewport. Cocos coordinates start at bottom-left.
        CCPoint p = ccp(winSize.width * std::clamp(xPercent, 0.f, 100.f) / 100.f,
                        winSize.height * std::clamp(yPercent, 0.f, 100.f) / 100.f);

        // Shakiness offsets the configured position, not the user's actual touch position.
        if (shake > 0.f) {
            p.x += randomOffset(shake);
            p.y += randomOffset(shake);
            p.x = std::clamp(p.x, radius, std::max(radius, winSize.width - radius));
            p.y = std::clamp(p.y, radius, std::max(radius, winSize.height - radius));

            // Avoid reusing the exact previous position when shakiness is enabled.
            if (s_hasLastDotPosition && std::abs(p.x - s_lastDotPosition.x) < 0.01f &&
                std::abs(p.y - s_lastDotPosition.y) < 0.01f) {
                p.x = std::clamp(p.x + std::max(1.f, shake * 0.1f), radius,
                                 std::max(radius, winSize.width - radius));
                if (std::abs(p.x - s_lastDotPosition.x) < 0.01f &&
                    std::abs(p.y - s_lastDotPosition.y) < 0.01f) {
                    p.y = std::clamp(p.y + std::max(1.f, shake * 0.1f), radius,
                                     std::max(radius, winSize.height - radius));
                }
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
        dot->runAction(CCSequence::create(
            CCDelayTime::create(std::max(0.05f, lifetimeMs / 1000.f)),
            CCRemoveSelf::create(),
            nullptr
        ));
    }
}

class $modify(ShowTapTouchDispatcher, CCTouchDispatcher) {
    void touchesBegan(CCSet* touches, CCEvent* event) {
        // Preserve the game's normal touch handling.
        CCTouchDispatcher::touchesBegan(touches, event);
        if (!touches) return;

        // Any touch triggers the indicator. Its position is controlled only by X/Y settings.
        for (auto it = touches->begin(); it != touches->end(); ++it) {
            if (typeinfo_cast<CCTouch*>(*it)) showTapDot();
        }
    }
};
