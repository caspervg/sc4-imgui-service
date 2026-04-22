# Transit-Enabled Lot Road Access Investigation

This note records the current investigation into allowing growable lots to
develop next to transit-enabled lots, especially RTMT-style on-street mass
transit stops. The working hypothesis is that the game currently treats a lot
beside a transit-enabled mass transit lot as not having road access, even when
the transit-enabled lot is itself placed on or beside a valid road-like
network.

The proposed mod is intentionally narrow:

- Hook the Windows `cSC4Lot::CalculateRoadAccess` implementation.
- Preserve the game's stock road-access result whenever it succeeds.
- If stock road access fails, grant access only when the growable lot directly
  side-touches a transit-enabled lot that has road-like network contact.
- Do not support chains of transit-enabled lots before reaching a real network.
- Optionally include Dirt Road in the same road-like mask used by both the
  transit-enabled-lot exception and the direct stock road-access check.

## Repository Context

The current prototype lives in:

- `src/sample/TransitAccessDirector.cpp`
- `CMakeLists.txt`, target `SC4TransitAccess`

Useful local references:

- `vendor/gzcom-dll/gzcom-dll/include/cISC4NetworkOccupant.h`
- `vendor/gzcom-dll/gzcom-dll/include/cISC4Lot.h`
- `vendor/gzcom-dll/gzcom-dll/include/cISC4LotManager.h`
- `vendor/sc4-properties/new_properties.xml`
- `vendor/sc4-properties/tropod_Properties.xml`

The DLL is gated to game version `641`, matching the Windows binary under
analysis.

## Problem Statement

RTMT-style lots are transit-enabled lots that sit on, or visually integrate
with, normal networks. A growable lot adjacent to one of these lots should be
able to treat that transit-enabled lot as providing access to the road network.
In practice, growables can fail to develop because the game's road-access
calculation sees the adjacent cell as a lot rather than as a road-like network.

This is different from transit pathfinding. The immediate issue is access
eligibility for lot development and lot state. Once access is granted, the
traffic simulator still needs valid paths and valid transit switches. Based on
the current Mac binary exploration, the commute setup code uses lot lookup and
pathfinding state rather than simply calling `HasRoadAccess`, so this hook
should be understood as fixing the access gate, not rewriting the pathfinder.

## Binary Instances Used

Two Ghidra instances were used:

- Mac OS x86 binary with symbols.
- Windows x86 game binary, mostly stripped, for addresses and hooks.

The Mac binary is useful for names, call shapes, and intent. The Windows binary
is authoritative for hook addresses, instruction sizes, and global pointer
addresses.

## Relevant Network Mask Model

The game represents network sets as a bitfield over
`cISC4NetworkOccupant::eNetworkType`.

The formula is:

```cpp
uint32_t mask = 1u << networkType;
```

The SDK header and property tables agree on the following ordering. Several
traffic simulator properties have `Count="13"` and list the same network order:
road, rail, highway, street, pipe, powerline, avenue, subway, light rail,
monorail, one-way road, dirt road, ground highway.

### Single-Network Masks

| SDK name | Index | Mask |
| --- | ---: | ---: |
| `Road` | 0 | `0x00000001` |
| `Rail` | 1 | `0x00000002` |
| `Highway` | 2 | `0x00000004` |
| `Street` | 3 | `0x00000008` |
| `WaterPipe` | 4 | `0x00000010` |
| `PowerPole` | 5 | `0x00000020` |
| `Avenue` | 6 | `0x00000040` |
| `Subway` | 7 | `0x00000080` |
| `LightRail` | 8 | `0x00000100` |
| `Monorail` | 9 | `0x00000200` |
| `OneWayRoad` | 10 | `0x00000400` |
| `DirtRoad` | 11 | `0x00000800` |
| `GroundHighway` | 12 | `0x00001000` |

### Useful Combined Masks

