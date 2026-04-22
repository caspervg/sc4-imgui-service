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

- `src/sample/transit-access/TransitAccessSampleDirector.cpp`
- `CMakeLists.txt`, target `SC4TransitAccessSample`

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

The traffic simulator does not simply ask `HasRoadAccess()` to decide every
trip. `cSC4TrafficSimulator::SetupTrip` and
`cSC4TrafficSimulator::SetupPathFinderForLot` configure a `cSC4PathFinder`,
and the pathfinder then seeds its starting queue from cells around the lot's
traffic-source rectangle.

The critical Mac symbol binary functions are:

| Mac address | Function | Meaning |
| ---: | --- | --- |
| `0x000531C4` | `cSC4TrafficSimulator::SetupTrip` | Configures trip/pathfinder parameters and destination rectangles |
| `0x00059780` | `cSC4TrafficSimulator::SetupPathFinderForLot` | Uses `lot->AsTrafficSource()->GetSourceCells(...)` as the pathfinder source rectangle |
| `0x00053726` | `cSC4TrafficSimulator::GetNetworkInfo` | Returns real network info for a network type/cell |
| `0x00053D40` | `cSC4TrafficSimulator::GetNeighborPoolPropertyHolder` | Returns a transit-neighbor property holder for switch/destination checks |
| `0x0009B804` | `cSC4PathFinder::FindPath` | Main pathfinder expansion loop |
| `0x0009B09C` | `cSC4PathFinder::CreateStartNodes` | Seeds the initial priority queue from real traffic network cells around the source rectangle |
| `0x0009AF00` | `cSC4PathFinder::AddTripNode` | Adds a non-root node during path expansion |
| `0x0009911A` | `cSC4PathFinder::AtGoal` | Checks whether a popped node satisfies the destination, including transit-neighbor property holders |
| `0x00099934` | `cSC4PathFinder::FindNearestStandardDest` | Chooses standard destination cells for normal trips |

`CreateStartNodes` is the important discovery. It scans rings around the source
rectangle and calls `cSC4TrafficSimulator::GetNetworkInfo(networkType, x, z)`.
It only pushes starting nodes when it sees a real network info record plus
legal edge/path bits. Transit-enabled lots without real network info are
therefore invisible as starting attachments, even if they have valid transit
switches and even if `CalculateRoadAccess` was patched to return true.

The main `FindPath` expansion loop is more capable than the start-node logic.
During normal expansion it uses a transit-switch occupancy grid from the
traffic simulator and can step into a transit-switch cell even when
`GetNetworkInfo` returns null. The failure is specifically that trips cannot
start from those transit-switch cells.

This changes the responsibility split:

- The road-access hook grants growth/access eligibility.
- A separate pathfinder hook is needed to let trips attach to adjacent
  transit-enabled lots.
- Dirt Road mask patching remains separate and does not need changes.

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

### Traffic Pathfinder Start Nodes

The Windows binary now has the relevant pathfinder functions identified:

