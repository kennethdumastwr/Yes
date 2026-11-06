#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <climits>
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
        // Sway (slow drifting while held)
        CCPoint swayBase = CCPointZero;
        float swayAmp = 0.f;
        float swayTime = 0.f;
        float swayPhase[4] = {0.f, 0.f, 0.f, 0.f};
    };

    std::array<DotSlot, SLOT_COUNT> s_slots;
    int s_touchCount = 0;
    // Set by the game's own "level complete" event (NOT the percentage, which some levels get wrong).
    bool s_levelComplete = false;

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
        if (PlayLayer::get() && s_levelComplete) return false;
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
        // The engine draws these shapes with PREMULTIPLIED alpha blending, so the colour
        // must be multiplied by its alpha. Without this, alpha 0 still drew the full colour.
        ccColor4F pm = {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
        node->drawPolygon(verts, kSegments, pm, 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
    }

    // ---------- Sway ----------
    // Driven by the DIRECTOR's scheduler (not by a scene node): a scene wipes its nodes'
    // scheduled updates whenever it closes, which silently killed sway before.
    void tickSway(float dt) {
        for (auto& slot : s_slots) {
            if (!slot.held || slot.swayAmp <= 0.f || !slot.dot || !slot.dot->isVisible()) continue;
            slot.swayTime += dt;
            const float t = slot.swayTime;
            const float ox = 0.6f * std::sin(1.7f * t + slot.swayPhase[0]) + 0.4f * std::sin(2.9f * t + slot.swayPhase[1]);
            const float oy = 0.6f * std::sin(1.3f * t + slot.swayPhase[2]) + 0.4f * std::sin(3.3f * t + slot.swayPhase[3]);
            slot.dot->setPosition(ccp(slot.swayBase.x + ox * slot.swayAmp, slot.swayBase.y + oy * slot.swayAmp));
        }
    }

    class SwayTicker : public CCObject {
    public:
        void update(float dt) override { tickSway(dt); }
    };

    void ensureSwayTicker() {
        static bool started = false;
        if (started) return;
        auto director = CCDirector::sharedDirector();
        if (!director || !director->getScheduler()) return;
        started = true;
        auto ticker = new SwayTicker();   // intentionally lives for the whole session
        director->getScheduler()->scheduleUpdateForTarget(ticker, 0, false);
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
            dot->removeFromParentAndCleanup(false);
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
        slot.swayBase = p;
        slot.swayAmp = static_cast<float>(mod->getSettingValue<int64_t>("sway"));
        slot.swayTime = 0.f;
        for (auto& ph : slot.swayPhase) {
            ph = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 6.2831853f;
        }
        ensureSwayTicker();
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

    // Touches that started on the in-game ALT button: they must never draw a real dot.
    Ref<CCNode> s_altBtn;
    std::unordered_set<int> s_ignoredTouches;

    bool touchOnAltButton(const CCPoint& glPos) {
        if (!s_altBtn || !s_altBtn->getParent() || !s_altBtn->isVisible()) return false;
        const CCPoint local = s_altBtn->convertToNodeSpace(glPos);
        const CCSize sz = s_altBtn->getContentSize();
        return CCRect(-6.f, -6.f, sz.width + 12.f, sz.height + 12.f).containsPoint(local);
    }

    // Not playing a level: menus, the level editor, the pause screen, etc.
    bool realTapActive() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled") || !mod->getSettingValue<bool>("real-show-tap")) return false;
        if (!PlayLayer::get()) {
            s_levelComplete = false;   // left the level: forget the completed state
            return true;
        }
        // In a level: active while paused, once completed, or ALWAYS if "during gameplay" is on.
        return isPaused() || s_levelComplete || mod->getSettingValue<bool>("real-show-tap-playing");
    }

    // One dedicated layer that holds every real-tap dot.
    //
    // INDEPENDENT mode (default): the layer is NOT part of any scene. It hangs off the
    // director's notification node, which the engine draws AFTER the whole scene every
    // frame. So no scroll layer, popup, clipping node or other mod's z-order can ever
    // cover it, move it or make it flicker.
    // CLASSIC mode (fallback toggle): a child of the running scene, raised to
    // "highest sibling + 1" (never INT_MAX - popups use "highest + 1" and would overflow).
    Ref<CCNode> s_tapLayer;

    CCNode* raiseTapLayer() {
        auto director = CCDirector::sharedDirector();
        if (!director) return nullptr;

        if (!s_tapLayer) {
            auto node = CCNode::create();
            if (!node) return nullptr;
            s_tapLayer = node;
        }
        CCNode* layer = s_tapLayer.data();

        if (Mod::get()->getSettingValue<bool>("independent-layer")) {
            CCNode* note = director->getNotificationNode();
            if (!note) {
                // Nobody uses the notification node: our layer becomes it.
                layer->removeFromParentAndCleanup(false);
                director->setNotificationNode(layer);
            } else if (note != layer && layer->getParent() != note) {
                layer->removeFromParentAndCleanup(false);
                note->addChild(layer, 1000000);
            }
            // The engine does not "enter" notification nodes by itself; without this the
            // fade-out animations would stay paused.
            if (!layer->isRunning()) layer->onEnter();
            return layer;
        }

        // ---- classic mode ----
        if (director->getNotificationNode() == layer) director->setNotificationNode(nullptr);
        auto scene = director->getRunningScene();
        if (!scene) return nullptr;

        if (layer->getParent() != scene) {
            layer->removeFromParentAndCleanup(false);
            scene->addChild(layer, 0);
        }

        int highest = 0;
        for (CCNode* child : scene->getChildrenExt()) {
            if (child != layer) highest = std::max(highest, child->getZOrder());
        }
        const int z = highest >= INT_MAX - 1 ? INT_MAX - 1 : highest + 1;
        // reorderChild also makes it the newest child, so it wins any tie.
        scene->reorderChild(layer, z);
        return layer;
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

    // `glPos` is the raw touch position in screen (GL) coordinates, captured BEFORE
    // the game or any other mod gets to handle the touch.
    void createRealDot(int id, const CCPoint& glPos) {
        removeRealDot(id);

        auto mod = Mod::get();
        const float radius = static_cast<float>(mod->getSettingValue<int64_t>("dot-size"));
        const float opacityPct = static_cast<float>(mod->getSettingValue<int64_t>("opacity"));
        const ccColor3B rgb = mod->getSettingValue<ccColor3B>("dot-color");
        if (opacityPct <= 0.f || radius <= 0.f) return;

        CCNode* layer = raiseTapLayer();
        if (!layer) return;

        auto dot = CCDrawNode::create();
        if (!dot) return;
        ccColor4F color = {
            rgb.r / 255.f,
            rgb.g / 255.f,
            rgb.b / 255.f,
            std::clamp(opacityPct / 100.f, 0.f, 1.f)
        };
        drawCircle(dot, radius, color);
        dot->setPosition(layer->convertToNodeSpace(glPos));
        layer->addChild(dot);
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
                if (touchOnAltButton(touch->getLocation())) {
                    s_ignoredTouches.insert(id);   // pressing the ALT button: no dot
                } else {
                    s_ignoredTouches.erase(id);
                    if (active) createRealDot(id, touch->getLocation());
                }
            } else if (type == 1) {          // moved
                if (s_ignoredTouches.count(id)) continue;
                auto found = s_realDots.find(id);
                if (found != s_realDots.end() && found->second) {
                    CCNode* layer = raiseTapLayer();   // stays on top of newly opened UIs
                    if (layer) {
                        found->second->setPosition(layer->convertToNodeSpace(touch->getLocation()));
                    }
                } else if (active) {
                    // Self-heal: finger is still down but the dot was lost (a UI/mod sent a
                    // stray cancel, or the scene changed) - bring it back.
                    createRealDot(id, touch->getLocation());
                }
            } else {                         // ended / cancelled
                s_ignoredTouches.erase(id);
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

    // ---------- Tap sounds ----------
    // 6 "Normal" + 6 "Light" mp3 files from the mod's resources folder. They are loaded
    // once (preloaded) and played straight through FMOD for the lowest possible latency.
    FMOD::Sound* s_tapSounds[2][6] = {};
    bool s_tapSoundsTried = false;
    int s_lastTapIndex[2] = {-1, -1};

    void loadTapSounds() {
        if (s_tapSoundsTried) return;
        auto engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system) return;   // audio not ready yet: try again later
        s_tapSoundsTried = true;

        const auto dir = Mod::get()->getResourcesDir();
        for (int kind = 0; kind < 2; ++kind) {
            for (int i = 0; i < 6; ++i) {
                const auto path = (dir / fmt::format("{}{}.mp3", kind == 0 ? "Normal" : "Light", i + 1)).string();
                FMOD::Sound* sound = nullptr;
                if (engine->m_system->createSound(path.c_str(), FMOD_DEFAULT, nullptr, &sound) == FMOD_OK) {
                    s_tapSounds[kind][i] = sound;
                }
            }
        }
    }

    // Plays one of the 6 variants (never the same one twice in a row).
    void playTapSound(bool light) {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled") || !mod->getSettingValue<bool>("tap-sounds")) return;

        loadTapSounds();
        auto engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system) return;

        const int kind = light ? 1 : 0;
        int idx = std::rand() % 6;
        if (idx == s_lastTapIndex[kind]) idx = (idx + 1 + std::rand() % 5) % 6;

        FMOD::Sound* sound = s_tapSounds[kind][idx];
        if (!sound) {   // that file failed to load: use any variant that did
            for (int i = 0; i < 6 && !sound; ++i) {
                if (s_tapSounds[kind][i]) { sound = s_tapSounds[kind][i]; idx = i; }
            }
            if (!sound) return;
        }
        s_lastTapIndex[kind] = idx;

        const float volume = std::clamp(static_cast<float>(mod->getSettingValue<int64_t>("tap-volume")) / 100.f, 0.f, 1.f);
        FMOD::Channel* channel = nullptr;
        if (engine->m_system->playSound(sound, nullptr, true, &channel) == FMOD_OK && channel) {
            channel->setVolume(volume);
            channel->setPaused(false);
        }
    }

    // Does the last finished click + the gap before this press fit the given CPS?
    bool cycleQualifies(Clock::time_point now, int targetCps) {
        if (!s_hasCycle) return false;
        const int t = std::clamp(targetCps, 2, 16);
        const double airMs = secondsBetween(now, s_lastRelease) * 1000.0;
        return s_lastHoldMs <= kHoldMs[t - 2] && airMs <= kAirMs[t - 2];
    }

    // Single-player press (not two-player mode): picks the dot AND the sound.
    void handleSinglePress() {
        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;

        const auto now = Clock::now();
        // Dual mode pushes Player 1 AND Player 2 for one click: count it once.
        if (secondsBetween(now, s_lastPress) < 0.002 && !s_heldClicks.empty()) return;
        s_lastPress = now;

        // ---- which dot (alternate clicking) ----
        int slot = SLOT_MAIN;
        bool altQualifies = false;
        if (mod->getSettingValue<bool>("alt-click-mode")) {
            altQualifies = cycleQualifies(now, static_cast<int>(mod->getSettingValue<int64_t>("alt-click-cps")));
        }
        if (altQualifies) {
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

        // ---- which sound (normal or light) ----
        // Light plays when alternate clicking is active, OR when its own CPS is reached.
        if (mod->getSettingValue<bool>("tap-sounds")) {
            bool light = false;
            if (s_altActive && mod->getSettingValue<bool>("light-on-alternate")) light = true;
            if (!light && mod->getSettingValue<bool>("light-own-cps") &&
                cycleQualifies(now, static_cast<int>(mod->getSettingValue<int64_t>("light-cps")))) {
                light = true;
            }
            playTapSound(light);
        }

        showDot(slot);
        s_heldClicks.push_back({slot, now});
        while (s_heldClicks.size() > 8) s_heldClicks.pop_front();
    }

    // Single-player release (not two-player mode). Always measures the hold duration,
    // because the light-sound CPS needs it even when alternate clicking is off.
    void handleSingleRelease() {
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

    // ---------- In-game alternate-clicking button ----------
    constexpr float kAltButtonRadius = 22.f;
    Ref<CCDrawNode> s_altCircle;
    Ref<CCLabelBMFont> s_altLabel;

    void refreshAltButton() {
        if (!s_altCircle || !s_altLabel) return;
        auto mod = Mod::get();
        const float opacity = std::clamp(static_cast<float>(mod->getSettingValue<double>("alt-button-opacity")), 0.f, 1.f);
        const bool on = mod->getSettingValue<bool>("alt-click-mode");

        // Green when alternate clicking is ON, grey when OFF.
        ccColor4F fill = on ? ccc4f(0.25f, 0.85f, 0.35f, opacity) : ccc4f(0.35f, 0.35f, 0.35f, opacity);
        s_altCircle->clear();
        drawCircle(s_altCircle.data(), kAltButtonRadius, fill);
        s_altLabel->setOpacity(static_cast<unsigned char>(opacity * 255.f));
    }
}

// Same detection method as the Click Sounds mod: the game's own button press/release events.
class $modify(ShowTapPlayer, PlayerObject) {
    bool pushButton(PlayerButton button) {
        bool result = PlayerObject::pushButton(button);
        if (shouldShowForButton(button)) {
            const int slot = slotFor(this);
            if (slot != SLOT_MAIN) {                // two-player mode: per-player dot
                showDot(slot);
                playTapSound(false);
            }
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
        // Real Show Tap runs BEFORE the game / other mods handle the touch, so scroll
        // layers and UIs can never shift the position we read.
        const bool realActive = set ? realTapActive() : false;
        if (set && (realActive || !s_realDots.empty())) updateRealDots(set, type, realActive);

        CCTouchDispatcher::touches(set, event, type);
        if (!set) return;
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
        s_levelComplete = false;   // restarted / new attempt: playing again
        PlayLayer::resetLevel();
        prewarmDots();
        loadTapSounds();
    }

    // The game's own "you beat the level" event - works even when the percentage is wrong.
    void levelComplete() {
        s_levelComplete = true;
        PlayLayer::levelComplete();
    }
};

// In-game button (bottom right): taps toggle "Alternate clicking mode" on/off.
class $modify(ShowTapUILayer, UILayer) {
    bool init(GJBaseGameLayer* layer) {
        if (!UILayer::init(layer)) return false;
        if (!Mod::get()->getSettingValue<bool>("alt-button")) return true;

        const float R = kAltButtonRadius;
        auto face = CCNode::create();
        face->setContentSize({R * 2.f, R * 2.f});

        auto circle = CCDrawNode::create();
        circle->setPosition({R, R});
        auto label = CCLabelBMFont::create("ALT", "bigFont.fnt");
        label->setScale(0.45f);
        label->setPosition({R, R});
        face->addChild(circle);
        face->addChild(label);
        s_altCircle = circle;
        s_altLabel = label;
        refreshAltButton();

        auto btn = CCMenuItemSpriteExtra::create(
            face, this, menu_selector(ShowTapUILayer::onToggleAltClicking)
        );
        s_altBtn = btn;
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->addChild(btn);
        const auto win = CCDirector::sharedDirector()->getWinSize();
        btn->setPosition({win.width - 34.f, 34.f});
        this->addChild(menu, 100);
        return true;
    }

    void onToggleAltClicking(CCObject*) {
        auto mod = Mod::get();
        mod->setSettingValue<bool>("alt-click-mode", !mod->getSettingValue<bool>("alt-click-mode"));
        refreshAltButton();
    }
};