| Meaning | Mask | Contents |
| --- | ---: | --- |
| Stock road access | `0x00000449` | Road, Street, Avenue, OneWayRoad |
| Stock road access plus Dirt Road | `0x00000C49` | Road, Street, Avenue, OneWayRoad, DirtRoad |
| Broad surface road-like set | `0x00001C4D` | Road, Highway, Street, Avenue, OneWayRoad, DirtRoad, GroundHighway |
| Rail/transit guideway set | `0x00000382` | Rail, Subway, LightRail, Monorail |
| Utility set | `0x00000030` | WaterPipe, PowerPole |
| All known network bits | `0x00001FFF` | Bits 0 through 12 |

The current prototype uses:

```cpp
constexpr uint32_t kStockRoadLikeNetworkMask = 0x00000449;
constexpr uint32_t kDirtRoadNetworkMask = 1u << 11;
```

When Dirt Road support is enabled, the effective mask becomes:

```cpp
0x00000449 | 0x00000800 == 0x00000C49
```

### Dirt Road Policy

Dirt Road is a vestigial game network reactivated by NAM and commonly used in
RHW-related contexts. The safest policy is to make Dirt Road support an
explicit option and have it affect both checks together:

- The direct stock road-access mask patch inside `CalculateRoadAccess`.
- The transit-enabled-lot exception's road-like network lookup.
- The ferry terminal road-access special case.
- The lot placement/facing street-count helper, if Dirt Road should behave as
  low-priority frontage like Street and OneWayRoad.

That keeps the mod internally consistent. If Dirt Road is not enabled, neither
direct lots nor lots beside transit-enabled lots should get special Dirt Road
access from this DLL.

Highway and GroundHighway should not be added casually. Stock road access does
not include them. Adding them would be a broader gameplay rule change than
adding Dirt Road for NAM compatibility.

## Key Properties And Interfaces

### Network Occupant Interface

`cISC4NetworkOccupant` has IID:

```cpp
0xA821EF94
```

Relevant methods include:

- `HasNetworkFlag(uint32_t flag)`
- `HasAnyNetworkFlag(uint32_t flag)`
- `GetNetworkFlag()`
- `IsOfType(eNetworkType type)`

The stock road-access check uses `HasAnyNetworkFlag(0x449)` on adjacent network
occupants.

### Transit Switch Point Property

Transit-enabled lots are identified by the `Transit Switch Point` exemplar
property:

```cpp
0xE90E25A1
```

The current prototype checks for this property on both:

- The lot property holder.
- The lot's building occupant property holder.

The second location is important because transit switches are commonly stored
on the building exemplar. There is still some uncertainty around which custom
content stores the property on the lot exemplar versus the building exemplar,
so checking both is the conservative implementation.

### Network Lots Are Different

The game also has network lots and type 21 network props. These appear to be
used for network beautification and network lot machinery, and should not be
treated as equivalent to transit-enabled lots. A lot being a network lot is not
the same thing as having a transit switch point.

The exception should therefore be based on transit switch properties plus real
network contact, not on network-lot classification alone.

## Mac Binary Findings

The Mac binary with symbols gives the clearest view of intent.

### `cSC4Lot::CalculateRoadAccess`

The stock road-access algorithm scans cells adjacent to the lot looking for
road-like networks. It uses the network occupant interface and checks:

```cpp
HasAnyNetworkFlag(0x449)
```

The `0x449` mask is:

- Road: `0x00000001`
- Street: `0x00000008`
- Avenue: `0x00000040`
- OneWayRoad: `0x00000400`

This exactly matches the stock road-access mask described above.

### Ferry Terminal Special Case

There is special handling for ferry terminals. The ferry path does not simply
use the adjacent network occupant scan. It checks the traffic network map
instead and filters out entries with an exclusion flag.

The Windows helper corresponding to this behavior uses:

- `spTrafficNetworkMap`
- A traffic network map lookup via vtable offset `+0x20`
- Mask `0x449`
- A returned-entry flag check via vtable offset `+0x20`
- Exclusion flag `0x00200000`