| Windows address | Current name/status | Meaning |
| ---: | --- | --- |
| `0x006D91B0` | `cSC4PathFinder_FindPath` | Mapped from Mac `cSC4PathFinder::FindPath`; calls standard-destination search, then `CreateStartNodes`, then expands the queue |
| `0x006D8A90` | `cSC4PathFinder_CreateStartNodes` | Mapped from Mac `cSC4PathFinder::CreateStartNodes`; scans around the source rectangle and seeds root trip nodes from real network cells only |
| `0x006D7730` | `cSC4PathFinder_TripNodeHashMap_GetNode` | Mapped from Mac `cSC4PathFinder::TripNodeHashMap::GetNode`; returns or allocates a `TripNode` for a packed node key |
| `0x006D88B0` | `cSC4PathFinder_TripNodePriorityQueue_Push` | Mapped from Mac `cSC4PathFinder::TripNodePriorityQueue::Push`; inserts a node into the open priority queue |
| `0x006D6460` | `cSC4PathFinder_TripNodePriorityQueue_UpdateNode` | Mapped from Mac priority queue update helper used when an already-open node gets a better score |
| `0x006D5DF0` | `cSC4PathFinder_DecodeTripNodeKey` | Decodes a packed trip-node key into mode/x/z fields |
| `0x006D6160` | `cSC4PathFinder_GetHeuristicCost` | Returns the destination heuristic component for an x/z cell |
| `0x006D8FA0` | `cSC4PathFinder_AddTripNode` | Mapped from Mac `cSC4PathFinder::AddTripNode`; adds non-root nodes during expansion |
| `0x006D7960` | `cSC4PathFinder_AtGoal` | Mapped from Mac `cSC4PathFinder::AtGoal`; checks destination lots, neighbor-pool holders, and special destination modes |
| `0x006D7E50` | `cSC4PathFinder_FindNearestStandardDest` | Mapped from Mac `cSC4PathFinder::FindNearestStandardDest` |
| `0x0070EC40` | `cSC4TrafficSimulator_SetupTrip` | Mapped from Mac `cSC4TrafficSimulator::SetupTrip`; configures a pathfinder from `StandardTripParams` and `cSC4TripData` |
| `0x0070FB30` | `cSC4TrafficSimulator_GetNetworkInfo` | Mapped from Mac `cSC4TrafficSimulator::GetNetworkInfo`; returns real network info for a network type/cell |
| `0x007114E0` | `cSC4TrafficSimulator_GetNeighborPoolPropertyHolder` | Mapped from Mac `cSC4TrafficSimulator::GetNeighborPoolPropertyHolder`; returns the transit-neighbor property holder used by `AtGoal` |
| `0x00711610` | `cSC4TrafficSimulator_SetupPathFinderForLot` | Mapped from Mac `cSC4TrafficSimulator::SetupPathFinderForLot`; configures a pathfinder for a specific source lot |
| `0x0071CB20` | `cSC4TrafficSimulator_TransitSwitchEntryCost` | Mapped from Mac `cSC4TrafficSimulator::TransitSwitchEntryCost`; computes cost for entering a transit switch by switch key |
| `0x0071D020` | `cSC4TrafficSimulator_WorkOnReRouteTrips` | Mapped from Mac `cSC4TrafficSimulator::WorkOnReRouteTrips`; one caller of `SetupTrip` |
| `0x0071D440` | `cSC4TrafficSimulator_WorkOnFullTrips` | Mapped from Mac `cSC4TrafficSimulator::WorkOnFullTrips`; one caller of `SetupTrip` |

Verified Windows prologue bytes:

```text
0x006D8A90: 83 EC 5C 53 55 56 8B F1 ...
```

A 6-byte inline hook at `0x006D8A90` copies complete instructions:

```asm
sub esp, 0x5c
push ebx
push ebp
push esi
```

The trampoline should jump back to:

```cpp
0x006D8A96
```

### Traffic Pathfinder Source-Lot Setup

The stripped Windows function at `0x00711610` has enough overlap with the Mac
symbol `cSC4TrafficSimulator::SetupPathFinderForLot` to rename it confidently.
The decisive pattern is:

- stack argument 1 is the `cISC4PathFinder*`;
- stack argument 2 is the `cISC4Lot*`;
- `ECX` is the traffic simulator instance;
- the lot vtable slot `+0x10` returns a traffic source;
- traffic-source vtable slot `+0x14` fills the source-cell rectangle;
- pathfinder vtable slot `+0x4C` receives that rectangle;
- pathfinder vtable slot `+0x34` is then called with `0x1CF, 0` and `3, 1`;
- the function returns a boolean in `AL`.

The interface/data reference to this function is at `0x00AB33EC`. Ghidra may
display the stripped function as `__stdcall`; the disassembly shows the
interface-style `this` pointer in `ECX`.

Hook shape:

