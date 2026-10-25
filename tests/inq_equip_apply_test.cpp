// Standalone white-box test: compile this instead of src/inq.cpp, and link
// src/config.cpp src/gamedata.cpp src/i18n.cpp src/hook.cpp (no game process needed).
#include "../src/inq.cpp"

#include <cstdio>
#include <vector>

// Keep diagnostics local; tests must not invoke the game's chat function.
void Log(const char*, ...) {}
void Chat(const std::string&) {}

static int failures = 0;
#define CHECK(cond, label) do { if (!(cond)) { std::printf("FAIL: %s\n", label); ++failures; } } while (0)

template<class T> static void Put(std::uintptr_t address, T value)
{
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
static bool MapGlobal(std::uintptr_t address)
{
    const auto base = address & ~std::uintptr_t(0xFFFF);
    return ::VirtualAlloc(reinterpret_cast<void*>(base), 0x10000,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != nullptr;
}
static hook::Ctx Context()
{
    hook::Ctx c{};
    c.rax = 0x71D0;
    c.rbx = 0x02A0;
    return c;
}

// Exercise the real Tick loading path: matching a preset is not proof that the
// plugin owns the meal. The player can eat exactly the same meal at the canteen.
static void CheckMealApply(std::uintptr_t load, std::uintptr_t food)
{
    using namespace inq;
    Config cfg;
    cfg.foods[4] = FoodPreset{};
    auto& preset = cfg.foods[4];
    preset.hp = 5; preset.stamina = 2; preset.attack = 3;
    preset.defense = 1; preset.elemRes = 2;
    preset.skill[0] = 1; preset.skill[1] = 2; preset.skill[2] = 3;
    cfg.foods[5] = preset;
    cfg.foods[5].attack = 1;
    cfg.foods[6] = FoodPreset{}; // Explicitly selected neutral meal, not unselected.
    const std::int32_t manual[10] = {12345, 5, 2, 3, 1, 2, 1, 2, 3, 54321};
    auto seed = [&]() { std::memcpy(reinterpret_cast<void*>(food + 0x19A4), manual, sizeof(manual)); };
    auto unchanged = [&]() { return std::memcmp(reinterpret_cast<void*>(food + 0x19A4), manual, sizeof(manual)) == 0; };
    Put<std::uint32_t>(load + 0x1D04, 1);
    CancelPending();

    for (int selected : {-1, 0}) {
        for (int active : {-1, 0, 999}) {
            cfg.food = selected; cfg.activeSet = active;
            seed(); ApplyOptions(cfg); Tick(cfg); Tick(cfg);
            CHECK(unchanged(), "unselected meal and set preserve matching manual meal during repeated loading ticks");
            CHECK(g.equipLoadout == 0 && g_pendingSetItem == 0 && g_pendingEquipApply.load() == 0,
                  "unselected set never queues equipment or item overrides");
            auto c = Context(); OnApplyResetAndEquip(&c);
            CHECK(c.rax == 0x71D0 && c.rbx == 0x02A0, "unselected set leaves equipment hook untouched");
        }
        seed(); Put<std::int32_t>(food + 0x19A8, 4);
        std::int32_t other[10];
        std::memcpy(other, reinterpret_cast<void*>(food + 0x19A4), sizeof(other));
        Tick(cfg);
        CHECK(std::memcmp(other, reinterpret_cast<void*>(food + 0x19A4), sizeof(other)) == 0,
              "unselected meal preserves nonmatching manual meal too");
    }

    LoadoutSet set; set.id = 1;
    cfg.sets.push_back(set); cfg.activeSet = 1;
    for (int inherited : {-1, 0}) {
        cfg.sets[0].food = inherited;
        for (int global : {-1, 0, 4}) {
            cfg.food = global; seed(); ApplyOptions(cfg); QueueSet(cfg.sets[0]);
            CHECK(g.food == global, "set food zero or minus one inherits global selection");
            Tick(cfg);
            CHECK(unchanged(), "inherited unselected or matching selected meal preserves all fields and guards");
        }
    }
    cfg.food = 4; cfg.sets[0].food = 5;
    seed(); ApplyOptions(cfg); QueueSet(cfg.sets[0]); Tick(cfg);
    CHECK(g.food == 5 && game::Rd<std::int32_t>(food + 0x19B0) == 1,
          "explicit set meal still overrides global meal");
    CHECK(game::Rd<std::int32_t>(food + 0x19A4) == manual[0] &&
          game::Rd<std::int32_t>(food + 0x19C8) == manual[9], "selected meal leaves adjacent memory unchanged");

    cfg.activeSet = -1; cfg.food = 4;
    seed(); ApplyOptions(cfg); Tick(cfg);
    cfg.food = -1; ApplyOptions(cfg); Tick(cfg);
    CHECK(unchanged(), "deselecting an applied meal does not clear previously written values");
    RequestRestart(cfg);
    CHECK(g.restart == 1 && g.equipLoadout == 0 && g_pendingSetItem == 0,
          "restart with no selected set queues no loadout override");
    Tick(cfg); CHECK(unchanged(), "unselected restart preserves manual meal during loading");
    CancelPending();

    cfg.food = 5; ApplyOptions(cfg); seed();
    Put<std::uint32_t>(load + 0x1D04, 0); Tick(cfg);
    CHECK(unchanged(), "selected meal is not written outside loading");
    Put<std::uint32_t>(load + 0x1D04, 1); Tick(cfg);
    CHECK(game::Rd<std::int32_t>(food + 0x19B0) == 1, "selected global meal still applies during loading");
    cfg.food = 999; ApplyOptions(cfg); seed(); Tick(cfg);
    CHECK(unchanged(), "missing positive meal id does not clear manual meal");
    cfg.food = 6; ApplyOptions(cfg); Tick(cfg);
    const std::int32_t neutral[8] = {0, 0, 0, 0, 0, -1, -1, -1};
    CHECK(std::memcmp(reinterpret_cast<void*>(food + 0x19A8), neutral, sizeof(neutral)) == 0,
          "explicit positive neutral preset can still request no meal");
    Put<std::uint32_t>(load + 0x1D04, 0);
    CancelPending();
}

int main()
{
    using namespace inq;
    Config cfg;
    cfg.enabled = true;
    CHECK(!RequestEquipApply(cfg, "1"), "Dormant refuses requests");
    CHECK(g_pendingEquipApply.load() == 0, "Dormant does not publish work");
    g_enabled.store(true);
    CHECK(!RequestEquipApply(cfg, "1"), "uninstalled hooks refuse requests");
    g_ready = true;
    cfg.enabled = false;
    CHECK(!RequestEquipApply(cfg, "1"), "disabled config refuses requests");
    cfg.enabled = true;
    CHECK(RequestEquipApply(cfg, "1"), "selection queues without restart");
    CHECK(g.restart == 0 && g_pendingEquipApply.load() == 1, "no restart side effect");
    CHECK(RequestEquipApply(cfg, "224"), "last slot accepted");
    CHECK(g_pendingEquipApply.load() == 224, "latest selection wins");
    CHECK(!RequestEquipApply(cfg, "225"), "invalid slot rejected");
    CHECK(g_pendingEquipApply.load() == 0, "invalid selection clears stale request");
    CHECK(RequestEquipApply(cfg, ""), "empty selection cancels");
    cfg.activeSet = 1;
    LoadoutSet set;
    set.id = 1; set.equip = "2"; set.item = "#3"; set.food = 4; set.quest = 101;
    cfg.sets.push_back(set);
    g.food = -1; g.questId = 0;
    CHECK(RequestActiveSetEquipApply(cfg), "active set queues equipment only");
    CHECK(g_pendingEquipApply.load() == 2 && g_pendingSetItem == 0 && g.food == -1 && g.questId == 0,
          "item food quest unchanged");
    cfg.activeSet = -1;
    CHECK(RequestActiveSetEquipApply(cfg) && g_pendingEquipApply.load() == 0,
          "unselect cancels pending without unequipping");

    if (!MapGlobal(game::pSave) || !MapGlobal(game::pUiBase) || !MapGlobal(game::pFood)) {
        std::printf("FAIL: could not reserve fixed game global addresses\n");
        return 1;
    }
    std::vector<unsigned char> save(0xB0), slot(0x140500), base(0x300), ui(0x400),
        load(0x1D10), equipUi(0x3000), pageUi(0x300), indexUi(0x300), objects(0x2B40), food(0x1A00);
    auto At = [](std::vector<unsigned char>& v) { return reinterpret_cast<std::uintptr_t>(v.data()); };
    Put(game::pSave, At(save)); Put(At(save) + 0xA8, At(slot));
    Put(game::pUiBase, At(base)); Put(At(base) + 0x278, At(ui));
    Put(At(ui) + 0x20, At(load)); Put(At(ui) + 0x310, At(equipUi));
    Put(At(ui) + 0x60, At(objects) + 0x80);
    Put(At(equipUi) + 0x2960, At(pageUi)); Put(At(equipUi) + 0x2958, At(indexUi));
    Put(At(objects) + 0x80 + 0x470, At(objects) + 0x5E0);
    Put(game::pFood, At(food));
    CheckMealApply(At(load), At(food));

    // Names are read from the current save each time, never cached by slot ID.
    const auto nameAt = At(slot) + 0x10CCF4 - 0x100;
    std::memcpy(reinterpret_cast<void*>(nameAt), "Before rename", 14);
    auto names = ListEquipLoadouts();
    CHECK(!names.empty() && names[0].number == 1 && names[0].name == "Before rename", "read original game loadout name");
    std::memset(reinterpret_cast<void*>(nameAt), 0, 0x100);
    std::memcpy(reinterpret_cast<void*>(nameAt), "After rename", 13);
    names = ListEquipLoadouts();
    CHECK(!names.empty() && names[0].number == 1 && names[0].name == "After rename", "reread sees in-game rename without restart or changing stable slot");

    RequestEquipApply(cfg, "10");
    auto c = Context(); c.rax = 0;
    OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 10, "unmatched UI call leaves request queued");
    Put<std::uint32_t>(At(load) + 0x1D04, 1);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(c.rax == 0x71D0 && g_pendingEquipApply.load() == 10, "loading defers without changing registers");
    Put<std::uint32_t>(At(load) + 0x1D04, 0);
    Put<std::uintptr_t>(At(equipUi) + 0x2958, 0);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 10, "missing UI chain defers");
    Put(At(equipUi) + 0x2958, At(indexUi));
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 0 && c.rax == game::kVtEquipApply, "matching game thread call applies without restart");
    CHECK(game::Rd<std::uint32_t>(At(pageUi) + 0x240) == 1 &&
          game::Rd<std::uint32_t>(At(indexUi) + 0x274) == 1 && c.rdx == 1,
          "slot ten translates to page one index one");
    CHECK(g.restart == 0 && g_pendingSetItem == 0, "equipment application has no other action");

    Put<std::uint32_t>(At(slot) + 0x10CCF4, 0xFFFFFFFF);
    RequestEquipApply(cfg, "1");
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(c.rax == 0x71D0 && g_pendingEquipApply.load() == 0 && (g.message & 1),
          "empty loadout reports invalid without redirecting game call");
    RequestEquipApply(cfg, "2");
    g.restart = 3;
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 2 && c.rax == 0x71D0,
          "immediate selection never interrupts the restart flow");
    g.restart = 0;
    RequestRestart(cfg);
    CHECK(g_pendingEquipApply.load() == 0 && g.restart == 1,
          "restart supersedes stale immediate selection");
    CancelPending();
    RequestEquipApply(cfg, "2"); CancelPending();
    CHECK(g_pendingEquipApply.load() == 0, "profile cancellation clears immediate work");
    RequestEquipApply(cfg, "2"); SetEnabled(false);
    c = Context(); OnApplyResetAndEquip(&c);
    CHECK(g_pendingEquipApply.load() == 0 && c.rax == 0x71D0, "Dormant clears work and hook is pass-through");
    CHECK(!RequestEquipApply(cfg, "3") && g_pendingEquipApply.load() == 0, "Dormant selection never revives work");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