This matters because it shows that Maxis already had a precedent for using the
traffic map rather than only raw adjacent network occupants when lot access
needs special handling.

### `cSC4Lot::HasRoadAccess`

`HasRoadAccess()` appears to call:

```cpp
HasOrganicRoadAccess(0)
```

For generic access, the path is still ultimately backed by
`CalculateRoadAccess`.

### `cSC4Lot::HasOrganicRoadAccess`

`HasOrganicRoadAccess(purpose)` first ensures the road-access cache exists.
If generic road access succeeds, it returns true immediately. If generic road
access fails and the purpose is nonzero, it calculates organic road access for
that purpose.

This likely supports agricultural and industrial-style organic layouts where
not every building needs to sit directly next to a road. It should not be the
primary hook point for RTMT road access because the RTMT issue is about the
generic access cache/result.

### Traffic Simulator

The currently inspected traffic simulator setup path does not simply ask
`HasRoadAccess()` to decide every trip. `cSC4TrafficSimulator::SetupTrip` uses
lot manager lookups and pathfinding state. Commute refresh code similarly uses
`LotManager::GetLot(x,z,true)` with a fallback lookup.

This supports the current division of responsibility:

- The DLL exception grants road access for growth/access eligibility.
- The existing traffic simulator still handles actual pathfinding through
  networks and transit switches.

## Windows Binary Findings

All hook addresses must be taken from the Windows binary.

### `cSC4Lot::CalculateRoadAccess`

Address:

```cpp
0x006C1A30
```

Observed prologue bytes:

```text
55 8B EC 83 E4 F8
```

The prototype overwrites exactly 6 bytes:

- 5-byte `JMP rel32`
- 1-byte `NOP`

The trampoline copies those 6 bytes and jumps back to:

```cpp
0x006C1A36
```

### Road-Access Cache Writes

The stock failure path writes false to the road-access cache around:

```cpp
0x006C1AAE
```

The stock success path writes true around:

```cpp
0x006C1C79
```

Both paths use the same internal map helper:

```cpp
0x006C0F30
```

The helper is called as a `__thiscall` using:

```cpp
this = lot + 0x40
arg  = &key0
```

The prototype reuses this helper after the stock function returns false. If the
transit-enabled-lot exception succeeds, it writes the road-access cache back to
true.

### Stock Network Mask Immediates

The stock direct road-access mask is pushed at:

```cpp
0x006C1BD1: 68 49 04 00 00
```

This is:

```asm
push 0x449
```

The following call uses `cISC4NetworkOccupant::HasAnyNetworkFlag(mask)`.

The optional Dirt Road patch changes the immediate:

```cpp
0x449 -> 0xC49
```

The same stock road-like mask also appears in two other Windows sites:

| Address | Function | Instruction | Purpose |
| ---: | --- | --- | --- |
| `0x004BE6DC` | `cSC4ViewInputControlPlaceLot::GetLotFacingStreetCount` | `push 0x449` | Traffic network map lookup for frontage scoring |
| `0x006C1726` | `cSC4Lot_CalculateFerryTerminalRoadAccess` | `push 0x449` | Ferry terminal road-access special case |
| `0x006C1BD1` | `cSC4Lot_CalculateRoadAccess` | `push 0x449` | Main lot road-access scan |

The facing-street scorer also has a second immediate:

```asm
0x004BE70B: TEST EAX, 0x408
```

That scoring logic is:

```text
Avenue              -> +4
Road                -> +2
Street/OneWayRoad   -> +1
Other               -> +0
```

Therefore Dirt Road support at `GetLotFacingStreetCount` requires two patches:

```cpp
lookup mask:  0x449 -> 0xC49
scoring mask: 0x408 -> 0xC08
```

Patching only the lookup mask would let the helper find Dirt Road tiles but
still score them as zero frontage. The intended Dirt Road behavior is to count
as low-priority frontage, matching Street and OneWayRoad.

All Dirt Road patches are currently disabled by default in the prototype.