```cpp
using SetupPathFinderForLotFn =
    bool(__thiscall*)(void* trafficSimulator, void* pathFinder, cISC4Lot* lot);

bool __fastcall HookSetupPathFinderForLot(
    void* trafficSimulator,
    void*,
    void* pathFinder,
    cISC4Lot* lot);
```

The hook should not change pathfinder behavior directly. Its best use is
bookkeeping: call the original, then record `pathFinder -> sourceLot` for the
later `CreateStartNodes` fallback.

Cleanup should be tied to actual hook use rather than to speculative ownership
of game objects:

- On setup success, overwrite the side-table entry for that `pathFinder`.
- On setup failure, erase any stale entry for that `pathFinder`.
- In the `CreateStartNodes` hook, erase the entry after the original/retry path
  consumes it.
- Keep the stored value as a raw `cISC4Lot*`; do not AddRef/Release unless a
  real ownership contract is proven.

The Windows pathfinder destructor chain is identified as:

| Windows address | Current name/status | Meaning |
| ---: | --- | --- |
| `0x006D61B0` | `cSC4PathFinder_Release` | Refcount release; calls vtable `+0x84` when the count reaches zero |
| `0x006D7050` | `cSC4PathFinder_DeletingDestructor` | Calls the real destructor, then `operator_delete` when requested |
| `0x006D6CA0` | `cSC4PathFinder_Destructor` | Destroys the pathfinder containers/strings |

A destructor hook could be used as a backstop to erase side-table entries, but
the first implementation should not need it if `SetupPathFinderForLot` and
`CreateStartNodes` keep the map bounded.

### Traffic Simulator Trip Setup

The stripped Windows function at `0x0070EC40` matches Mac
`cSC4TrafficSimulator::SetupTrip(StandardTripParams&, cSC4TripData&)`.
Important overlap:

- `param_1 + 0x08` is the primary `cISC4PathFinder*`;
- `param_1 + 0x0C` is another pathfinder-like object configured in parallel;
- `param_1 + 0x10` is the traffic source used to fill the source rectangle;
- trip byte `+0x1C` controls the travel mode;
- trip byte `+0x1D` selects a cost table/wealth or destination variant;
- trip byte `+0x1E` contains pathfinder flags;
- trip byte `+0x1F` selects a special destination branch;
- trip words `+0x20/+0x22` are destination cell coordinates for special trips.

The function calls pathfinder vtable slots `+0x48`, `+0x24`, `+0x30`,
`+0x28`, `+0x54`, `+0x4C`, `+0x1C`, `+0x3C`, `+0x40`, `+0x44`, and `+0x34`.
The familiar mask setup appears here too:

```cpp
// pathfinder vtable +0x34
call(0x1CF, 0);
call(1 << tripData[0x1D], 1);
```

This is useful context, but it is not the preferred hook for RTMT start access.
It configures the pathfinder for many trip types and does not receive the
source `cISC4Lot*` as directly as `SetupPathFinderForLot`.

The only direct Windows callers found so far match the Mac symbol binary:

| Windows call site | Windows function | Mac equivalent |
| ---: | --- | --- |
| `0x0071D233` | `cSC4TrafficSimulator_WorkOnReRouteTrips` | `cSC4TrafficSimulator::WorkOnReRouteTrips` calls `SetupTrip` at `0x00064FA1` |
| `0x0071D636` | `cSC4TrafficSimulator_WorkOnFullTrips` | `cSC4TrafficSimulator::WorkOnFullTrips` calls `SetupTrip` at `0x0006544B` |

Observed Windows pathfinder field offsets from the decompiler:

