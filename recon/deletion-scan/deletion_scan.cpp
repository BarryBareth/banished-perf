// ---- entity deleted: which citizens' actions referred to it? ----------------------------------------------------
// exe+0x8f300(town, entityRef) runs for every deleted entity (a felled tree, an emptied pile...): for each of the town's
// citizens (town+0xa0, count +0xa8) it calls exe+0xca790 on the citizen's action queue (*(citizen+0x278)+0x48:
// [0] slots, [1] count), which asks every queued action "do you refer to this entity?" (Action vf9, vtable +0x48) and
// cancels the whole queue on the first yes. ~1000 citizens x a few actions x virtual calls, per deletion.
// Every Action vf9 is a pure pointer comparison (CheckPtr is a 3-byte no-op in this build): a field of the action, or
// the owner entity (+0x10) of a component the field points to, against the entity. We do those comparisons inline per
// action type (by vtable) and call exe+0xca790 only for a citizen whose queue has a match - it then cancels exactly as
// before; a queue without one is left alone, as the original leaves it. Unknown types / null fields: the real call.
enum : uint8_t { kNever, kD20, kD28, kD20orD28, kO20, kO28, kD28orO20, kD38orO30, kO28orO20, kO28orD30orD38 };
struct ActionKind { uintptr_t vtableRva; uint8_t test; };
static const ActionKind kActions[] = {
    {0x15ea70, kD20},           // PlayAnimation
    {0x15ead0, kO20},           // ShowTool
    {0x165f20, kD20},           // AnimateNaturalResource
    {0x165f80, kNever},         // CompleteRoad
    {0x165fe0, kD28orO20},      // CompleteTask
    {0x166050, kD20},           // ConsumeRawMaterial
    {0x1660c0, kD28},           // CreateNaturalResource
    {0x166130, kO28orD30orD38}, // CreateRawMaterial
    {0x1661a0, kD38orO30},      // DropOffRawMaterial
    {0x166200, kO28orO20},      // HuntAnimal
    {0x166260, kO28},           // MoveTo
    {0x1662c0, kO20},           // PerformWork
    {0x166330, kD28orO20},      // PickupRawMaterial
    {0x1663a0, kD28orO20},      // PickupStoredRawMaterial
    {0x166400, kD20},           // RemoveAnimal
    {0x166470, kD28orO20},      // RemoveNaturalResource
    {0x1664e0, kD20orD28},      // SendComponentMessage
};
static uintptr_t g_actionVt[sizeof kActions / sizeof kActions[0]];
uint32_t g_delScans, g_delCitizens, g_delCancels;

using RefersTo_t = bool(__fastcall*)(const uint8_t* action, const uint8_t* const* entityRef);
// The answer of `action`'s vf9 for entity E, computed inline when we know the type (-1: unknown, ask the action).
static int RefersTo(const uint8_t* a, const uint8_t* e) {
    uintptr_t vt = *(const uintptr_t*)a;
    int k = -1;
    for (size_t i = 0; i < sizeof kActions / sizeof kActions[0]; i++)
        if (g_actionVt[i] == vt) { k = kActions[i].test; break; }
    if (k < 0) return -1;
    auto D = [a](int off) { return *(const uint8_t* const*)(a + off); };
    auto O = [a](int off, const uint8_t** owner) {  // owner entity of the component at `off` (false: null field)
        const uint8_t* c = *(const uint8_t* const*)(a + off);
        if (!c) return false;
        *owner = *(const uint8_t* const*)(c + 0x10);
        return true;
    };
    const uint8_t *o1, *o2;
    switch (k) {
        case kNever: return 0;
        case kD20: return D(0x20) == e;
        case kD28: return D(0x28) == e;
        case kD20orD28: return D(0x20) == e || D(0x28) == e;
        case kO20: return O(0x20, &o1) ? o1 == e : -1;
        case kO28: return O(0x28, &o1) ? o1 == e : -1;
        case kD28orO20: return D(0x28) == e ? 1 : O(0x20, &o1) ? o1 == e : -1;
        case kD38orO30: return D(0x38) == e ? 1 : O(0x30, &o1) ? o1 == e : -1;
        case kO28orO20: return !O(0x28, &o1) ? -1 : o1 == e ? 1 : O(0x20, &o2) ? o2 == e : -1;
        case kO28orD30orD38: return !O(0x28, &o1) ? -1 : (o1 == e || D(0x30) == e || D(0x38) == e);
    }
    return -1;
}

using CitizenScan_t = void(__fastcall*)(int64_t* queue, const uint8_t* const* entityRef);  // exe+0xca790
static CitizenScan_t g_citizenScan;
using EntityGone_t = void(__fastcall*)(uint8_t* town, const uint8_t* const* entityRef);
static EntityGone_t o_EntityGone;
static void __fastcall h_EntityGone(uint8_t* town, const uint8_t* const* entityRef) {
    g_delScans++;
    const uint8_t* e = *entityRef;
    for (int i = 0; i < *(int*)(town + 0xa8); i++) {  // count and array re-read: a cancel may change them
        uint8_t* citizen = (*(uint8_t***)(town + 0xa0))[i];
        int64_t* queue = (int64_t*)(*(uint8_t**)(citizen + 0x278) + 0x48);
        g_delCitizens++;
        const uint8_t* const* slots = (const uint8_t* const*)queue[0];
        int n = (int)queue[1];
        bool hit = false;
        for (int s = 0; s < n && !hit; s++) {
            const uint8_t* a = slots[s];
            if (!a) continue;
            int r = RefersTo(a, e);
            if (r < 0 || g_verify) {
                bool real = (*(RefersTo_t*)(*(const uintptr_t*)a + 0x48))(a, entityRef);
                if (r >= 0 && (r != 0) != real && ++g_mismatches <= 20)
                    Log("movefast deletion MISMATCH: action vtable exe+%llx says %d, mine %d",
                        (unsigned long long)(*(const uintptr_t*)a - (uintptr_t)g_exe), real, r);
                r = real;
            }
            hit = r != 0;
        }
        if (hit) {
            g_delCancels++;
            g_citizenScan(queue, entityRef);  // the game's own: finds the same action and cancels the queue
        }
    }
}