### Globals

The prototype uses these Windows global pointers:

```cpp
spLotManager         = 0x00B43D08
spTrafficNetworkMap = 0x00B43D54
```

### Traffic Network Map Shape

The ferry helper confirmed these vtable offsets:

```cpp
trafficNetworkMap->vtable[8] // vtable +0x20, lookup network info
entry->vtable[8]             // vtable +0x20, HasFlag-like check
```

The prototype uses the same shape:

```cpp
using GetNetworkInfoFn =
    void*(__thiscall*)(void*, int32_t x, int32_t z, uint32_t mask, bool);

using HasFlagFn =
    bool(__thiscall*)(void*, uint32_t flag);
```

The exclusion flag copied from the ferry-style logic is:

```cpp
0x00200000
```

## Proposed Algorithm

The hook runs after the stock function has made its own decision.

Pseudocode:

```cpp
bool HookCalculateRoadAccess(cISC4Lot* lot) {
    if (OriginalCalculateRoadAccess(lot)) {
        return true;
    }

    if (HasAdjacentTransitEnabledRoadAccess(lot)) {
        SetRoadAccessCache(lot, true);
        return true;
    }

    return false;
}
```

The exception check is:

```cpp
bool HasAdjacentTransitEnabledRoadAccess(cISC4Lot* sourceLot) {
    sourceBounds = sourceLot->GetBoundingRect();

    for each cell on the immediate perimeter of sourceBounds:
        candidate = LotManager->GetLot(x, z, true);
        deduplicate candidate;

    for each candidate:
        if candidate is sourceLot:
            continue;
        if candidate does not side-touch sourceLot:
            continue;
        if candidate does not have Transit Switch Point:
            continue;
        if candidate does not have road-like network contact:
            continue;

        return true;

    return false;
}
```

The road-like network contact check scans both the transit-enabled lot footprint
and its immediate perimeter:

```cpp
for z in teBounds.top - 1 .. teBounds.bottom + 1:
    for x in teBounds.left - 1 .. teBounds.right + 1:
        if TrafficNetworkMap has non-excluded entry at x,z for effective mask:
            return true;
```

This is deliberate. On-street TE lots may own the same cells that visually and
functionally contain the road. A pure perimeter-only scan may miss those lots.

## Why `GetLot(x,z,true)` Instead Of `GetLotsInCellRect`

There was uncertainty about whether `GetLotsInCellRect` returns lots that
partially overlap the rectangle or only lots fully contained in it. To avoid
depending on that behavior, the prototype queries exact perimeter cells with
`LotManager::GetLot`.

This has a few advantages:

- It matches how other game paths use cell-level lot lookup.
- It naturally handles large adjacent transit-enabled lots.
- It avoids false assumptions about rectangle containment semantics.
- It only inspects the immediate perimeter around the growable source lot.

The candidate list is stored as an unordered set of raw lot pointers because a
multi-tile adjacent lot can be returned from many perimeter cells. The set
reserves enough space for the source lot perimeter but can grow if a large or
unusual lot arrangement returns more unique neighbors than expected.

## Direct Adjacency Rule

Only direct side adjacency counts.

Allowed:

```text
Growable lot | TE lot | road-like network
```

Not allowed:

```text
Growable lot | TE lot | TE lot | road-like network
```

Chains are intentionally not supported for now. Supporting chains would require
a bounded graph or flood-fill search over transit-enabled lots, careful cycle
handling, and stricter rules about what counts as a valid terminal network.
That would increase the risk of granting access through decorative or unrelated
lots.

## Expected Behavior For Lots Beside A TE Lot

When the exception succeeds, the growable lot is treated as having road access
for the road-access cache. This should allow normal development checks that
depend on road access to pass.

The transit-enabled lot itself must still be functional:

- It must have a transit switch property.
- It must contact a road-like network according to the traffic network map.
- It must provide valid transit switch behavior for actual trip routing.