| Offset | Meaning |
| ---: | --- |
| `+0x0C` | `cSC4TrafficSimulator*` |
| `+0x18` | source rectangle min x |
| `+0x1C` | source rectangle min z |
| `+0x20` | source rectangle max x |
| `+0x24` | source rectangle max z |
| `+0x38..+0x44` | expanded heuristic/source envelope derived from the source rectangle |
| `+0x64` | enabled/goal travel-mode mask |
| `+0x66` | allowed start travel-mode mask |
| `+0x70` | pathfinder flags |
| `+0x80` | destination type |
| `+0x84` | destination/occupancy mask used by standard destination search |
| `+0x90` | optional start-cost table; otherwise uses simulator table |
| `+0xC8..+0xE8` | per-mode rejection counters used by `AddTripNode` |
| `+0xEC` | per-mode reject/disable mask set after repeated over-budget attempts |
| `+0x114` | trip-node priority queue |
| `+0x120` | priority queue size/count |
| `+0x128` | trip-node hash map |
| `+0xB8` | city width |
| `+0xBC` | city height |
| `+0xC4` | resume/continuation flag |

Relevant Windows traffic-simulator offsets seen through `FindPath`:

| Offset | Meaning |
| ---: | --- |
| `+0x58` | per-cell path/edge data table, indexed as `(x << 8 | z) * 0x2E` |
| `+0x7C` | real network-info grid used by `cSC4TrafficSimulator_GetNetworkInfo` |
| `+0x88/+0x94/+0xA0/+0xAC` | boundary/neighbor-connection rows used by `GetNeighborPoolPropertyHolder` |
| `+0xB8` | property-holder map used by `GetNeighborPoolPropertyHolder` |
| `+0xC4` | default property holder returned by `GetNeighborPoolPropertyHolder` |
| `+0x164` | vector of six congestion/speed grids |
| `+0x700` | second switch-related grid acquired before expansion; return value not used in the observed path |
| `+0x704` | transit-switch occupancy grid used by `FindPath` |
| `+0x70C` | transit-switch hash map used by `TransitSwitchEntryCost` |
| `+0x720` | transit-switch entry-cost scale factor |
| `+0x7A4` | pathfinder mode flag that narrows allowed mode mask |
| `+0xC50` | maximum start-node ring scan radius |
| `+0xC78` | default start-cost table |
| `+0xCF8` | standard destination heuristic scale |

The Mac offsets for the two switch-related grids are `+0x718` and `+0x71C`.
The Windows equivalents are `+0x700` and `+0x704`. The `+0x704` grid is the
one assigned to the local pointer and consulted throughout `FindPath`.

### Traffic Switch Traversal

The normal pathfinder expansion is more TE-aware than `CreateStartNodes`.
The key Windows flow in `cSC4PathFinder_FindPath` is:

1. Load the transit-switch occupancy grid from simulator `+0x704`.
2. Pop a `TripNode` from the priority queue.
3. If the current cell is in bounds, first test whether the occupancy grid has
   an entry at `x,z`.
4. Use `cSC4TrafficSimulator_GetNetworkInfo(networkType, x, z)` at
   `0x0070FB30` when real network info is needed.
5. If real network info is absent but the occupancy grid has an entry, apply
   `cSC4TrafficSimulator::TransitSwitchEntryCost` and enqueue the switch step.
6. Use `cSC4TrafficSimulator_GetNeighborPoolPropertyHolder` at `0x007114E0`
   inside `AtGoal` to validate destination capacity/property-holder state for
   transit-neighbor cells.

This confirms that the simulator can traverse transit-switch cells once a path
already exists in the queue. The missing piece for RTMT-adjacent growables is
therefore the initial queue seed, not general transit-switch traversal.

The Windows `CreateStartNodes` root-node insertion path calls:

```cpp
0x006D7730 // cSC4PathFinder_TripNodeHashMap_GetNode(pathFinder + 0x128, key)
0x006D88B0 // cSC4PathFinder_TripNodePriorityQueue_Push(pathFinder + 0x114, node)
```

Root node keys use the same shape as the Mac code:

```cpp
0x10000000 | (edge << 24) | (travelMode << 16) | (x << 8) | z
```

The root node fields then look like:

```cpp
node->parent = 0;
node->cost = startCost;
node->score = startCost + heuristic;
node->closed = false;     // byte at +0x11
node->queued = true;      // byte at +0x10
node->entryEdge = edge;   // byte at +0x12
```

More complete `TripNode` layout observed so far:

| Offset | Meaning |
| ---: | --- |
| `+0x00` | packed node key |
| `+0x04` | parent `TripNode*`; null for root nodes |
| `+0x08` | accumulated path cost |
| `+0x0C` | priority score, cost plus heuristic |
| `+0x10` | queued/open flag |
| `+0x11` | closed/processed flag |
| `+0x12` | entry edge/direction used for path-data edge bits |
| `+0x13` | travel mode/network byte decoded from the key |
| `+0x14` | decoded x coordinate, `uint16_t` |
| `+0x16` | decoded z coordinate, `uint16_t` |
| `+0x18` | next node in hash bucket chain |
| `+0x1C` | hash bucket index |
| `+0x20` | priority-queue heap index |

Nodes are allocated in `0x24000` byte blocks and the allocator advances by
`0x24`, so the node size is `0x24` bytes. The hash map at `pathFinder + 0x128`
uses `0xC07` buckets and hashes the packed key with `key * -0x61C8864F`.

Important static arrays from the Windows binary:

```cpp
// 0x00AB20D8 / 0x00AB20E8
kDX = {-1,  0, +1,  0};
kDZ = { 0, -1,  0, +1};

// 0x00AB20F8, indexed by travel mode 0..8
kTravelModeToTransitNetwork = {0, 1, 1, 2, 1, 2, 3, 4, 5};
```

`TransitSwitchEntryCost` at `0x0071CB20` has this effective Windows behavior:

```cpp
float __thiscall TransitSwitchEntryCost(void* trafficSimulator, uint32_t switchKey);
```

It indexes the transit-switch hash map at simulator `+0x70C`. For the returned
switch record, it computes `usage / capacity`. If the ratio is at least `5.0`,
it returns a huge cost. Otherwise it returns:

```cpp
switchBaseCost * congestionToInverseSpeedCurve(ratio) * *(float*)(sim + 0x720)
```

The switch record fields used here match the Mac symbol binary:

| Offset | Meaning |
| ---: | --- |
| `+0x04` | usage/current volume |
| `+0x08` | base transit-switch cost |
| `+0x0C` | capacity |

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

The first prototype does not create traffic paths, transit switch entries, or
network connectivity. It only changes the road-access result for the adjacent
growable lot. Testing on a real Windows SC4 install showed that this is not
enough for tangible commute behavior: the traffic/pathfinder layer still cannot
attach trips to the adjacent TE lot.

The missing behavior is now believed to be start-node creation, not road-access
caching. A lot beside a TE lot needs to be treated as if it can start at, or
just beyond, the adjacent transit-enabled lot.

## Pathfinder Hook Options

### Option A: Retry `CreateStartNodes` Through The Adjacent TE Lot

This is the lower-risk prototype.

Flow:

1. Hook Windows `cSC4PathFinder_CreateStartNodes` at `0x006D8A90`.
2. Call the original first.
3. If the original succeeds, return true.
4. If the original fails, identify the source lot from the side table
   populated by the `SetupPathFinderForLot` hook at `0x00711610`.
5. Find side-touching adjacent TE lots using the same property checks as the
   road-access hook.
6. For each adjacent TE lot that has road-like contact, temporarily replace the
   pathfinder source rectangle at `+0x18..+0x24` with the TE lot's bounds.
7. Call the original `CreateStartNodes` trampoline again.
8. Restore the original source rectangle and return the retry result.

Why this is attractive:

- It reuses Maxis' own root-node creation code.
- It avoids direct writes to the pathfinder's trip-node hash map and priority
  queue.
- It keeps legal starting edge checks, path info checks, speed/cost setup, and
  mode filtering inside stock code.
- It only runs after normal start-node creation fails.

Verification notes:

- `CreateStartNodes` returns false only when it has not pushed any root nodes,
  so a retry should not need to clear partial root-node state.
- The function reads the source rectangle from pathfinder offsets
  `+0x18..+0x24` throughout its scan. Temporarily replacing those four fields
  is therefore enough to redirect the stock perimeter scan.