The hook does not create traffic paths, transit switch entries, or network
connectivity. It only changes the road-access result for the adjacent growable
lot.

## Zoning And Parcellization Notes

Drawn RCI zoning is handled by `cSC4ZoneDeveloper`, not by the plopped-lot
placement helper. The Mac symbol binary shows this rough flow:

```text
cSC4ZoneDeveloper::HighlightParcels(...)     preview path
cSC4ZoneDeveloper::DoParcellization(...)     commit/rebuild path
  -> OnZone(...)
     -> CreateParcellizableRegion(...)
     -> DetermineLotSize(...)
     -> optional ExtendAsStreets / LayInJogStreets
     -> SubdivideTheZone(...)
     -> for each subregion:
        -> ExamineBoundaries(...)
        -> Parcellize(...) or SimpleParcellize(...)
        -> CreateLot(...)
```

`ExamineBoundaries` scans the cells just outside each side of the drawn zone
region and calls `ExamineCell`. `ExamineCell` classifies and scores adjacent
networks using masks like:

```cpp
0xC49 // Road | Street | Avenue | OneWayRoad | DirtRoad
0x104 // Highway | LightRail
0x808 // Street | DirtRoad
0x401 // Road | OneWayRoad
0x040 // Avenue
0x002 // Rail
```

This means the zone/parcellization path already appears to know about Dirt Road
frontage. It probably does not know about transit-enabled lots as frontage,
which is why drawing zones directly toward an RTMT-style TE lot may still be a
minor UX issue. That can be treated separately from the core growth/access fix:
players can draw a normal road, draw zones, and then replace the road-facing
edge with the TE lot.

Confirmed Windows zoning-related functions:

| Windows address | Current name/status | Meaning |
| ---: | --- | --- |
| `0x0072A2A0` | `OOAnalyzer::cls_0x72a2a0::cSC4ZoneDeveloper::DrawStreet` | Automatic street drawing/insertion while zoning; uses `0xC49` repeatedly |
| `0x0072D200` | `cSC4ZoneDeveloper_CreateLotToFaceRoad` | Mapped from Mac `cSC4ZoneDeveloper::CreateLotToFaceRoad`; scans proposed lot sides for `0xC49` frontage and creates/orients toward the first matching side |
| `0x0072FAB0` | `cSC4ZoneDeveloper_ExamineCell` | Mapped from Mac `cSC4ZoneDeveloper::ExamineCell`; boundary scoring helper used by `ExamineBoundaries` while zoning/parcellizing |
| `0x0072FE80` | `cSC4ZoneDeveloper_ExistsNetworkConnectionForSide` | Helper used by `ExtendAsStreets` / `LayInJogStreets`; checks network existence and side connectivity, with special Street/DirtRoad fallback when mask includes `0x808` |
| `0x00731C80` | `ExamineBoundaries_` | Mapped from Mac `cSC4ZoneDeveloper::ExamineBoundaries`; scans outside the active zone region and calls `cSC4ZoneDeveloper_ExamineCell` |

The Windows `cSC4ZoneDeveloper_ExamineCell` score chain matches the Mac symbol
binary:

```text
0x104  Highway | LightRail      -> obstruction/boundary score +5
0x808  Street | DirtRoad        -> frontage score +30
0x401  Road | OneWayRoad        -> frontage score +100
0x040  Avenue                   -> frontage score +400
0x002  Rail                     -> obstruction/boundary score +5
```

This is why Dirt Road appears to already be part of drawn-zone frontage logic.
The missing RTMT behavior is different: a TE lot is not itself a network entry,
so `ExamineCell` will not naturally score it as road frontage without a
separate TE-aware zoning enhancement.

Still useful to locate in the Windows binary:

- `cSC4ZoneDeveloper::OnZone`
- `cSC4ZoneDeveloper::DoParcellization`
- `cSC4ZoneDeveloper::HighlightParcels`

## Interaction With Transit Switches

The forum testing supplied by the user supports several practical points:

- Transit switches can affect how Sims enter, leave, or change travel modes.
- Switch direction flags matter, and limited-direction switch entries can be
  placement-sensitive.
- `Ped/Car` in-to-out switches can create "car stealing" behavior.
- Network-enabled lots and transit-enabled lots should not be conflated.
- Lots can act as park-and-ride style intermediates when their transit switches
  are configured accordingly.

For this DLL, the important conclusion is that the access exception should not
try to infer detailed pathing behavior from switch contents. The exception only
needs to identify that an adjacent lot is transit-enabled and that it contacts
a valid road-like network. The traffic simulator and transit switch properties
remain responsible for actual movement.

## Implementation Notes

### Logging

The prototype uses `LOG_TRACE`, `LOG_DEBUG`, `LOG_INFO`, and `LOG_WARN`
extensively.

Current logs include:

- DLL startup and detected game version.
- Global pointer values.
- Effective road-like network mask.
- Hook installation and trampoline address.
- Original function call sampling.
- Original road-access failures.
- Adjacent lot scan bounds.
- Transit switch property location.
- Traffic network map hits.
- Successful TE exceptions.
- Shutdown counters.

Because `CalculateRoadAccess` is a hot function, trace logging can become noisy.
The current sampling helper logs early calls and then periodic calls:

```cpp
count <= 64 || (count % 1024) == 0
```

For early validation, high verbosity is useful. For release builds, the log
level should probably be reduced.

### Ref Counting

Do not wrap the raw `cISC4Lot*` returned by `LotManager::GetLot` in
`cRZAutoRefCount` unless the ownership contract is proven.

The local SDK's `cRZAutoRefCount(T*)` constructor does not necessarily AddRef
before destruction. If used incorrectly, it could Release a pointer that this
code does not own. The prototype follows the game's own nearby usage pattern:
query, inspect immediately, and do not retain the pointer.

### Cache Ordering

The hook calls the original function first. This means the original function may
write the cache to false before the exception runs. On exception success, the
prototype writes the cache back to true with the same helper used by the stock
function.

This is preferable to bypassing the stock function because:

- Stock success behavior remains unchanged.
- Stock failure behavior remains available as the baseline.
- The hook only mutates the result in the narrow exception case.

### Traffic Simulator Split

The traffic simulator has at least three relevant layers:

- `CalculateRoadAccess` decides whether the lot passes the access gate.
- `GetConnectedDestinationCount` decides whether the lot has any connected
  destinations for a given purpose.
- `GetSubnetworksForLot` exposes the lot's actual subnetwork membership.

The current prototype now hooks all three layers conceptually, but they are not
equivalent:

- `CalculateRoadAccess` can succeed while `GetSubnetworksForLot` still returns
  empty.
- `GetConnectedDestinationCount` can be nonzero because of a TE-adjacent
  fallback, even when the source lot itself is not in the graph.
- The route query tool appears to depend on real subnetwork membership, not
  just a nonzero destination count.

That is why the source lot can lose the zot while still showing no path in the
route query UI.

## Verification Checklist

### Binary Verification

Before trusting a build, verify the Windows binary assumptions:

- `0x006C1A30` still begins with `55 8B EC 83 E4 F8`.
- The 6-byte overwrite does not split a required instruction.
- The trampoline jumps back to `0x006C1A36`.
- `0x004BE6DC` is still `push 0x449`.
- `0x004BE70B` is still `test eax, 0x408`.
- `0x006C1726` is still `push 0x449`.
- `0x006C1BD1` is still `push 0x449`.
- `0x006C0F30` still matches the road-access cache helper call shape.
- `spLotManager` at `0x00B43D08` is non-null in city context.
- `spTrafficNetworkMap` at `0x00B43D54` is non-null in city context.
- Traffic network map lookup still sits at vtable slot 8.
- Traffic network entry flag check still sits at vtable slot 8.

### Build Verification