- `FindPath` computes an expanded heuristic envelope at `+0x38..+0x44` before
  calling `CreateStartNodes`. A retry with only `+0x18..+0x24` changed will
  reuse the original envelope. That can affect root-node scoring, but it should
  not invalidate root nodes. For an immediately adjacent TE lot, the difference
  should usually be small.
- The source lot should be recovered from the `SetupPathFinderForLot` side
  table. Scanning the current source rectangle with `LotManager::GetLot(x,z,
  true)` remains a fallback only, because it is less explicit and can become
  ambiguous if the source rectangle does not map cleanly to the intended lot.

Limitations:

- The route will effectively start from a real network cell adjacent to the TE
  lot, not from the growable lot cell itself.
- It may undercount or ignore the small walk segment through the TE lot.
- It only works when stock `CreateStartNodes` can find real network cells
  around the TE lot. If a TE lot only exposes transit-switch cells and no real
  network info near its perimeter, this fallback can still fail.
- It may allow access through a TE lot even if the switch directions are not
  semantically ideal, because the stock retry is looking for nearby real network
  start nodes rather than validating a full transit-switch path from the
  growable side.

Despite those limitations, this is the safest next experiment because it avoids
constructing internal pathfinder nodes by hand.

### Option B: Inject Synthetic Start Nodes On Transit-Switch Cells

This is the more accurate but riskier option.

Flow:

1. Hook Windows `cSC4PathFinder_CreateStartNodes` at `0x006D8A90`.
2. Call the original first.
3. If the original fails, scan the source lot perimeter for adjacent
   transit-switch cells, preferably constrained to side-touching TE lots.
4. Consult the traffic simulator's transit-switch occupancy grid at
   simulator `+0x704`.
5. For suitable cells and modes, create root trip nodes directly:
   - call `0x006D7730` to get/create a trip-node hash entry;
   - initialize parent/cost/score/edge fields;
   - call `0x006D88B0` to push the node into the priority queue.

Groundwork from the Windows binary:

```cpp
using TripNodeHashMapGetNodeFn =
    void*(__thiscall*)(void* tripNodeHashMap, uint32_t key);        // 0x006D7730
using TripNodePriorityQueuePushFn =
    void(__thiscall*)(void* priorityQueue, void* tripNode);         // 0x006D88B0
using TransitSwitchEntryCostFn =
    float(__thiscall*)(void* trafficSimulator, uint32_t switchKey); // 0x0071CB20
```

The pathfinder subobjects are:

```cpp
auto* queue = reinterpret_cast<uint8_t*>(pathFinder) + 0x114;
auto* hashMap = reinterpret_cast<uint8_t*>(pathFinder) + 0x128;
```

A synthetic root insertion would look roughly like:

```cpp
uint32_t key =
    0x10000000u |
    (static_cast<uint32_t>(entryEdge) << 24) |
    (static_cast<uint32_t>(travelMode) << 16) |
    (static_cast<uint32_t>(x & 0xff) << 8) |
    static_cast<uint32_t>(z & 0xff);

auto* node = static_cast<uint8_t*>(TripNodeHashMapGetNode(hashMap, key));
float cost = startCost[travelMode] + optionalTransitSwitchEntryCost;
float heuristic = IsOutsideEnvelope(pathFinder, x, z)
    ? static_cast<float>(xHeuristic[x] + zHeuristic[z])
    : 0.0f;

*reinterpret_cast<void**>(node + 0x04) = nullptr;
*reinterpret_cast<float*>(node + 0x08) = cost;
*reinterpret_cast<float*>(node + 0x0C) = cost + heuristic * pathFinderScale;
node[0x11] = 0;
node[0x10] = 1;
node[0x12] = entryEdge;

TripNodePriorityQueuePush(queue, node);
```

The most plausible candidate selection is:

1. Use the `SetupPathFinderForLot` side table to identify the source lot.
2. Find side-touching adjacent TE lots.
3. Inspect only TE-lot cells on the side that touches the source lot, not
   arbitrary cells inside the TE lot.
4. Read the transit-switch occupancy grid at simulator `+0x704`; the grid cell
   stores a nonzero switch key when a transit switch occupies that cell.
5. For each nonzero switch key, derive `entryEdge` from the side between the
   TE cell and the source lot using `kDX/kDZ`.
6. Loop travel modes `0..8`, constrained by `pathFinder +0x66`.
7. Check the path/edge data table at simulator `+0x58` for the chosen
   travel-mode/network and `entryEdge`, mirroring the stock
   `CreateStartNodes` test:

```cpp
if ((pathInfoForMode & (0xF << (entryEdge * 4))) == 0) {
    skip;
}
```

8. Include `TransitSwitchEntryCost(sim, switchKey)` in the root cost unless
   testing proves the game charges that cost elsewhere after a switch-cell root
   node is inserted.

The switch-grid lookup shape seen in `FindPath` is:

```cpp
void* gridHolder = *reinterpret_cast<void**>(sim + 0x704);
void* matrix = gridHolder->vtable[0x60 / 4](gridHolder);
uint8_t* rows = *reinterpret_cast<uint8_t**>(matrix);
uint8_t* row = rows + x * 0x0C;          // std::vector-like row object
uint32_t* rowValues = *reinterpret_cast<uint32_t**>(row);
uint32_t switchKey = rowValues[z];
```

This should be wrapped carefully and treated as provisional until tested in
the Windows game. Bad bounds checks here would be far more dangerous than the
current Option A source-rectangle retry.

Why this is attractive:

- It models the actual missing concept: starting at a transit-switch cell.
- It does not need to pretend the TE lot is a source rectangle.
- It can work even when there is no real network info directly around the TE
  lot, provided the switch grid and path data can lead to a valid route.

Risks:

- It depends on internal `cSC4PathFinder::TripNode` layout.
- It depends on the hash-map and priority-queue helper addresses.
- It must calculate valid starting edges and initial costs correctly.
- It must avoid duplicate queue entries for the same root key. Stock
  `CreateStartNodes` effectively creates distinct root keys; a synthetic scan
  should keep a small local `seenKeys` set before pushing.
- It probably needs to respect switch directionality more strictly than Option
  A. The path-data edge test is the first guard, but it may not fully capture
  building-exemplar transit-switch direction semantics.
- It changes pathfinder internal state directly, so a crash or bad path is more
  likely than with the current retry-through-TE-lot approach.
- Incorrect root nodes could produce bad paths, non-reversible trips, or
  unstable queue/hash-map state.

This should be considered only if Option A does not produce useful commute
behavior.

### Option C: Hook `SetupPathFinderForLot` For Source-Lot Bookkeeping

`cSC4TrafficSimulator_SetupPathFinderForLot` is now identified in the Windows
binary at `0x00711610`. This hook is not a separate functional fix; it is
support infrastructure for Option A.

The best use of this hook is:

- call the original setup function first;
- when setup succeeds, record `pathFinder -> sourceLot`;
- when setup fails, remove any stale record for that pathfinder;
- let `CreateStartNodes` use this side table when stock root-node creation
  fails;
- erase the side-table entry after `CreateStartNodes` has consumed it.

This is safer than recovering the source lot indirectly from
`pathFinder +0x18..+0x24`. The source rectangle is only the lot's traffic-source
footprint, while `SetupPathFinderForLot` receives the exact `cISC4Lot*` that
the simulator is preparing.

The hook should not replace the source rectangle, expand `GetSourceCells`, or
otherwise change pathfinder setup. Merely expanding the source rectangle can
put nearby real network cells inside the source rectangle rather than on its
boundary, which may still fail stock start-node seeding and could affect
destination heuristics. The actual behavior change should stay in the
`CreateStartNodes` fallback.

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