The current macOS environment cannot directly configure the Win32 target with
the repo's CMake settings because `CMAKE_GENERATOR_PLATFORM Win32` is a Visual
Studio/Windows generator concept. Build verification should happen in the
intended Win32 build environment.

Expected target:

```text
SC4TransitAccess.dll
```

The DLL should export:

```cpp
RZGetCOMDllDirector
```

### Runtime Log Verification

In a city, the log should show:

- `TransitAccess: OnStart`
- Detected game version `641`
- Non-null lot manager and traffic network map once city systems are ready
- Installed road-access hook
- Some original road-access failures
- For the target case, at least one:

```text
TransitAccess: TE road-access exception success
```

If testing Dirt Road support with the option enabled, the log should also show:

```text
TransitAccess: installed DirtRoad mask patches successfully
```

When the option is disabled, it should instead log that the Dirt Road mask
patches are disabled and the effective mask should remain `0x449`.

## In-Game Test Plan

Use a small, controlled city with minimal plugins first. The goal is to isolate
growth/access behavior before testing realistic NAM/RTMT setups.

### Baseline Without The DLL

1. Create a straight road or street.
2. Place an RTMT-style on-street bus/subway stop.
3. Zone a small residential lot directly beside the TE lot where the TE lot
   separates the growable from direct road access.
4. Let the simulation run.
5. Confirm the growable does not develop or reports no road access.

This establishes the failing case.

### Same Case With The DLL

1. Install the DLL.
2. Load the same city or recreate the same layout.
3. Run the simulation.
4. Check that the growable develops or no longer reports missing road access.
5. Confirm the log records a TE exception success for the relevant lot.

### Negative Control: No TE Property

1. Place a non-transit lot beside the growable.
2. Put a road-like network beside that non-transit lot.
3. Confirm the growable does not receive access through the non-transit lot.

This confirms the exception is not simply granting access through any lot.

### Negative Control: TE Lot Without Road-Like Contact

1. Place a transit-enabled lot that does not touch a road-like network.
2. Zone a growable beside it.
3. Confirm no access is granted.

This confirms the traffic network map contact check matters.

### Negative Control: TE Chain

1. Place a growable beside TE lot A.
2. Place TE lot A beside TE lot B.
3. Place TE lot B beside the real road-like network.
4. Confirm no access is granted unless TE lot A itself has road-like contact.

This confirms chains are not accidentally supported.

### Dirt Road Option

Run this only if the Dirt Road option is enabled in the build.

1. Place a NAM Dirt Road.
2. Place the target TE lot on or beside the Dirt Road.
3. Place a growable beside the TE lot.
4. Confirm access is granted.
5. Confirm the same build also grants direct stock-style road access beside
   Dirt Road where expected.

Then repeat with the option disabled and confirm Dirt Road does not become
valid access through this DLL.

### Traffic Simulator Sanity Test

After access/growth succeeds:

1. Provide real jobs reachable through the connected road/transit system.
2. Let the city run for several simulation months.
3. Use the traffic query tool to inspect commute paths.
4. Confirm Sims can leave the grown lot and route through valid networks or
   valid transit switches.

If growth succeeds but Sims cannot commute, the access exception is working but
the transit switch/pathfinding configuration still needs separate attention.

## Risks And Open Questions

### TE Property Location

The prototype checks both lot and building property holders. This should cover
the likely cases, but custom content could still encode TE behavior in unusual
ways. If logs show expected RTMT lots being skipped as non-TE, inspect the DAT
with the DBPF tooling and verify where `0xE90E25A1` is stored.

### Traffic Network Map Lookup Semantics

The traffic network map lookup is inferred from the ferry terminal logic. This
is a strong clue, but it still relies on Windows vtable slot assumptions. If
the game crashes or returns unexpected entries, this should be rechecked in the
Windows binary.

### Exclusion Flag Semantics

The exclusion flag `0x00200000` is copied from the ferry-style check. The
working interpretation is that entries with this flag should not count as
usable road-like contact. This should be validated with logging around known
network tiles and excluded/placeholder tiles.