The new pathfinder finding refines this: the traffic simulator does understand
transit switches once a route is already expanding. The specific gap is that
`CreateStartNodes` does not seed the initial pathfinder queue from
transit-switch-only cells. Therefore a TE-aware pathfinder fix should be scoped
to initial attachment, not a wholesale replacement of transit switch traversal.

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
- `0x006D8A90` still matches `cSC4PathFinder_CreateStartNodes`.
- `0x006D8FA0` still matches `cSC4PathFinder_AddTripNode`.
- `0x006D91B0` still matches `cSC4PathFinder_FindPath`.
- `0x006D7960` still matches `cSC4PathFinder_AtGoal`.
- `0x006D7E50` still matches `cSC4PathFinder_FindNearestStandardDest`.
- `0x0070EC40` still matches `cSC4TrafficSimulator_SetupTrip`.
- `0x0070FB30` still matches `cSC4TrafficSimulator_GetNetworkInfo`.
- `0x007114E0` still matches
  `cSC4TrafficSimulator_GetNeighborPoolPropertyHolder`.
- `0x00711610` still matches `cSC4TrafficSimulator_SetupPathFinderForLot`.
- `0x0071D020` still matches `cSC4TrafficSimulator_WorkOnReRouteTrips`.
- `0x0071D440` still matches `cSC4TrafficSimulator_WorkOnFullTrips`.
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
SC4TransitAccessSample.dll
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

After access/growth succeeds with only the road-access hook:

1. Provide real jobs reachable through the connected road/transit system.
2. Let the city run for several simulation months.
3. Use the traffic query tool to inspect commute paths.
4. Confirm Sims can leave the grown lot and route through valid networks or
   valid transit switches.

If growth succeeds but Sims cannot commute, the access exception is working but
the pathfinder start-node configuration still needs separate attention. This
has now been observed in a real Windows SC4 test: road access and Dirt Road
masking can work while commutes still fail to attach to TE lots.

For the next pathfinder prototype, add a focused test:

1. Start from a city where a normal road-adjacent residential lot can commute.
2. Replace the road-facing segment with a known RTMT-style TE lot.
3. Confirm the lot still grows or remains occupied.
4. Confirm `CreateStartNodes` fallback logs identify the source lot, adjacent
   TE lot, and retry rectangle.
5. Let the simulator run for several months.
6. Use the traffic query tool to verify whether paths now start from the
   nearby real road cells or transit-switch cells.

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

The current road-access prototype does not guarantee that Sims can actually
commute. It only grants the road-access result when an adjacent
transit-enabled lot contacts a valid road-like network.

Actual traffic behavior still depends on:

- Transit switch point entries.
- Transit switch directions.
- Network connectivity.
- Simulator settings.
- Congestion and path costs.

The newly identified pathfinder gap is more concrete: initial trip attachment
depends on `CreateStartNodes`, and that function only seeds root nodes from
real network cells. Therefore the next functional prototype needs a
pathfinder-start hook in addition to the road-access hook.

## Current Confidence

The Dirt Road portion is high confidence after user testing: the optional mask
patch does what it was intended to do and should be left alone.

The original road-access exception is still useful, but incomplete. It follows
existing game behavior:

- It preserves stock `CalculateRoadAccess` first.
- It uses the stock road-access mask as the default.
- It models the exception after the ferry terminal's use of the traffic network
  map.
- It writes the same road-access cache that the stock function writes.
- It avoids broad TE-chain behavior.

The overall solution now likely needs two behavior hooks plus one bookkeeping
hook:

- `cSC4Lot::CalculateRoadAccess` for growth/access eligibility.
- `cSC4PathFinder::CreateStartNodes` for actual commute attachment.
- `cSC4TrafficSimulator::SetupPathFinderForLot` for reliable
  `pathFinder -> sourceLot` bookkeeping.

The recommended next prototype is Option A, the source-rectangle retry through
the adjacent TE lot, with the setup hook feeding it the exact source lot. It is
less precise than direct synthetic transit-switch root nodes, but it reuses
stock node creation and should quickly tell us whether the missing piece is
simply start-node attachment beyond the TE lot.