### Lot Bounds And Coordinates

The algorithm uses `SC4Rect<int32_t>` bounds and scans inclusive edges. If the
game's lot bounds are inclusive, this is correct. If any lot reports unusual
bounds, logs should make that visible.

### Candidate List Cost

The adjacent candidate list is a dynamically sized `std::unordered_set` of raw
`cISC4Lot*` pointers. It reserves based on the source lot perimeter and
deduplicates by pointer identity. This avoids a hard cap and avoids repeated
linear scans. The expected candidate count is still small because the scan only
covers the immediate perimeter around one source lot.

### Hot-Path Cost

The hook runs inside `CalculateRoadAccess`. The expensive exception path only
runs after stock road access fails, which should limit cost. Still, heavy trace
logging can dominate runtime during validation.

### Dirt Road Scope

Adding Dirt Road to the road-like mask is targeted. Adding Highway or
GroundHighway would be a different design decision and should not be bundled
into this fix without explicit testing.

### Traffic Path Availability

This mod does not guarantee that Sims can actually commute. The current stage
now covers both the road-access gate and the destination-count gate, but the
route graph can still be incomplete until the source lot receives real or
synthesized subnetwork membership. Actual traffic behavior still depends on:

- Transit switch point entries.
- Transit switch directions.
- Network connectivity.
- Simulator settings.
- Congestion and path costs.
- Subnetwork assignment for the source lot.
- Whether the caller uses `GetSubnetworksForLot` or a deeper route graph query.

## Current Confidence

The overall approach is plausible because it follows existing game behavior:

- It preserves stock `CalculateRoadAccess` first.
- It uses the stock road-access mask as the default.
- It models the exception after the ferry terminal's use of the traffic network
  map.
- It writes the same road-access cache that the stock function writes.
- It avoids broad TE-chain behavior.

The main remaining uncertainty is not the high-level idea, but the exact
runtime behavior of the Windows traffic network map calls and property-holder
coverage across real RTMT lots. Those should be validated with logs in a small
test city before reducing verbosity or broadening the feature.

## Traffic Simulator Findings

The destination-count hook confirmed an important split between access
eligibility and actual route graph membership.

Observed runtime behavior:

- A growable lot beside a TE lot can have `CalculateRoadAccess` fail stock and
  then succeed through the TE exception.
- Even when that happens, `cSC4TrafficSimulator::GetConnectedDestinationCount`
  may still return zero for the source lot unless we synthesize TE-adjacent
  subnetworks.
- The source lot's `GetSubnetworksForLot` result can remain empty while the TE
  station itself reports a valid subnetwork, for example `[3]`.
- In the `R$1_1x2 at (38, 52)` test case, the TE fallback returned a nonzero
  destination count (`27`), which cleared the zot, but the route query tool
  still showed no route until the source lot's subnetworks were also
  synthesized.
- After adding the `GetSubnetworksForLot` fallback, the source lot can be
  exposed as a participant in the TE station's subnetwork list instead of only
  passing the destination-count gate.

Interpretation:

- The road-access cache alone is not sufficient for commute/path visibility.
- `GetConnectedDestinationCount` is the access gate, but it is not the source
  of truth for the route query tool.
- `GetSubnetworksForLot` is the more relevant source-data path when the goal is
  to make the lot behave like a real participant in the traffic graph.
- The next likely patch point is `UpdateSubnetworkInfo` or a hook that
  synthesizes subnetworks for TE-adjacent lots when the stock source lot has
  none.

Current practical conclusion:

- `CalculateRoadAccess` hook: fixes growth/access eligibility.
- `GetConnectedDestinationCount` fallback: fixes the zot / job-access gate.
- `GetSubnetworksForLot` fallback: starts exposing the TE connectivity to code
  that reads subnetwork membership.
- `UpdateSubnetworkInfo`: still the likely final source-side fix if the route
  query tool or commute simulation needs the lot to be a genuine subnetwork
  member instead of a synthesized proxy.
