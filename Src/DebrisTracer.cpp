#include "DebrisTracer.h"

#include <Globals.h>
#include <Hooks.h>
#include <IRenderer.h>
#include <Logging.h>

#include <Windows.h>

#include <Glacier/SExternalReferences.h>
#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZCollision.h>
#include <Glacier/ZEntityManager.h>
#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHM5CrippleBox.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZInventory.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZPhysics.h>
#include <Glacier/ZResource.h>
#include <Glacier/ZScene.h>
#include <Glacier/ZSpatialEntity.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
constexpr const char* kVersion = "1.0.0";

// The name PhysX's scene locks are given for their own bookkeeping (where a lock was taken). A plain name
// rather than __FILE__, which would put the path of the machine the mod was built on into the DLL.
constexpr const char* kLockFile = "DebrisTracer.cpp";

// The log is appended to across sessions; past this size it starts again empty.
constexpr uintmax_t kMaxLogBytes = 20ull * 1024 * 1024;

// Addresses as Ghidra saw them; Windows loads the game elsewhere, so they are shifted at runtime.
// They are for game version 3.290.1. The FUN_ names in the comments are those of the build the analysis
// was done on (3.280), kept so they can be looked up in that analysis.
constexpr uintptr_t kGhidraImageBase = 0x140000000;
constexpr uintptr_t kGhidraImageEnd = 0x150000000;

// The violin, the only one in the repository.
const ZRepositoryID kViolinId("4fad7437-59e9-4ca9-9b31-a6d97484216b");

using Clock = std::chrono::steady_clock;

long long Ns(Clock::time_point p_From, Clock::time_point p_To) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(p_To - p_From).count();
}

// Enough pieces to see what happens, few enough that following them stays free.
constexpr int kMaxTracked = 200;

// The plugin's place in the frame. Three were measured - far before the character, as usual, far
// after - and the launch took just as often from each, so it stays where every plugin goes.
constexpr int kUpdatePriority = 1;
constexpr int kUpdateMode = 0;

struct KnownVTable {
    uintptr_t Address;
    const char* Name;
};

// Every vtable a physics object can present. A class with several bases has one per base, which is
// why the same name appears more than once.
constexpr KnownVTable kKnownVTables[] = {
    { 0x141f4b400, "ZDestructiblePhysicsObject" },
    { 0x141f4bc08, "ZDestructiblePhysicsObject" },
    { 0x141f4c568, "ZDestructiblePhysicsObject" },
    { 0x141f4af70, "ZPhysicsObject" },
    { 0x141f4b708, "ZPhysicsObject" },
    { 0x141f40288, "ZInterpolatedPhysicsObject" },
    { 0x141f3b900, "ZPhysicsObjectReplica" },
    { 0x141f3b9b0, "ZPhysicsObjectReplica" },
    { 0x141f4a388, "IPhysicsObject" },
    { 0x141f48ec0, "ZDestructiblePhysicsG2Adapter" },
    { 0x141f49298, "ZDestructiblePhysicsG2Adapter" },
    { 0x141f3de38, "ZStaticPhysicsAspect" },
    { 0x141f3df38, "ZDynamicPhysicsAspect" },
};

// Where the game keeps its physics, read out of HITMAN3.exe with Ghidra: a global holding the
// physics manager, which creates PhysX's controller manager and keeps it at +0x58.
constexpr uintptr_t kPhysicsManagerGlobal = 0x143159430;
constexpr uintptr_t kPhysicsManagerVTable = 0x141f4abc0;
constexpr size_t kControllerManagerOffset = 0x58;

// Where the game keeps 47's own controller (Ghidra, 27/09): the hero's update, FUN_1402ca230, stores his
// ZCharacterController in this global, and PhysX's capsule controller sits 0x20 into it.
constexpr uintptr_t kHeroControllerGlobal = 0x14315b6d8;

// PhysX's character controller DLL - the game's own build of PhysX 3.4.1 - read with Ghidra and
// checked against NVIDIA's published source. The vtables identify the two objects before anything
// is called on them: slot 3 of the manager's is the function that owns the "getController():
// out-of-range index" message, and the capsule controller's is the one with IOI's three extra
// methods that the game calls after every move.
constexpr const char* kCctModuleName = "PhysX3CharacterKinematic_x64.dll";
constexpr const char* kPhysXModuleName = "PhysX3_x64.dll";
constexpr uintptr_t kCctImageBase = 0x180000000;
constexpr uintptr_t kControllerManagerVTable = 0x180023db0;
constexpr uintptr_t kCapsuleControllerVTable = 0x1800237e8;
constexpr int kGetNbControllersSlot = 2;
constexpr int kGetControllerSlot = 3;

// Inside the capsule controller, from the pointer the game holds. getState reads the first two, the
// ride reads all of them - which is how each was identified.
constexpr size_t kTouchedShapeOffset = 0x128;
constexpr size_t kTouchedActorOffset = 0x140;
constexpr size_t kCentreOffset = 0x218;          // three doubles, the centre of the capsule
constexpr size_t kTouchedPosWorldOffset = 0x174; // three floats, mTouchedPosShape_World
constexpr size_t kDeltaXPOffset = 0x230;         // three floats, mDeltaXP = the ride's delta * timeCoeff
constexpr size_t kSceneOffset = 0x248;
constexpr size_t kRideStepOffset = 0x250;        // the scene step at which the ride was last worked out
constexpr size_t kCollisionFlagsOffset = 0x270;
constexpr size_t kStandingOnMovingOffset = 0x271;
constexpr int kSceneGetTimestampSlot = 7;        // the call the ride itself makes on the scene
constexpr uint8_t kCollisionDown = 4;

// From a PhysX actor back to the game's fragment, as the game's own ray code does it (Ghidra,
// FUN_1410dbe20): the actor's userData is the fragment object's second base, whose slot 2 is simply
// "return this - 8". Both vtables are checked before the object is used.
constexpr size_t kActorUserDataOffset = 0x10;
constexpr uintptr_t kFragmentUserDataVTable = 0x141f4b400;
constexpr uintptr_t kFragmentVTable = 0x141f4c568;
constexpr uintptr_t kUserDataToObject = 8;

// A piece's owner and its destructible (Ghidra, 27/09). The owner entity sits at object+0x18 (slot 60
// reads it). The entity's type lists its interfaces as (type id, offset) pairs - what FUN_140efa8a0
// walks. For a destructible the "physics access" interface is ZDestructiblePhysicsG2Adapter+0x18: its
// +0x10 is the destructible, whose registry keeps the first piece at +0x190 and all of them at +0x198.
constexpr size_t kObjectOwnerOffset = 0x18;
constexpr size_t kEntityInterfacesOffset = 0x20;
constexpr uintptr_t kPhysicsAccessTypeId = 0x144627b30;
constexpr uintptr_t kG2AccessVTable = 0x141f48ec0;
constexpr size_t kAccessSystemOffset = 0x10;
constexpr size_t kSystemFirstPieceOffset = 0x190;
constexpr size_t kSystemPiecesOffset = 0x198;
constexpr uintptr_t kPieceHolderVTable = 0x141f4bc08; // the list holds object+0x140
constexpr size_t kHolderToObject = 0x140;

// The PhysX actor inside one of the game's physics objects, and where PhysX keeps its type.
constexpr size_t kActorOffset = 0x30;
constexpr size_t kConcreteTypeOffset = 8;

// The box an actor fills in the world: PxActor::getWorldBounds, slot 9 counting down NVIDIA's 3.4
// headers - the same count puts getGlobalPose and setGlobalPose at 20 and 21, which the game's own
// code confirms. It is called under the scene's write lock, taken exactly as the game's SetPosition
// takes it (Ghidra, FUN_1410f8e80): the scene at physics manager +0x68, lock 0x330, unlock 0x338.
constexpr size_t kGetWorldBoundsSlot = 9;
constexpr size_t kPhysicsSceneOffset = 0x68;
constexpr size_t kSceneLockWriteOffset = 0x330;
constexpr size_t kSceneUnlockWriteOffset = 0x338;

struct PxBounds {
    float MinX, MinY, MinZ, MaxX, MaxY, MaxZ;
};

// The pose PhysX really has for an actor: PxRigidActor::getGlobalPose, slot 20.
constexpr size_t kGetGlobalPoseSlot = 20;

struct PxPose {
    float QX, QY, QZ, QW;
    float PX, PY, PZ;
};

// The game keeps its own copy of a piece's rotation at +0x80 and its position at +0x70; SetPosition
// (slot 13) sends PhysX the new position WITH THAT COPY of the rotation. Slot 8 (FUN_1410f8bc0)
// takes both, rotation first, and is how a piece can be moved keeping the rotation it really has.
constexpr size_t kCachedRotationOffset = 0x80;
constexpr size_t kSetPoseSlot = 8;
constexpr uintptr_t kSetPoseFunction = 0x141111430;

struct GamePose {
    float QX, QY, QZ, QW;
    float PX, PY, PZ, PW;
};

// A method returning a struct takes the place to write it right after "this". getGlobalPose takes no
// third argument; the one passed is simply ignored.
using ActorGetterFn = void* (*)(uintptr_t p_Actor, void* p_Out, float p_Inflation);
using SceneLockFn = void (*)(uintptr_t p_Scene, const char* p_File, uint32_t p_Line);
using SceneUnlockFn = void (*)(uintptr_t p_Scene);

// Kept apart, as __try cannot share a function with anything that has to be destroyed.
bool CallActorGetter(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, uintptr_t p_Actor,
    ActorGetterFn p_Getter, void* p_Out) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Getter(p_Actor, p_Out, 1.f);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// The centre of a capsule, as PhysX keeps it.
struct Vec3d {
    double X, Y, Z;
};

struct Vec3f {
    float X, Y, Z;
};

// The scene's list of its bodies (Ghidra on PhysX3_x64.dll, 24/09): NpScene's vtable is at
// 0x1801ac768 - found from the messages of unlockRead, lockWrite and unlockWrite, which sit at 0x328,
// 0x330 and 0x338 of it, just where the game calls them. Slot 18 counts the actors of the kinds asked
// for, slot 19 copies them out; the kinds are flags passed by pointer, bit 1 for moving bodies.
constexpr uintptr_t kPhysXImageBase = 0x180000000;
constexpr uintptr_t kNpSceneVTable = 0x1801ac768;
constexpr size_t kGetNbActorsSlot = 18;
constexpr size_t kGetActorsSlot = 19;
constexpr uint16_t kActorTypeRigidDynamic = 2;
constexpr uint32_t kMaxSceneBodies = 200000; // A sanity limit: Miami has some sixteen thousand.

// PxActor::setActorFlag, slot 10 - right after getWorldBounds (9), both counted down NVIDIA's headers
// with getGlobalPose and setGlobalPose at 20 and 21 as the check. Flag 2 turns gravity off.
constexpr size_t kSetActorFlagSlot = 10;
constexpr int kDisableGravityFlag = 2;

// How far under the map removed pieces are sent.
constexpr float kGraveDepth = 1000.f;

using GetNbActorsFn = uint32_t (*)(uintptr_t p_Scene, const uint16_t* p_Types);
using GetActorsFn = uint32_t (*)(uintptr_t p_Scene, const uint16_t* p_Types, uintptr_t* p_Buffer, uint32_t p_Size,
    uint32_t p_Start);
using SetActorFlagFn = void (*)(uintptr_t p_Actor, int p_Flag, bool p_Value);

// Under the scene's write lock, as everything else here. Kept apart from anything that has to be
// destroyed, for __try.
bool CallListActors(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, GetNbActorsFn p_Count,
    GetActorsFn p_List, uintptr_t* p_Buffer, uint32_t p_Size, uint32_t& p_Total, uint32_t& p_Got) {
    volatile bool s_Locked = false;
    const uint16_t s_Types = kActorTypeRigidDynamic;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Total = p_Count(p_Scene, &s_Types);
        p_Got = p_Buffer ? p_List(p_Scene, &s_Types, p_Buffer, p_Size, 0) : 0;
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// PxRigidBody::addForce, at 0x148 of the actor's vtable: read out of PxRigidBodyExt::addForceAtPos,
// which is compiled into HITMAN3.exe, and used on 23/09 to give pieces a velocity. Mode 2 is a change
// of velocity; with autowake the body is woken up by it.
using AddForceFn = void(__fastcall*)(uintptr_t, const float*, int, bool);
constexpr size_t kAddForceOffset = 0x148;
constexpr int kVelocityChangeMode = 2;

bool CallAddForce(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, AddForceFn p_Add, uintptr_t p_Actor,
    const float* p_Vector) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Add(p_Actor, p_Vector, kVelocityChangeMode, true);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// PxRigidBody::getMass, at 0x100: PxRigidBodyExt::setMassAndUpdateInertia and updateMassAndInertia,
// compiled into HITMAN3.exe, both call setMass at 0xf8 (Ghidra, DumpMass.java, 25/09), and NVIDIA's
// header has getMass right after it.
using GetMassFn = float(__fastcall*)(uintptr_t);
constexpr size_t kGetMassOffset = 0x100;

bool CallGetMass(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, GetMassFn p_Get, uintptr_t p_Actor,
    float& p_Mass) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Mass = p_Get(p_Actor);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// ---- The game's set-down, as Ghidra read it (26/09, DumpSetDown*.java) ----
//
// After every substep of 47's move (ZCharacterController::Move, FUN_1410d75d0), FUN_1410d78c0 compares
// his feet before and after: if they moved up or down by more than 0.2 m, a sphere half as wide as his
// capsule is swept by the collision manager (slot 110, FUN_1410e30f0) from 1 m above the higher height
// down to the lower one, and his feet are put on the first thing it hits. The query's filter is the
// game's own: word1 of a shape's query filter data against a table of the collision manager, and word3
// against another. PhysX's own move looks at word0 instead.
constexpr uintptr_t kCollisionManagerGlobal = 0x143159398;
constexpr uintptr_t kCollisionManagerVTable = 0x141f4bf50;
constexpr uintptr_t kCmLayerFunction = 0x1410f3970;    // slot 34: layer & 0x1f, -1 -> 0
constexpr uintptr_t kCmMaskFunction = 0x1410f3980;     // slot 67: what a layer collides with
constexpr uintptr_t kCmSweepFunction = 0x1410fb960;    // slot 110: the sphere sweep of the set-down
constexpr size_t kCmLayerOffset = 0x110;
constexpr size_t kCmMaskOffset = 0x218;
constexpr size_t kCmSweepOffset = 0x370;
constexpr size_t kCmWord1TableOffset = 0x1e0;
constexpr size_t kCmWord3TableOffset = 0x260;
constexpr size_t kCmMatrixOffset = 0x160;

// 47's controller, the game's side: the PxController's report callback is this object + 0x10 (its
// userData is left empty - read 0 in the game), and it keeps the PxController at +0x20, the layer at
// +0x44, the largest vertical change it lets through at +0x58.
constexpr uintptr_t kCharacterControllerVTable = 0x141f4be28;
constexpr size_t kPxReportCallbackOffset = 0x68;       // Cct::Controller::mReportCallback (CCT constructor)
constexpr size_t kPxRadiusOffset = 0x288;              // PxCapsuleController::getRadius, slot 26
constexpr size_t kPxGetFootOffset = 0x30;              // PxController::getFootPosition, slot 6
constexpr size_t kWrapperControllerOffset = 0x20;
constexpr size_t kWrapperLayerOffset = 0x44;
constexpr size_t kWrapperMaxStepOffset = 0x58;

// A PhysX actor's shapes, and a shape's query filter data: slots the game itself uses (getShapes in
// FUN_1410d6e80, getQueryFilterData in ZCustomQueryFilter's preFilter).
constexpr size_t kGetShapesOffset = 0xd0;              // PxRigidActor::getShapes, slot 26
constexpr size_t kGetQueryFilterDataOffset = 0xb0;     // PxShape::getQueryFilterData, slot 22
constexpr size_t kGetSimulationFilterDataOffset = 0xa0; // PxShape::getSimulationFilterData, slot 20 (FUN_1410ea8d0
                                                        // reads it there before setting it at 0x98)

// Whether two bodies touch at all is the game's own "simulation filter shader" (Ghidra, 27/09): a pure function
// of the two objects' attributes and simulation filter data - layer matrix on word0, then shared bits in word2
// (groups) or word3, then contact flags from a 5x5 table on word1. Called here as PhysX calls it.
constexpr uintptr_t kFilterShaderFunction = 0x141118cb0;
constexpr size_t kPxGetActorOffset = 0x38;             // PxController::getActor, slot 7 (the kinematic capsule)
constexpr uint32_t kFilterDynamic = 1;                 // PxFilterObjectType::eRIGID_DYNAMIC
constexpr uint32_t kFilterKinematic = 0x10;            // PxFilterObjectFlag::eKINEMATIC

using FilterShaderFn = uint16_t*(__fastcall*)(uint16_t*, uint32_t, const uint32_t*, uint32_t, const uint32_t*,
    uint16_t*, const void*, uint32_t);
using GetActorFn = uintptr_t(__fastcall*)(void*);

bool CallFilterShader(FilterShaderFn p_Fn, uint32_t p_Attr0, const uint32_t* p_Data0, uint32_t p_Attr1,
    const uint32_t* p_Data1, uint16_t& p_Result, uint16_t& p_PairFlags) {
    __try {
        uint16_t s_Result = 0;
        p_PairFlags = 0;
        p_Fn(&s_Result, p_Attr0, p_Data0, p_Attr1, p_Data1, &p_PairFlags, nullptr, 0);
        p_Result = s_Result;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallGetActor(GetActorFn p_Fn, void* p_Controller, uintptr_t& p_Out) {
    __try {
        p_Out = p_Fn(p_Controller);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using CmLayerFn = uint16_t(__fastcall*)(void*, int);
using CmMaskFn = uint32_t(__fastcall*)(void*, uint16_t);
using CmSweepFn = uint64_t(__fastcall*)(void*, float*, float*);
using GetFootFn = double*(__fastcall*)(void*, double*);
using GetShapesFn = uint32_t(__fastcall*)(uintptr_t, uintptr_t*, uint32_t, uint32_t);
using GetQueryFilterDataFn = uint32_t*(__fastcall*)(uintptr_t, uint32_t*);

bool CallCmLayer(CmLayerFn p_Fn, void* p_Cm, int p_Value, uint16_t& p_Out) {
    __try {
        p_Out = p_Fn(p_Cm, p_Value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallCmMask(CmMaskFn p_Fn, void* p_Cm, uint16_t p_Layer, uint32_t& p_Out) {
    __try {
        p_Out = p_Fn(p_Cm, p_Layer);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The sweep takes the scene's read lock itself, as the game calls it.
bool CallCmSweep(CmSweepFn p_Fn, void* p_Cm, float* p_Result, float* p_Query, uint64_t& p_Out) {
    __try {
        p_Out = p_Fn(p_Cm, p_Result, p_Query);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallGetFoot(GetFootFn p_Fn, void* p_Controller, double* p_Out) {
    __try {
        p_Fn(p_Controller, p_Out);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Up to four shapes of an actor and the query filter data of each, under the scene's lock. The shape's
// method is checked to be inside PhysX before it is called.
bool CallShapeFilters(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, GetShapesFn p_GetShapes,
    uintptr_t p_Actor, uintptr_t p_PhysXBase, uintptr_t p_PhysXEnd, uint32_t* p_Words, uint32_t& p_Count,
    size_t p_GetterOffset = kGetQueryFilterDataOffset) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;

        uintptr_t s_Shapes[4] = {};
        p_Count = p_GetShapes(p_Actor, s_Shapes, 4, 0);

        if (p_Count > 4) {
            p_Count = 4;
        }

        for (uint32_t i = 0; i < p_Count; ++i) {
            const uintptr_t s_VTable = *reinterpret_cast<const uintptr_t*>(s_Shapes[i]);
            const uintptr_t s_Get = *reinterpret_cast<const uintptr_t*>(s_VTable + p_GetterOffset);

            if (s_Get < p_PhysXBase || s_Get >= p_PhysXEnd) {
                p_Count = i;
                break;
            }

            reinterpret_cast<GetQueryFilterDataFn>(s_Get)(s_Shapes[i], p_Words + 4 * i);
        }

        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// A body's escape speed - how fast PhysX may push it out of something it is stuck in - and its body
// flags: NpRigidDynamic's vtable in PhysX3_x64.dll (Ghidra, DumpRigidDynamic.java, 26/09): slot 47
// getRigidBodyFlags (a byte out), 50 setMaxDepenetrationVelocity, 51 getMaxDepenetrationVelocity.
constexpr size_t kGetBodyFlagsOffset = 0x178;
constexpr size_t kSetMaxDepenOffset = 0x190;
constexpr size_t kGetMaxDepenOffset = 0x198;
constexpr uint8_t kBodyFlagCcd = 4;            // PxRigidBodyFlag::eENABLE_CCD
constexpr uint8_t kBodyFlagSpeculativeCcd = 32; // PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD

using GetMaxDepenFn = float(__fastcall*)(uintptr_t);
using SetMaxDepenFn = void(__fastcall*)(uintptr_t, float);
using GetBodyFlagsFn = uint8_t*(__fastcall*)(uintptr_t, uint8_t*);

bool CallEscapeRead(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, GetMaxDepenFn p_Get,
    GetBodyFlagsFn p_Flags, uintptr_t p_Actor, float& p_Speed, uint8_t& p_BodyFlags) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Speed = p_Get(p_Actor);
        p_Flags(p_Actor, &p_BodyFlags);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

bool CallEscapeWrite(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, SetMaxDepenFn p_Set,
    uintptr_t p_Actor, float p_Speed) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Set(p_Actor, p_Speed);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

bool CallSetActorFlag(uintptr_t p_Scene, SceneLockFn p_Lock, SceneUnlockFn p_Unlock, SetActorFlagFn p_Set,
    uintptr_t p_Actor, int p_Flag, bool p_Value) {
    volatile bool s_Locked = false;

    __try {
        p_Lock(p_Scene, kLockFile, __LINE__);
        s_Locked = true;
        p_Set(p_Actor, p_Flag, p_Value);
        p_Unlock(p_Scene);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (s_Locked) {
            __try {
                p_Unlock(p_Scene);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        return false;
    }
}

// A read that cannot take the game down: memory that has gone away just reads as a failure.
template <typename T>
bool ReadRaw(uintptr_t p_Address, T& p_Out) {
    if (p_Address < 0x10000) {
        return false;
    }

    __try {
        p_Out = *reinterpret_cast<const T*>(p_Address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Reading a body the engine has just destroyed would take the game down with it. Asking Windows
// about the page first is safe but costs a system call, which is far too much to pay for every
// piece on every frame. Guarding the read itself costs nothing and fails just as gracefully.
bool ReadVTable(const void* p_Pointer, uintptr_t& p_Out) {
    if (!p_Pointer || reinterpret_cast<uintptr_t>(p_Pointer) < 0x10000) {
        return false;
    }

    __try {
        p_Out = *static_cast<const uintptr_t*>(p_Pointer);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Readable(const void* p_Pointer, size_t p_Size) {
    if (!p_Pointer || reinterpret_cast<uintptr_t>(p_Pointer) < 0x10000) {
        return false;
    }

    MEMORY_BASIC_INFORMATION s_Info {};

    if (VirtualQuery(p_Pointer, &s_Info, sizeof(s_Info)) == 0 || s_Info.State != MEM_COMMIT) {
        return false;
    }

    constexpr DWORD s_Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    if ((s_Info.Protect & s_Readable) == 0 || (s_Info.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const auto s_End = reinterpret_cast<uintptr_t>(s_Info.BaseAddress) + s_Info.RegionSize;

    return reinterpret_cast<uintptr_t>(p_Pointer) + p_Size <= s_End;
}

SVector3 ToVec3(const float4& p_Vec) {
    return SVector3(p_Vec.x, p_Vec.y, p_Vec.z);
}

std::string LogPath() {
    const char* s_Local = std::getenv("LOCALAPPDATA");
    return std::string(s_Local ? s_Local : ".") + "\\DebrisTracer.log";
}
} // namespace

DebrisTracer::~DebrisTracer() {
    const ZMemberDelegate<DebrisTracer, void(const SGameUpdateEvent&)> s_Delegate(this, &DebrisTracer::OnFrameUpdate);
    Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, kUpdatePriority, static_cast<EUpdateMode>(kUpdateMode));
}

void DebrisTracer::Init() {
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &DebrisTracer::OnClearScene);
}

void DebrisTracer::OnEngineInitialized() {
    const auto s_Base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    m_ModuleDelta = s_Base - kGhidraImageBase;

    std::error_code s_Error;
    const auto s_LogSize = std::filesystem::file_size(LogPath(), s_Error);
    m_File.open(LogPath(), !s_Error && s_LogSize > kMaxLogBytes ? std::ios::trunc : std::ios::app);

    const ZMemberDelegate<DebrisTracer, void(const SGameUpdateEvent&)> s_Delegate(this, &DebrisTracer::OnFrameUpdate);
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, kUpdatePriority, static_cast<EUpdateMode>(kUpdateMode));

    m_Enabled = GetSettingBool("general", "enabled", true);
    m_AlwaysVisible = GetSettingBool("general", "window_always_visible", false);
    m_FollowPieces = GetSettingBool("general", "track_fragments", false);
    m_TraceHero = GetSettingBool("general", "track_47", false);
    m_TrackRadius = static_cast<float>(GetSettingDouble("general", "track_radius", 20.0));

    Log(fmt::format("Loaded v{}. Game base 0x{:x}, shift 0x{:x}. Log: {}", kVersion, s_Base, m_ModuleDelta, LogPath()));

    m_KnownGame = CheckGameBuild();

    if (!m_KnownGame) {
        Log("Unknown game build (updated, or another store's version): the tools that call into the game may not "
            "work, and \"Collide check\" is off.");
    }
}

bool DebrisTracer::CheckGameBuild() const {
    const auto s_Slot = [this](uintptr_t p_VTable, size_t p_Offset, uintptr_t p_Function) {
        uintptr_t s_Value = 0;
        return ReadRaw(p_VTable + m_ModuleDelta + p_Offset, s_Value) && s_Value == p_Function + m_ModuleDelta;
    };

    return s_Slot(kCollisionManagerVTable, kCmLayerOffset, kCmLayerFunction) &&
        s_Slot(kCollisionManagerVTable, kCmMaskOffset, kCmMaskFunction) &&
        s_Slot(kCollisionManagerVTable, kCmSweepOffset, kCmSweepFunction) &&
        s_Slot(kFragmentVTable, kSetPoseSlot * sizeof(void*), kSetPoseFunction);
}

// A live object of the game's own carries a vtable inside the game's image. Cheap enough to check
// every frame, and it is how a body that has since been destroyed gets noticed.
// Only the physics classes Ghidra named. Any vtable inside the game's image used to pass, and that
// took the game down twice: when the violin's pieces were cleared away their memory went to other
// objects, those passed the check, and asking one of them for its position - through the vtable of
// a different class - came back with a pointer to nowhere.
bool DebrisTracer::LooksLikeGameObject(const void* p_Pointer) const {
    uintptr_t s_VTable = 0;

    if (!ReadVTable(p_Pointer, s_VTable)) {
        return false;
    }

    const uintptr_t s_Ghidra = s_VTable - m_ModuleDelta;

    for (const auto& s_Known : kKnownVTables) {
        if (s_Ghidra == s_Known.Address) {
            return true;
        }
    }

    return false;
}

// The same check, but it also names what it found, including the classes we have no name for: a
// vtable inside the game's image means an object of some class, and the address Ghidra would show
// for it is enough to look that class up afterwards.
std::string DebrisTracer::DescribePointer(const void* p_Pointer) const {
    if (!Readable(p_Pointer, sizeof(void*))) {
        return {};
    }

    const auto s_VTable = reinterpret_cast<uintptr_t>(*static_cast<void* const*>(p_Pointer));

    if (!Readable(reinterpret_cast<const void*>(s_VTable), sizeof(void*))) {
        return {};
    }

    const uintptr_t s_Ghidra = s_VTable - m_ModuleDelta;

    // A vtable lives in the game's own image. Anything else is not an object of ours.
    if (s_Ghidra < kGhidraImageBase || s_Ghidra >= kGhidraImageEnd) {
        return {};
    }

    for (const auto& s_Known : kKnownVTables) {
        if (s_Ghidra == s_Known.Address) {
            return fmt::format("{} [ghidra 0x{:x}]", s_Known.Name, s_Ghidra);
        }
    }

    return fmt::format("UNKNOWN class, vtable ghidra 0x{:x}", s_Ghidra);
}

void DebrisTracer::OnDrawMenu() {
    if (ImGui::Button("DEBRIS TRACER")) {
        m_ShowWindow = !m_ShowWindow;
    }
}

void DebrisTracer::OnDrawUI(bool p_HasFocus) {
    const auto s_Start = Clock::now();
    struct Timer {
        std::atomic<long long>& Sum;
        Clock::time_point Start;
        ~Timer() { Sum += Ns(Start, Clock::now()); }
    } s_Timer { m_TimeNs[kTimeWindow], s_Start };

    // With the mod menu closed, only if asked for.
    if (!m_ShowWindow || (!p_HasFocus && !m_AlwaysVisible)) {
        return;
    }

    ImGui::PushFont(SDK()->GetImGuiBlackFont());
    const auto s_Expanded = ImGui::Begin("Debris Tracer", &m_ShowWindow);
    ImGui::PushFont(SDK()->GetImGuiRegularFont());

    if (s_Expanded) {
        ImGui::TextDisabled("Debris Tracer v%s", kVersion);

        if (!m_KnownGame) {
            ImGui::TextColored(
                ImVec4(1.f, 0.6f, 0.2f, 1.f), "Unknown game build: some tools may not work, \"Collide check\" is off."
            );
        }

        if (ImGui::Checkbox("Enabled", &m_Enabled)) {
            SetSettingBool("general", "enabled", m_Enabled);
        }

        if (ImGui::Checkbox("Window always visible (also with the mod menu closed)", &m_AlwaysVisible)) {
            SetSettingBool("general", "window_always_visible", m_AlwaysVisible);
        }

        ImGui::Separator();

        if (!m_Enabled) {
            ImGui::TextWrapped("Off: nothing is read, searched or drawn.");
        }
        else {
            // ---- Tracking ----
            if (ImGui::Checkbox("Track the fragments around 47", &m_FollowPieces)) {
                SetSettingBool("general", "track_fragments", m_FollowPieces);
            }

            if (m_FollowPieces) {
                if (ImGui::SliderFloat("Look for fragments within (m)", &m_TrackRadius, 2.f, 60.f, "%.0f")) {
                    SetSettingDouble("general", "track_radius", m_TrackRadius);
                }

                ImGui::Text(
                    "Fragments tracked: %d (moving bodies in the scene: %d)", m_TrackedCount.load(), m_SceneBodies
                );

                ImGui::Checkbox("Show the real boxes of the pieces near 47 (green)", &m_ShowRealBoxes);
            }

            if (ImGui::Checkbox("Track 47", &m_TraceHero)) {
                SetSettingBool("general", "track_47", m_TraceHero);
            }

            if (ImGui::Button("Clear the lines")) {
                m_ClearRequested = true;
            }

            // ---- For testing ----
            ImGui::Separator();

            // Wide enough for the number between the - and + buttons.
            ImGui::SetNextItemWidth(170.f);

            if (ImGui::InputInt("##ViolinCount", &m_ViolinCount)) {
                m_ViolinCount = std::clamp(m_ViolinCount, 1, 20);
            }

            ImGui::SameLine();

            const auto s_Give = m_ViolinCount == 1 ? std::string("Give 47 a violin")
                                                   : fmt::format("Give 47 {} violins", m_ViolinCount);

            if (ImGui::Button(s_Give.c_str())) {
                m_ViolinRequested = true;
            }

            ImGui::SameLine();

            // The tick shows what the game says too: ammo left infinite by something else can be turned off here.
            bool s_InfiniteAmmo = m_InfiniteAmmo || m_HitmanInfiniteAmmo;

            if (ImGui::Checkbox("Infinite ammo", &s_InfiniteAmmo)) {
                m_InfiniteAmmo = s_InfiniteAmmo;
                m_AmmoToggleRequested = true;
            }

            if (ImGui::Button("REMOVE ALL FRAGMENTS around 47 and stop tracking them")) {
                m_RemoveRequested = true;
            }

            {
                std::scoped_lock s_Lock(m_DataMutex);

                for (size_t i = 0; m_FollowPieces && i < m_Tracked.size() && i < 12; ++i) {
                    const auto& s_Item = m_Tracked[i];
                    const SVector3 s_Now = s_Item.Path.empty() ? SVector3() : s_Item.Path.back();
                    const float s_Rise = s_Now.z - s_Item.Start.z;

                    ImGui::Text(
                        "#%d  z=%.2f  rise=%+.2f m  points=%d%s", s_Item.Id, s_Now.z, s_Rise,
                        static_cast<int>(s_Item.Path.size()), s_Item.Alive ? "" : "  (gone)"
                    );
                }
            }

            // ---- Measuring tools: each costs nothing until it is switched on ----
            ImGui::Separator();
            ImGui::Checkbox("Measuring tools", &m_MeasuringTools);

            if (m_MeasuringTools) {
                DrawMeasuringTools();
            }
        }

        ImGui::Separator();

        std::scoped_lock s_Lock(m_LogMutex);

        for (auto it = m_Log.rbegin(); it != m_Log.rend(); ++it) {
            ImGui::TextUnformatted(it->c_str());
        }
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopFont();
}

// The tools used to measure the boost: timers, PhysX's own state, the ray under his feet, the detectors,
// and the launch by hand.
void DebrisTracer::DrawMeasuringTools() {
    ImGui::Indent();

    // Timers.
    ImGui::Checkbox("Timers (what each part costs per frame)", &m_ToolTimers);

    if (m_ToolTimers) {
        ImGui::TextColored(
            ImVec4(0.3f, 0.8f, 1.f, 1.f), "ms per frame: plugin %.3f | tracking %.3f | ray %.3f | PhysX %.3f",
            m_TimeShownMs[kTimeFrame], m_TimeShownMs[kTimePlayer], m_TimeShownMs[kTimeRay], m_TimeShownMs[kTimePhysX]
        );
        ImGui::TextColored(
            ImVec4(0.3f, 0.8f, 1.f, 1.f), "launch %.3f | this window %.3f | 3D %.3f",
            m_TimeShownMs[kTimeLaunch], m_TimeShownMs[kTimeWindow], m_TimeShownMs[kTimeDraw3D]
        );
    }

    // PhysX's state.
    ImGui::Checkbox("PhysX state (what PhysX has 47 standing on)", &m_ToolPhysX);

    if (m_ToolPhysX) {
        std::scoped_lock s_Lock(m_DataMutex);

        if (!m_Px.Controller) {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "PhysX: no controller for 47 (%s)", m_Px.Why.c_str());
        }
        else {
            ImGui::TextColored(
                m_Px.TouchedPiece > 0 || m_Px.TouchedFragment ? ImVec4(0.3f, 1.f, 0.4f, 1.f) : ImVec4(1.f, 0.6f, 0.2f, 1.f),
                "PhysX says: %s", PxLine().c_str()
            );
        }

        ImGui::Text("Controllers in the scene: %d", m_ControllerCount);
    }

    if (m_ToolPhysX && ImGui::Button("Write what PhysX says to the log for 10 s")) {
        m_PxTraceRequested = true;
    }

    // What the piece under him is: mass, size, and where PhysX touches it against his feet.
    if (ImGui::Button("Describe the piece under 47")) {
        m_DescribeRequested = true;
    }

    ImGui::SameLine();

    // The game's own filter shader, asked with the real simulation filter data: does his body touch them at all?
    if (ImGui::Button("Collide check")) {
        m_CollideCheckRequested = true;
    }

    // The game's own set-down sweep, run from where he stands: does it see the piece under him?
    if (ImGui::Button("Probe the set-down under 47")) {
        m_ProbeSetDownRequested = true;
    }

    ImGui::SameLine();
    ImGui::TextWrapped("the sphere the game drops after a ride: lands on the piece, or goes through it? moves nothing");

    // The ray from his feet.
    ImGui::Checkbox("Ray from his feet (what 47 stands on, every frame)", &m_SupportRayEachFrame);

    if (m_SupportRayEachFrame) {
        {
            std::scoped_lock s_Lock(m_DataMutex);

            if (m_SupportId > 0) {
                ImGui::TextColored(
                    ImVec4(0.3f, 1.f, 0.4f, 1.f), "STANDING ON PIECE #%d, %.3f m under his feet", m_SupportId,
                    -m_SupportDrop
                );
            }
            else {
                ImGui::TextColored(
                    ImVec4(1.f, 0.6f, 0.2f, 1.f), "Standing on: %s (%.3f m under his feet)", m_SupportName.c_str(),
                    -m_SupportDrop
                );
            }
        }

        ImGui::SliderFloat("Support ray starts at (m)", &m_SupportStart, -0.1f, 0.3f, "%.3f");
        ImGui::SliderFloat("Support ray reaches (m)", &m_SupportReach, 0.1f, 1.f, "%.2f");
    }

    // The rocket detector.
    ImGui::Checkbox("Log rockets (tracked pieces that move far in one frame)", &m_ToolRockets);

    if (m_ToolRockets) {
        ImGui::SliderFloat("A rocket moves in one frame more than (m)", &m_RocketStep, 1.f, 50.f, "%.0f");
        ImGui::Text("Rockets written to the log: %d", m_RocketCount);

        if (!m_FollowPieces) {
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Needs \"Track the fragments around 47\" on.");
        }
    }

    // The third factor, checked at every send of the launch.
    ImGui::Checkbox("Third factor: at every send, where PhysX touches the piece + set-down probe", &m_ToolThirdFactor);

    if (m_ToolThirdFactor) {
        ImGui::TextWrapped(
            "Use with \"Launch\". Each send writes how far from 47's axis PhysX touches the piece against the game's "
            "set-down sphere (half the capsule radius), and runs the set-down probe before the piece moves. If he "
            "does not go up, the log says where the touch was."
        );
    }

    // The boost recorder.
    ImGui::Checkbox("Record boosts (the frames around every time 47 rises in one frame)", &m_ToolRecordBoosts);

    if (m_ToolRecordBoosts) {
        ImGui::SliderFloat("A boost is a rise in one frame of (m)", &m_BoostRiseThreshold, 0.1f, 2.f, "%.2f");
        ImGui::TextColored(
            m_BoostOpen ? ImVec4(1.f, 0.8f, 0.2f, 1.f) : ImVec4(0.3f, 0.8f, 1.f, 1.f), "%s - boosts written to the log: %d",
            m_BoostOpen ? "RECORDING a boost" : "Waiting for a boost", m_BoostCount
        );
    }

    // The launch, by hand.
    ImGui::Checkbox("Launch", &m_ToolAutoTest);

    if (m_ToolAutoTest) {
        DrawLaunchTools();
    }

    ImGui::Unindent();
}

void DebrisTracer::DrawLaunchTools() {
    ImGui::TextWrapped(
        "The launch: if PhysX already has him on a fragment, that one is sent up. Otherwise a piece is put "
        "under his feet by its box, 47 is lifted over it so he comes down on it, and it is sent up once PhysX "
        "holds it. A launch that does not take is tried again."
    );

    ImGui::SliderFloat("Launch by (m)", &m_LiftAmount, 0.5f, 20.f, "%.2f");
    ImGui::SliderFloat("... and sideways by (m, one send only)", &m_LaunchSideways, 0.f, 5.f, "%.2f");
    ImGui::SliderInt("How many tries", &m_MaxRetries, 1, 20);
    ImGui::SliderFloat("Close the gap to within (m)", &m_GapAllowed, 0.001f, 0.05f, "%.3f");
    ImGui::SliderFloat("Hop 47 up by (m)", &m_HopUp, 0.f, 0.5f, "%.2f");
    ImGui::SliderInt("Tries to make it his footing", &m_MaxAttempts, 1, 40);
    ImGui::Combo("Place the piece", &m_PlaceMode, "origin at his feet (old)\0by its box (new)\0the two in turn\0");
    ImGui::SliderFloat("Box bottom above his feet (m)", &m_PlaceClearance, 0.f, 0.1f, "%.3f");
    ImGui::SliderFloat("Hop over the piece by (m)", &m_HopMargin, 0.f, 0.5f, "%.2f");
    ImGui::Checkbox("Always put a piece under him", &m_AlwaysStage);
    ImGui::Checkbox("Pick the tallest piece", &m_PreferBig);
    ImGui::Checkbox("Move pieces keeping their real rotation", &m_KeepRealRotation);
    ImGui::SliderFloat("Rays around his feet reach (m)", &m_ProbeReach, 0.3f, 3.f, "%.2f");

    if (ImGui::Button("LAUNCH")) {
        m_SequenceRequested = true;
    }

    const char* s_Stage = nullptr;

    switch (m_Stage) {
        case Stage::Settling: s_Stage = "waiting for him to land on it"; break;
        case Stage::Tightening: s_Stage = "closing the gap"; break;
        case Stage::Confirming: s_Stage = "he is on it, checking he stays"; break;
        case Stage::Retrying: s_Stage = "sending, and trying again if it does not take"; break;
        default: break;
    }

    if (s_Stage) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "%s", s_Stage);
    }
}

void DebrisTracer::OnDraw3D(IRenderer* p_Renderer) {
    const auto s_Start = Clock::now();
    struct Timer {
        std::atomic<long long>& Sum;
        Clock::time_point Start;
        ~Timer() { Sum += Ns(Start, Clock::now()); }
    } s_Timer { m_TimeNs[kTimeDraw3D], s_Start };

    // Only what is being tracked is drawn: nothing at all with both off.
    if (!m_Enabled || (!m_FollowPieces && !m_TraceHero)) {
        return;
    }

    std::scoped_lock s_Lock(m_DataMutex);

    // Debris in orange, brighter as it goes up; 47 in blue.
    for (const auto& s_Item : m_Tracked) {
        if (!m_FollowPieces) {
            break;
        }

        for (size_t i = 1; i < s_Item.Path.size(); ++i) {
            p_Renderer->DrawLine3D(
                s_Item.Path[i - 1], s_Item.Path[i], SVector4(1.f, 0.45f, 0.1f, 1.f), SVector4(1.f, 0.8f, 0.2f, 1.f)
            );
        }

        // Where the piece really is: its box in green, and the marker in its middle rather than at its
        // origin, which on the flowers is 0.7 m off.
        if (m_ShowRealBoxes && s_Item.HasBox && !s_Item.Path.empty()) {
            const SVector3 s_At = s_Item.Path.back();
            const SVector3 s_Min(s_At.x + s_Item.BoxMin.x, s_At.y + s_Item.BoxMin.y, s_At.z + s_Item.BoxMin.z);
            const SVector3 s_Max(s_At.x + s_Item.BoxMax.x, s_At.y + s_Item.BoxMax.y, s_At.z + s_Item.BoxMax.z);

            p_Renderer->DrawBox3D(s_Min, s_Max, SVector4(0.2f, 1.f, 0.3f, 1.f));
            continue;
        }

        if (!s_Item.Path.empty()) {
            const SVector3 s_Last = s_Item.Path.back();
            const SVector3 s_Size(0.03f, 0.03f, 0.03f);

            p_Renderer->DrawBox3D(
                SVector3(s_Last.x - s_Size.x, s_Last.y - s_Size.y, s_Last.z - s_Size.z),
                SVector3(s_Last.x + s_Size.x, s_Last.y + s_Size.y, s_Last.z + s_Size.z),
                SVector4(1.f, 0.3f, 0.f, 1.f)
            );
        }
    }

    for (size_t i = 1; m_TraceHero && i < m_HeroPath.size(); ++i) {
        p_Renderer->DrawLine3D(
            m_HeroPath[i - 1], m_HeroPath[i], SVector4(0.2f, 0.6f, 1.f, 1.f), SVector4(0.4f, 0.9f, 1.f, 1.f)
        );
    }
}

// Straight into the inventory, the way the Assets mod gives items: through 47's inventory
// subcontroller, which is the seventh of his character's controllers.
void DebrisTracer::GiveViolin() {
    auto s_Hitman = SDK()->GetLocalPlayer();

    if (!s_Hitman || !s_Hitman.m_pInterfaceRef) {
        Log("No 47 to give a violin to.");
        return;
    }

    const auto s_Character = s_Hitman.m_pInterfaceRef->m_pCharacter.m_pInterfaceRef;

    if (!s_Character || !s_Character->m_rSubcontrollerContainer.m_pInterfaceRef) {
        Log("Violin: 47's character is not ready.");
        return;
    }

    const auto* s_Controllers = &s_Character->m_rSubcontrollerContainer.m_pInterfaceRef->m_aReferencedControllers;

    if (s_Controllers->size() <= 6 || !(*s_Controllers)[6].m_pInterfaceRef) {
        Log("Violin: 47's inventory is not where it should be.");
        return;
    }

    const auto s_Inventory = static_cast<ZCharacterSubcontrollerInventory*>((*s_Controllers)[6].m_pInterfaceRef);

    const int s_Count = std::clamp(m_ViolinCount, 1, 20);

    for (int i = 0; i < s_Count; ++i) {
        Functions::ZCharacterSubcontrollerInventory_CreateItem->Call(
            s_Inventory, kViolinId, "", {}, ZCharacterSubcontrollerInventory::ECreateItemType::ECIT_ContractItem
        );
    }

    Log(s_Count == 1 ? std::string("Gave 47 a violin.") : fmt::format("Gave 47 {} violins.", s_Count));
}

// Infinite ammo is a cripple box with limited ammo turned off, as the Player mod makes it. The game keeps
// its cripple boxes on a list and recomputes 47's abilities from that list only when a box is activated or
// deactivated (ZHM5CrippleBox slots 6 and 7). A box's destructor just takes it off the list, so a box
// deleted without Deactivate first, by hand or by the level ending, leaves the ammo infinite until the game
// is closed: that is why the box is always deactivated here before it goes.
void DebrisTracer::SetInfiniteAmmo() {
    auto s_Hitman = SDK()->GetLocalPlayer();

    if (!m_InfiniteAmmo) {
        RemoveCrippleBox();

        // Still infinite, with no box of ours: left so by a box deleted without Deactivate (another mod, or
        // an older build of this one). A box switched on and off makes the game recompute the ammo from the
        // boxes that are left, which brings it back to normal.
        if (s_Hitman && s_Hitman.m_pInterfaceRef->IsInfiniteAmmoEnabled() && CreateCrippleBox()) {
            if (auto* s_CrippleBox = m_CrippleBoxEntity.QueryInterface<ZHM5CrippleBox>()) {
                s_CrippleBox->m_bActivateOnStart = true;
                s_CrippleBox->m_rHitmanCharacter = s_Hitman;
                s_CrippleBox->m_bLimitedAmmo = true;
                s_CrippleBox->Activate(0);
            }

            RemoveCrippleBox();
        }

        Log("Infinite ammo off.");
        return;
    }

    if (!s_Hitman) {
        Log("Infinite ammo: 47 is not in the world.");
        m_InfiniteAmmo = false;
        return;
    }

    if (!m_CrippleBoxEntity && !CreateCrippleBox()) {
        Log("Infinite ammo: could not create the cripple box (is a level loaded?).");
        m_InfiniteAmmo = false;
        return;
    }

    auto* s_CrippleBox = m_CrippleBoxEntity.QueryInterface<ZHM5CrippleBox>();

    if (!s_CrippleBox) {
        Log("Infinite ammo: the cripple box has no interface.");
        RemoveCrippleBox();
        m_InfiniteAmmo = false;
        return;
    }

    s_CrippleBox->m_bActivateOnStart = true;
    s_CrippleBox->m_rHitmanCharacter = s_Hitman;
    s_CrippleBox->m_bLimitedAmmo = false;
    s_CrippleBox->Activate(0);

    Log("Infinite ammo on.");
}

bool DebrisTracer::CreateCrippleBox() {
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;

    if (!s_Scene) {
        return false;
    }

    constexpr auto s_CrippleBoxId = ResId<"[modules:/zhm5cripplebox.class].pc_entitytype">;

    TResourcePtr<ZTemplateEntityFactory> s_Factory;
    Globals::ResourceManager->GetResourcePtr(s_Factory, s_CrippleBoxId, 0);

    if (!s_Factory) {
        return false;
    }

    SExternalReferences s_ExternalRefs;

    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, m_CrippleBoxEntity, "", s_Factory, s_Scene.m_entityRef, s_ExternalRefs, -1
    );

    return static_cast<bool>(m_CrippleBoxEntity);
}

// Deactivate first, so that the game recomputes the ammo without this box; then delete it.
void DebrisTracer::RemoveCrippleBox() {
    if (!m_CrippleBoxEntity) {
        return;
    }

    if (auto* s_CrippleBox = m_CrippleBoxEntity.QueryInterface<ZHM5CrippleBox>()) {
        s_CrippleBox->Deactivate(0);
    }

    Functions::ZEntityManager_DeleteEntity->Call(Globals::EntityManager, m_CrippleBoxEntity, {});
    m_CrippleBoxEntity = {};
}

// The nearest piece of debris lying still on his floor, within reach of where he started. Only
// fragments count: the whole violin, before it breaks, is a physics body too.
void* DebrisTracer::PickRestingPiece(const SVector3& p_Around) const {
    void* s_Best = nullptr;
    float s_BestSide = 3.f;
    float s_BestTall = -1.f;

    for (const auto& s_Item : m_Tracked) {
        if (!s_Item.Alive || s_Item.Path.empty() || s_Item.TypeName.find("ZDestructiblePhysicsObject") != 0 ||
            !LooksLikeGameObject(s_Item.Object) ||
            std::find(m_AutoRejected.begin(), m_AutoRejected.end(), s_Item.Object) != m_AutoRejected.end()) {
            continue;
        }

        const SVector3 s_Now = ToVec3(static_cast<IPhysicsObject*>(s_Item.Object)->GetPosition());
        const SVector3 s_Last = s_Item.Path.back();

        const float s_Moved = std::abs(s_Now.x - s_Last.x) + std::abs(s_Now.y - s_Last.y) +
            std::abs(s_Now.z - s_Last.z);
        const float s_Dx = s_Now.x - p_Around.x, s_Dy = s_Now.y - p_Around.y;
        const float s_Side = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy);

        if (s_Moved >= 0.01f || std::abs(s_Now.z - p_Around.z) >= 0.5f || s_Side >= 3.f) {
            continue;
        }

        // For testing the big ones: the tallest piece in reach, rather than the nearest.
        if (m_PreferBig) {
            PieceBox s_Box;
            std::string s_Why;

            if (ReadPieceBox(s_Item.Object, s_Box, s_Why) && s_Box.MaxZ - s_Box.MinZ > s_BestTall) {
                s_BestTall = s_Box.MaxZ - s_Box.MinZ;
                s_Best = s_Item.Object;
            }

            continue;
        }

        if (s_Side < s_BestSide) {
            s_BestSide = s_Side;
            s_Best = s_Item.Object;
        }
    }

    return s_Best;
}

// Goes to the file only, without the game's logger or the on-screen list.
void DebrisTracer::LogFile(const std::string& p_Message) {
    std::scoped_lock s_Lock(m_LogMutex);

    if (m_File) {
        m_File << fmt::format("[{:.2f}] {}\n", m_Time, p_Message);
    }
}

void DebrisTracer::Log(const std::string& p_Message) {
    Logger::Info("[DebrisTracer] {}", p_Message);

    {
        std::scoped_lock s_Lock(m_LogMutex);

        if (m_File) {
            m_File << fmt::format("[{:.2f}] {}\n", m_Time, p_Message);
            m_File.flush();
        }

        m_Log.push_back(fmt::format("[{:.1f}] {}", m_Time, p_Message));

        while (m_Log.size() > 12) {
            m_Log.pop_front();
        }
    }
}

void DebrisTracer::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    const auto s_FrameStart = Clock::now();
    m_Time += static_cast<float>(p_UpdateEvent.m_GameTimeDelta.ToSeconds());

    // Off: nothing at all, not even 47's position.
    if (!m_Enabled) {
        return;
    }

    // Once per level, a few seconds in: which fragments were there from the start.
    if (!m_HaveBaseline) {
        if (m_BaselineAt < 0.f) {
            m_BaselineAt = m_Time + 3.f;
        }
        else if (m_Time >= m_BaselineAt) {
            TakeBaseline();
        }
    }

    // Read here, on the game's own update, for the window to show.
    const auto s_Hitman = SDK()->GetLocalPlayer();
    m_HitmanInfiniteAmmo = s_Hitman && s_Hitman.m_pInterfaceRef->IsInfiniteAmmoEnabled();

    if (m_ViolinRequested.exchange(false)) {
        GiveViolin();
    }

    if (m_AmmoToggleRequested.exchange(false)) {
        SetInfiniteAmmo();
    }

    if (m_ClearRequested.exchange(false)) {
        std::scoped_lock s_Lock(m_DataMutex);
        m_Tracked.clear();
        m_HeroPath.clear();
        m_TrackedCount = 0;
        Log("Cleared.");
    }

    if (m_RemoveRequested.exchange(false)) {
        RemoveAllFragments();
    }

    const bool s_Testing = m_MeasuringTools && m_ToolAutoTest;
    const bool s_Launching = m_Stage != Stage::Idle || m_SequenceRequested;

    auto s_Mark = Clock::now();
    const auto s_Lap = [&](TimedPart p_Part) {
        const auto s_Now = Clock::now();
        m_TimeNs[p_Part] += Ns(s_Mark, s_Now);
        s_Mark = s_Now;
    };

    // Follows what is switched on: the fragments, 47, or neither.
    UpdateTracked();

    if (m_FollowPieces && m_HaveBaseline && m_Time - m_LastScan > m_ScanInterval) {
        ScanScene();
    }

    s_Lap(kTimePlayer);

    // The measuring tools, each only when switched on - or when the test or a launch needs it.
    if ((m_MeasuringTools && m_SupportRayEachFrame) || s_Launching) {
        FindSupport();
    }

    s_Lap(kTimeRay);

    const bool s_Recording = m_MeasuringTools && m_ToolRecordBoosts;

    const bool s_Describe = m_DescribeRequested.exchange(false);
    const bool s_ProbeSetDown = m_ProbeSetDownRequested.exchange(false);
    const bool s_Collide = m_CollideCheckRequested.exchange(false);

    if ((m_MeasuringTools && (m_ToolPhysX || m_ToolRockets)) || s_Recording || s_Launching || s_Describe ||
        s_ProbeSetDown || s_Collide || m_PxTraceRequested || m_Time < m_PxTraceUntil) {
        ReadPhysX();
    }

    if (s_Collide) {
        CollideCheck();
    }

    if (s_ProbeSetDown) {
        ProbeSetDown();
    }

    if (s_Describe) {
        void* s_Piece = nullptr;
        float s_Touch = 0.f;

        {
            std::scoped_lock s_Lock(m_DataMutex);
            s_Piece = m_Px.TouchedFragment;
            s_Touch = m_Px.TouchZ - m_LastFeet.z;
        }

        if (s_Piece) {
            const std::string s_Facts = PieceFacts(s_Piece);
            std::scoped_lock s_Lock(m_DataMutex);
            Log(fmt::format("Under 47: {} ({}), PhysX touching it {:+.3f} from his feet.", BoostName(s_Piece), s_Facts, s_Touch));
        }
        else {
            Log(fmt::format("Under 47: no fragment - {}.", PxLine()));
        }
    }

    // The boost recorder, only while its switch is on; switched off, whatever it held is dropped.
    if (s_Recording) {
        RecordBoostFrame();
    }
    else if (m_BoostOpen || !m_BoostPre.empty()) {
        ResetBoostRecorder();
    }

    s_Lap(kTimePhysX);

    if (s_Testing || s_Launching) {
        RunSequence();
        s_Lap(kTimeLaunch);
    }

    // While the timers are off, what was summed is thrown away, so switching them on shows this
    // second and not everything since the start divided by a handful of frames.
    if (!(m_MeasuringTools && m_ToolTimers)) {
        for (auto& s_Sum : m_TimeNs) {
            s_Sum = 0;
        }

        m_TimeFrames = 0;
        m_TimeWindowStart = m_Time;
        return;
    }

    m_TimeNs[kTimeFrame] += Ns(s_FrameStart, Clock::now());
    ++m_TimeFrames;

    if (m_Time - m_TimeWindowStart >= 1.f) {
        const int s_Frames = std::max(1, m_TimeFrames.exchange(0));

        for (int i = 0; i < kTimedParts; ++i) {
            m_TimeShownMs[i] = static_cast<double>(m_TimeNs[i].exchange(0)) / s_Frames / 1e6;
        }

        m_TimeWindowStart = m_Time;
    }

    if (m_Time - m_LastTimingLog >= 5.f) {
        m_LastTimingLog = m_Time;

        LogFile(fmt::format(
            "TIMING ms per frame: plugin {:.3f} | tracking {:.3f} | ray {:.3f} | physx {:.3f} | launch {:.3f} | "
            "test {:.3f} | window {:.3f} | 3d {:.3f}", m_TimeShownMs[kTimeFrame], m_TimeShownMs[kTimePlayer],
            m_TimeShownMs[kTimeRay], m_TimeShownMs[kTimePhysX], m_TimeShownMs[kTimeLaunch], m_TimeShownMs[kTimeTest],
            m_TimeShownMs[kTimeWindow], m_TimeShownMs[kTimeDraw3D]
        ));
    }
}

// Asks the engine what 47 is standing on, instead of inferring it from the fact that he rises a
// centimetre - an inference that failed three times over, with the successes and the misses coming
// out identical in every number we recorded.
//
// The trick is where the ray starts. A ray that starts inside him is stopped at zero distance and
// never reaches the ground. Level with his feet it is at the boundary instead, and the first thing
// below is his footing.
void DebrisTracer::FindSupport() {
    if (!Globals::CollisionManager || !*Globals::CollisionManager) {
        return;
    }

    const SVector3 s_From(m_LastFeet.x, m_LastFeet.y, m_LastFeet.z + m_SupportStart);
    const SVector3 s_To(m_LastFeet.x, m_LastFeet.y, m_LastFeet.z - m_SupportReach);

    ZRayQueryInput s_Input {
        .m_vFrom = float4(s_From.x, s_From.y, s_From.z, 1.f),
        .m_vTo = float4(s_To.x, s_To.y, s_To.z, 1.f),
        .m_eType = ECollidablesType::ECST_STATIC_AND_DYNAMIC,
    };
    ZRayQueryOutput s_Out {};

    void* s_Body = nullptr;

    if ((*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Out)) {
        auto* s_Proxy = s_Out.m_pBlockingPhysicsObject.m_pProxy;

        if (Readable(s_Proxy, sizeof(void*))) {
            auto* s_Candidate = *reinterpret_cast<void* const*>(s_Proxy);

            if (LooksLikeGameObject(s_Candidate)) {
                s_Body = s_Candidate;
            }
        }
    }

    std::scoped_lock s_Lock(m_DataMutex);

    m_SupportBody = s_Body;
    m_SupportId = 0;
    m_SupportDrop = s_Out.m_vPosition.z - m_LastFeet.z;
    m_SupportName = s_Body ? DescribePointer(s_Body) : std::string("nothing");

    // Naming it as one of the pieces we follow is what makes the reading usable: standing on debris
    // is only interesting when we know which piece.
    for (const auto& s_Item : m_Tracked) {
        if (s_Item.Object == s_Body) {
            m_SupportId = s_Item.Id;
            break;
        }
    }
}

// Where a loaded DLL starts and ends, from its own header.
bool DebrisTracer::ModuleRange(const char* p_Name, uintptr_t& p_Base, uintptr_t& p_End) const {
    const auto s_Module = reinterpret_cast<uintptr_t>(GetModuleHandleA(p_Name));
    uint32_t s_Header = 0, s_Size = 0;

    if (!s_Module || !ReadRaw(s_Module + 0x3c, s_Header) || !ReadRaw(s_Module + s_Header + 0x50, s_Size)) {
        return false;
    }

    p_Base = s_Module;
    p_End = s_Module + s_Size;
    return true;
}

// Finds 47's controller among PhysX's: the capsule whose centre is right above his feet. Every
// pointer is checked against the vtable Ghidra found for it before anything is called on it.
void DebrisTracer::FindController() {
    m_LastControllerSearch = m_Time;
    m_Px.Controller = nullptr;

    if ((!m_CctBase && !ModuleRange(kCctModuleName, m_CctBase, m_CctEnd)) ||
        (!m_PhysXBase && !ModuleRange(kPhysXModuleName, m_PhysXBase, m_PhysXEnd))) {
        m_Px.Why = "the PhysX DLLs are not loaded";
        return;
    }

    uintptr_t s_Physics = 0, s_PhysicsVTable = 0;

    if (!ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) || !ReadRaw(s_Physics, s_PhysicsVTable)) {
        m_Px.Why = "the physics manager is not readable";
        return;
    }

    if (s_PhysicsVTable - m_ModuleDelta != kPhysicsManagerVTable) {
        m_Px.Why = fmt::format("the global holds vtable ghidra 0x{:x}, not the physics manager",
            s_PhysicsVTable - m_ModuleDelta);
        return;
    }

    uintptr_t s_Manager = 0, s_ManagerVTable = 0;

    if (!ReadRaw(s_Physics + kControllerManagerOffset, s_Manager) || !ReadRaw(s_Manager, s_ManagerVTable) ||
        s_ManagerVTable - m_CctBase + kCctImageBase != kControllerManagerVTable) {
        m_Px.Why = "the controller manager is not where Ghidra put it";
        return;
    }

    auto* const* s_Table = reinterpret_cast<void* const*>(s_ManagerVTable);

    using GetNbControllers = uint32_t(__fastcall*)(void*);
    using GetController = void*(__fastcall*)(void*, uint32_t);

    const uint32_t s_Count =
        reinterpret_cast<GetNbControllers>(s_Table[kGetNbControllersSlot])(reinterpret_cast<void*>(s_Manager));

    m_ControllerCount = static_cast<int>(s_Count);

    if (s_Count > 1024) {
        m_Px.Why = fmt::format("{} controllers is not a believable number", s_Count);
        return;
    }

    const uintptr_t s_Capsule = kCapsuleControllerVTable - kCctImageBase + m_CctBase;
    float s_BestSide = 1.f;

    for (uint32_t i = 0; i < s_Count; ++i) {
        auto* s_Controller =
            reinterpret_cast<GetController>(s_Table[kGetControllerSlot])(reinterpret_cast<void*>(s_Manager), i);

        uintptr_t s_VTable = 0;
        Vec3d s_Centre {};

        if (!ReadRaw(reinterpret_cast<uintptr_t>(s_Controller), s_VTable) || s_VTable != s_Capsule ||
            !ReadRaw(reinterpret_cast<uintptr_t>(s_Controller) + kCentreOffset, s_Centre)) {
            continue;
        }

        const float s_Dx = static_cast<float>(s_Centre.X) - m_LastFeet.x;
        const float s_Dy = static_cast<float>(s_Centre.Y) - m_LastFeet.y;
        const float s_Dz = static_cast<float>(s_Centre.Z) - m_LastFeet.z;
        const float s_Side = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy);

        // The centre of his capsule sits about a metre above his feet.
        if (s_Side < s_BestSide && s_Dz > 0.f && s_Dz < 2.5f) {
            s_BestSide = s_Side;
            m_Px.Controller = s_Controller;
        }
    }

    // The same controller, read where the game keeps it - what StrikerBoost uses from v2.3. Both are written
    // down, and the game's own pointer wins: the search could only ever guess by position.
    uintptr_t s_Wrapper = 0, s_WrapperVTable = 0, s_FromGlobal = 0, s_GlobalVTable = 0;
    const bool s_GlobalOk = ReadRaw(kHeroControllerGlobal + m_ModuleDelta, s_Wrapper) && s_Wrapper &&
        ReadRaw(s_Wrapper, s_WrapperVTable) && s_WrapperVTable - m_ModuleDelta == kCharacterControllerVTable &&
        ReadRaw(s_Wrapper + kWrapperControllerOffset, s_FromGlobal) && s_FromGlobal &&
        ReadRaw(s_FromGlobal, s_GlobalVTable) && s_GlobalVTable == s_Capsule;

    const auto s_Searched = reinterpret_cast<uintptr_t>(m_Px.Controller);

    if (s_GlobalOk) {
        m_Px.Controller = reinterpret_cast<void*>(s_FromGlobal);
    }

    if (!m_Px.Controller) {
        m_Px.Why = fmt::format("none of the {} controllers is at 47, and the game's global holds none", s_Count);
        return;
    }

    m_Px.Why.clear();

    Log(fmt::format(
        "47's PhysX controller: the game's global gives 0x{:x}, the search among {} gives 0x{:x} ({:.2f} m off his "
        "feet) - {}.", s_GlobalOk ? s_FromGlobal : 0, s_Count, s_Searched, s_BestSide,
        !s_GlobalOk ? "GLOBAL UNUSABLE, the search is used" : s_Searched == s_FromGlobal ? "the same" : "DIFFERENT, the global is used"
    ));
}

// What PhysX says, read every frame. The line is written to the log while a launch is under way
// and for a second after it, so every try shows what PhysX was holding when the piece went up.
void DebrisTracer::ReadPhysX() {
    std::scoped_lock s_Lock(m_DataMutex);

    if (m_PxTraceRequested.exchange(false)) {
        m_PxTraceUntil = m_Time + 10.f;
        Log("Writing what PhysX says to the log for 10 s.");
    }

    // Kept only while it is still a capsule controller and still with 47. A controller that has
    // been released reads as something else, and the scene may have been changed under us.
    if (m_Px.Controller) {
        uintptr_t s_VTable = 0;
        Vec3d s_Centre {};
        const auto s_At = reinterpret_cast<uintptr_t>(m_Px.Controller);

        const bool s_Still = ReadRaw(s_At, s_VTable) &&
            s_VTable == kCapsuleControllerVTable - kCctImageBase + m_CctBase &&
            ReadRaw(s_At + kCentreOffset, s_Centre) &&
            std::abs(static_cast<float>(s_Centre.X) - m_LastFeet.x) < 1.5f &&
            std::abs(static_cast<float>(s_Centre.Y) - m_LastFeet.y) < 1.5f;

        if (!s_Still) {
            Log("Lost 47's PhysX controller; looking again.");
            m_Px.Controller = nullptr;
        }
    }

    if (!m_Px.Controller) {
        if (m_Time - m_LastControllerSearch > 1.f) {
            FindController();
        }

        if (!m_Px.Controller) {
            return;
        }
    }

    const auto s_At = reinterpret_cast<uintptr_t>(m_Px.Controller);

    uintptr_t s_Actor = 0;
    uint8_t s_Flags = 0, s_Moving = 0;
    Vec3d s_Centre {};

    ReadRaw(s_At + kTouchedActorOffset, s_Actor);
    ReadRaw(s_At + kRideStepOffset, m_Px.RideStep);
    ReadRaw(s_At + kCollisionFlagsOffset, s_Flags);
    ReadRaw(s_At + kStandingOnMovingOffset, s_Moving);
    ReadRaw(s_At + kCentreOffset, s_Centre);

    Vec3f s_RideSpeed {}, s_Touch {};
    ReadRaw(s_At + kDeltaXPOffset, s_RideSpeed);
    ReadRaw(s_At + kTouchedPosWorldOffset, s_Touch);
    m_Px.RideSpeedZ = s_RideSpeed.Z;
    m_Px.TouchZ = s_Touch.Z;

    m_Px.TouchedActor = reinterpret_cast<void*>(s_Actor);
    m_Px.Grounded = (s_Flags & kCollisionDown) != 0;
    m_Px.Moving = s_Moving != 0;
    m_Px.CentreZ = s_Centre.Z;

    uint16_t s_Type = 0;
    m_Px.TouchedType = s_Actor && ReadRaw(s_Actor + kConcreteTypeOffset, s_Type) ? s_Type : 0;

    m_Px.TouchedFragment = m_Px.TouchedType == 6 ? FragmentFromActor(s_Actor) : nullptr;

    // The scene's step counter, asked the way the ride asks it. The scene must be one of PhysX's own
    // objects before anything is called on it.
    uintptr_t s_Scene = 0, s_SceneVTable = 0, s_Method = 0;

    if (ReadRaw(s_At + kSceneOffset, s_Scene) && ReadRaw(s_Scene, s_SceneVTable) &&
        s_SceneVTable >= m_PhysXBase && s_SceneVTable < m_PhysXEnd &&
        ReadRaw(s_SceneVTable + kSceneGetTimestampSlot * sizeof(void*), s_Method) &&
        s_Method >= m_PhysXBase && s_Method < m_PhysXEnd) {
        using GetTimestamp = uint32_t(__fastcall*)(void*);
        m_Px.Step = reinterpret_cast<GetTimestamp>(s_Method)(reinterpret_cast<void*>(s_Scene));
    }

    // Which of our pieces it is, if it is one.
    m_Px.TouchedPiece = 0;

    for (const auto& s_Item : m_Tracked) {
        uintptr_t s_ItemActor = 0;

        if (s_Item.Alive && ReadRaw(reinterpret_cast<uintptr_t>(s_Item.Object) + kActorOffset, s_ItemActor) &&
            s_ItemActor == s_Actor && s_Actor != 0) {
            m_Px.TouchedPiece = s_Item.Id;
            break;
        }
    }

    if (m_Stage != Stage::Idle || m_Time < m_PxTraceUntil) {
        std::string s_Piece;

        if (m_LaunchedPiece && LooksLikeGameObject(m_LaunchedPiece)) {
            const float4 s_At = static_cast<IPhysicsObject*>(m_LaunchedPiece)->GetPosition();
            s_Piece = fmt::format(" | launched piece at z={:.2f}", s_At.z);
        }

        LogFile("PX " + PxLine() + s_Piece);
    }
}

// One line of what PhysX holds. Needs no lock of its own: it only formats what ReadPhysX stored.
std::string DebrisTracer::PxLine() const {
    if (!m_Px.Controller) {
        return fmt::format("no controller ({})", m_Px.Why);
    }

    std::string s_On;

    if (!m_Px.TouchedActor) {
        s_On = "nothing";
    }
    else if (m_Px.TouchedPiece > 0) {
        s_On = fmt::format("PIECE #{}", m_Px.TouchedPiece);
    }
    else if (m_Px.TouchedFragment) {
        s_On = "a FRAGMENT";
    }
    else if (m_Px.TouchedType == 7) {
        s_On = "something static";
    }
    else {
        s_On = fmt::format("another body (type {}, 0x{:x})", m_Px.TouchedType,
            reinterpret_cast<uintptr_t>(m_Px.TouchedActor));
    }

    return fmt::format(
        "feet z={:.3f} centre z={:.3f} | on {} (touched at {:+.3f} from his feet) | step {} ride worked out at {}{} | "
        "moving {} | grounded {}",
        m_LastFeet.z, m_Px.CentreZ, s_On, m_Px.TouchZ - m_LastFeet.z, m_Px.Step, m_Px.RideStep,
        m_Px.TouchedType == 6 && m_Px.Step == m_Px.RideStep ? " (SAME: a ride now would be skipped)" : "", m_Px.Moving ? "yes" : "no",
        m_Px.Grounded ? "yes" : "no"
    );
}

void* DebrisTracer::FragmentFromActor(uintptr_t p_Actor) const {
    uintptr_t s_UserData = 0, s_UserDataVTable = 0, s_ObjectVTable = 0;

    if (!ReadRaw(p_Actor + kActorUserDataOffset, s_UserData) || !ReadRaw(s_UserData, s_UserDataVTable) ||
        s_UserDataVTable - m_ModuleDelta != kFragmentUserDataVTable) {
        return nullptr;
    }

    const uintptr_t s_Object = s_UserData - kUserDataToObject;

    if (!ReadRaw(s_Object, s_ObjectVTable) || s_ObjectVTable - m_ModuleDelta != kFragmentVTable) {
        return nullptr;
    }

    return reinterpret_cast<void*>(s_Object);
}

void DebrisTracer::FindPiecesNow() {
    {
        std::scoped_lock s_Lock(m_DataMutex);
        m_Tracked.clear();
        m_TrackedCount = 0;
    }

    Probe(false);
}

// Compares the piece's own PhysX actor with the one PhysX has as 47's touched object.
bool DebrisTracer::PhysXHolds(void* p_Piece) const {
    uintptr_t s_Actor = 0;

    return p_Piece && m_Px.Controller && m_Px.TouchedActor &&
        ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kActorOffset, s_Actor) &&
        reinterpret_cast<void*>(s_Actor) == m_Px.TouchedActor;
}

// Every pointer is checked before anything is called: the scene's and the actor's methods must lie
// inside PhysX, and the box must be the size of a piece and near its origin. A box that fails any of
// it is not used, and the piece is placed the old way. The origin need not be inside the box: the
// first test found it up to 19 cm outside.
bool DebrisTracer::ReadPieceBox(void* p_Piece, PieceBox& p_Out, std::string& p_Why) const {
    const auto s_InPhysX = [this](uintptr_t p_Address) {
        return p_Address >= m_PhysXBase && p_Address < m_PhysXEnd;
    };

    uintptr_t s_Physics = 0, s_Scene = 0, s_SceneVTable = 0, s_Lock = 0, s_Unlock = 0;
    uintptr_t s_Actor = 0, s_ActorVTable = 0, s_Bounds = 0, s_Pose = 0;
    uint16_t s_Type = 0;

    if (!m_PhysXBase) {
        p_Why = "PhysX not located";
        return false;
    }

    if (!ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) ||
        !ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) || !ReadRaw(s_Scene, s_SceneVTable) ||
        !ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) ||
        !ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) || !s_InPhysX(s_Lock) || !s_InPhysX(s_Unlock)) {
        p_Why = "no scene lock";
        return false;
    }

    if (!ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kActorOffset, s_Actor) ||
        !ReadRaw(s_Actor + kConcreteTypeOffset, s_Type) || s_Type != 6 || !ReadRaw(s_Actor, s_ActorVTable) ||
        !ReadRaw(s_ActorVTable + kGetWorldBoundsSlot * sizeof(void*), s_Bounds) || !s_InPhysX(s_Bounds) ||
        !ReadRaw(s_ActorVTable + kGetGlobalPoseSlot * sizeof(void*), s_Pose) || !s_InPhysX(s_Pose)) {
        p_Why = "no moving actor";
        return false;
    }

    PxBounds s_Box {};
    PxPose s_Real {};
    const auto s_LockFn = reinterpret_cast<SceneLockFn>(s_Lock);
    const auto s_UnlockFn = reinterpret_cast<SceneUnlockFn>(s_Unlock);

    if (!CallActorGetter(s_Scene, s_LockFn, s_UnlockFn, s_Actor, reinterpret_cast<ActorGetterFn>(s_Bounds), &s_Box) ||
        !CallActorGetter(s_Scene, s_LockFn, s_UnlockFn, s_Actor, reinterpret_cast<ActorGetterFn>(s_Pose), &s_Real)) {
        p_Why = "the call failed";
        return false;
    }

    struct { float V[4]; } s_Cached {};

    if (!ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kCachedRotationOffset, s_Cached)) {
        p_Why = "no cached rotation";
        return false;
    }

    const float4 s_At = static_cast<IPhysicsObject*>(p_Piece)->GetPosition();
    const float s_Size = std::max({ s_Box.MaxX - s_Box.MinX, s_Box.MaxY - s_Box.MinY, s_Box.MaxZ - s_Box.MinZ });
    const float s_DX = s_At.x - (s_Box.MinX + s_Box.MaxX) * 0.5f;
    const float s_DY = s_At.y - (s_Box.MinY + s_Box.MaxY) * 0.5f;
    const float s_DZ = s_At.z - (s_Box.MinZ + s_Box.MaxZ) * 0.5f;
    const float s_ToMiddle = std::sqrt(s_DX * s_DX + s_DY * s_DY + s_DZ * s_DZ);

    if (!(s_Size > 0.005f && s_Size < 2.f) || s_ToMiddle > 1.5f) {
        p_Why = fmt::format(
            "box makes no sense: ({:.2f}, {:.2f}, {:.2f})-({:.2f}, {:.2f}, {:.2f}) for a piece at ({:.2f}, {:.2f}, {:.2f})",
            s_Box.MinX, s_Box.MinY, s_Box.MinZ, s_Box.MaxX, s_Box.MaxY, s_Box.MaxZ, s_At.x, s_At.y, s_At.z
        );
        return false;
    }

    // The angle between two rotations, from the dot product of the quaternions.
    const float s_Dot = std::min(1.f, std::abs(s_Real.QX * s_Cached.V[0] + s_Real.QY * s_Cached.V[1] +
        s_Real.QZ * s_Cached.V[2] + s_Real.QW * s_Cached.V[3]));

    p_Out.MinX = s_Box.MinX;
    p_Out.MinY = s_Box.MinY;
    p_Out.MinZ = s_Box.MinZ;
    p_Out.MaxX = s_Box.MaxX;
    p_Out.MaxY = s_Box.MaxY;
    p_Out.MaxZ = s_Box.MaxZ;
    p_Out.Q[0] = s_Real.QX;
    p_Out.Q[1] = s_Real.QY;
    p_Out.Q[2] = s_Real.QZ;
    p_Out.Q[3] = s_Real.QW;
    std::copy(std::begin(s_Cached.V), std::end(s_Cached.V), std::begin(p_Out.CachedQ));
    p_Out.RotationGapDeg = 2.f * std::acos(s_Dot) * 57.29578f;
    p_Out.OriginToMiddle = s_ToMiddle;
    return true;
}

// One line on where a piece is: its box against 47's feet, how far its origin is from the middle,
// and how far the game's copy of its rotation is from the real one.
// A piece's mass as PhysX has it, and the size of the box it fills: a flower's stem or petal is thin
// and long, a shard of the vase wide and curved.
std::string DebrisTracer::PieceFacts(void* p_Piece) {
    if (!p_Piece) {
        return "no piece";
    }

    std::string s_Mass = "mass unreadable";
    uintptr_t s_Actor = 0, s_ActorVTable = 0, s_Get = 0, s_Physics = 0, s_Scene = 0, s_SceneVTable = 0;
    uintptr_t s_Lock = 0, s_Unlock = 0;
    uint16_t s_Type = 0;

    if (ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kActorOffset, s_Actor) &&
        ReadRaw(s_Actor + kConcreteTypeOffset, s_Type) && s_Type == 6 && ReadRaw(s_Actor, s_ActorVTable) &&
        ReadRaw(s_ActorVTable + kGetMassOffset, s_Get) && ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) &&
        ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) && ReadRaw(s_Scene, s_SceneVTable) &&
        ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) && ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) &&
        s_Get >= m_PhysXBase && s_Get < m_PhysXEnd && s_Lock >= m_PhysXBase && s_Lock < m_PhysXEnd &&
        s_Unlock >= m_PhysXBase && s_Unlock < m_PhysXEnd) {
        float s_Kg = 0.f;

        if (CallGetMass(s_Scene, reinterpret_cast<SceneLockFn>(s_Lock), reinterpret_cast<SceneUnlockFn>(s_Unlock),
            reinterpret_cast<GetMassFn>(s_Get), s_Actor, s_Kg)) {
            s_Mass = fmt::format("mass {:.4f} kg", s_Kg);
        }
    }

    PieceBox s_Box;
    std::string s_Why;

    if (!ReadPieceBox(p_Piece, s_Box, s_Why)) {
        return s_Mass + ", no box (" + s_Why + ")";
    }

    return fmt::format(
        "{}, box {:.3f} x {:.3f} x {:.3f} m (high {:.3f})", s_Mass, s_Box.MaxX - s_Box.MinX, s_Box.MaxY - s_Box.MinY,
        s_Box.MaxZ - s_Box.MinZ, s_Box.MaxZ - s_Box.MinZ
    );
}

// The game's set-down sweep, run by hand from where 47 stands: the same collision manager call, with the
// query FUN_1410d78c0 builds after a ride - a sphere half his capsule's radius, from 1 m above his feet
// down to 3 m below them, with his controller's mask - and what it hits against the piece he is on.
// Nothing is moved: the game would put his feet where this hits.
void DebrisTracer::ProbeSetDown(bool p_Locked) {
    constexpr float kStartAbove = 1.f;   // DAT_141f9cf18 in FUN_1410d78c0
    constexpr float kRadiusShare = 0.5f; // DAT_141f9cd84
    constexpr float kProbeDepth = 3.f;   // Below his feet: past any piece, down to the floor under it.

    void* s_Controller = nullptr;
    void* s_Piece = nullptr;
    std::string s_Held;

    {
        std::unique_lock<std::mutex> s_Lock(m_DataMutex, std::defer_lock);
        if (!p_Locked) {
            s_Lock.lock();
        }

        s_Controller = m_Px.Controller;
        s_Piece = m_Px.TouchedFragment;
        s_Held = PxLine();
    }

    if (!s_Controller) {
        Log("Set-down probe: no PhysX controller for 47 yet - try again in a second.");
        return;
    }

    // 47's controller on the game's side, reached from PhysX's userData and checked both ways.
    const auto s_P = reinterpret_cast<uintptr_t>(s_Controller);
    uintptr_t s_Callback = 0, s_WrapperVTable = 0, s_WrapperP = 0;

    if (!ReadRaw(s_P + kPxReportCallbackOffset, s_Callback) || s_Callback < 0x10000 ||
        !ReadRaw(s_Callback - 0x10, s_WrapperVTable) || s_WrapperVTable - m_ModuleDelta != kCharacterControllerVTable ||
        !ReadRaw(s_Callback - 0x10 + kWrapperControllerOffset, s_WrapperP) || s_WrapperP != s_P) {
        Log(fmt::format(
            "Set-down probe: 47's game-side controller is not where Ghidra put it (report callback 0x{:x}, vtable ghidra 0x{:x}).",
            s_Callback, s_WrapperVTable - m_ModuleDelta
        ));
        return;
    }

    const uintptr_t s_Wrapper = s_Callback - 0x10;
    int s_Layer = -1;
    float s_MaxStep = 0.f, s_Radius = 0.f;

    ReadRaw(s_Wrapper + kWrapperLayerOffset, s_Layer);
    ReadRaw(s_Wrapper + kWrapperMaxStepOffset, s_MaxStep);
    ReadRaw(s_P + kPxRadiusOffset, s_Radius);

    // The collision manager and the three methods, each checked against the address Ghidra read.
    uintptr_t s_Cm = 0, s_CmVTable = 0, s_LayerFn = 0, s_MaskFn = 0, s_SweepFn = 0;

    if (!ReadRaw(kCollisionManagerGlobal + m_ModuleDelta, s_Cm) || !ReadRaw(s_Cm, s_CmVTable) ||
        s_CmVTable - m_ModuleDelta != kCollisionManagerVTable ||
        !ReadRaw(s_CmVTable + kCmLayerOffset, s_LayerFn) || s_LayerFn - m_ModuleDelta != kCmLayerFunction ||
        !ReadRaw(s_CmVTable + kCmMaskOffset, s_MaskFn) || s_MaskFn - m_ModuleDelta != kCmMaskFunction ||
        !ReadRaw(s_CmVTable + kCmSweepOffset, s_SweepFn) || s_SweepFn - m_ModuleDelta != kCmSweepFunction) {
        Log("Set-down probe: the collision manager is not the one Ghidra read - not calling anything.");
        return;
    }

    // The mask as FUN_1410d78c0 builds it, and the table index the sweep derives from it.
    auto* const s_CmObject = reinterpret_cast<void*>(s_Cm);
    uint32_t s_Mask = 0xffffffff;
    uint16_t s_LayerIndex = 0, s_Index = 0;

    if (s_Layer != -1 &&
        (!CallCmLayer(reinterpret_cast<CmLayerFn>(s_LayerFn), s_CmObject, s_Layer, s_LayerIndex) ||
         !CallCmMask(reinterpret_cast<CmMaskFn>(s_MaskFn), s_CmObject, s_LayerIndex, s_Mask))) {
        Log("Set-down probe: the layer calls failed.");
        return;
    }

    if (!CallCmLayer(reinterpret_cast<CmLayerFn>(s_LayerFn), s_CmObject, static_cast<int>(s_Mask), s_Index)) {
        Log("Set-down probe: the layer call failed.");
        return;
    }

    uint32_t s_Word1Table = 0, s_Word3Table = 0, s_Matrix = 0;
    ReadRaw(s_Cm + kCmWord1TableOffset + s_Index * 4u, s_Word1Table);
    ReadRaw(s_Cm + kCmWord3TableOffset + s_Index * 4u, s_Word3Table);
    ReadRaw(s_Cm + kCmMatrixOffset + s_LayerIndex * 4u, s_Matrix);

    // His feet, from PhysX's own getFootPosition, as the set-down reads them.
    uintptr_t s_PVTable = 0, s_GetFoot = 0;
    double s_Foot[3] = {};

    if (!ReadRaw(s_P, s_PVTable) || !ReadRaw(s_PVTable + kPxGetFootOffset, s_GetFoot) || s_GetFoot < m_CctBase ||
        s_GetFoot >= m_CctEnd || !CallGetFoot(reinterpret_cast<GetFootFn>(s_GetFoot), s_Controller, s_Foot)) {
        Log("Set-down probe: could not read his feet from PhysX.");
        return;
    }

    const float s_FootX = static_cast<float>(s_Foot[0]), s_FootY = static_cast<float>(s_Foot[1]);
    const float s_FootZ = static_cast<float>(s_Foot[2]);

    // The query, laid out as FUN_1410d78c0 lays it out: start, radius, end, mask, type 0 (static and
    // moving bodies).
    alignas(16) float s_Query[16] = {};
    s_Query[0] = s_FootX;
    s_Query[1] = s_FootY;
    s_Query[2] = s_FootZ + kStartAbove;
    s_Query[3] = 1.f;
    s_Query[4] = s_Radius * kRadiusShare;
    s_Query[8] = s_FootX;
    s_Query[9] = s_FootY;
    s_Query[10] = s_FootZ - kProbeDepth;
    s_Query[11] = 1.f;
    std::memcpy(&s_Query[12], &s_Mask, sizeof(s_Mask));

    alignas(16) float s_Result[32] = {};
    uint64_t s_Returned = 0;

    if (!CallCmSweep(reinterpret_cast<CmSweepFn>(s_SweepFn), s_CmObject, s_Result, s_Query, s_Returned)) {
        Log("Set-down probe: the sweep call failed.");
        return;
    }

    const bool s_Hit = (s_Returned & 0xff) != 0 && reinterpret_cast<const uint8_t*>(s_Result)[0x3c] != 0;
    const float s_HitX = s_Result[4], s_HitY = s_Result[5], s_HitZ = s_Result[6];

    Log(fmt::format(
        "SET-DOWN PROBE: 47 layer {} (index {}, collides with 0x{:08x}), mask 0x{:08x}; the sweep uses table index "
        "{}: word1 0x{:08x}, word3 0x{:08x}. Max step {:.2f} m, sphere {:.3f} m from z={:.3f} to z={:.3f}.", s_Layer,
        s_LayerIndex, s_Matrix, s_Mask, s_Index, s_Word1Table, s_Word3Table, s_MaxStep, s_Query[4], s_Query[2],
        s_Query[10]
    ));

    Log(s_Hit ? fmt::format(
                    "SET-DOWN PROBE: it hits at z={:.3f} ({:+.3f} from his feet), normal z {:.2f}; PhysX: {}.", s_HitZ,
                    s_HitZ - s_FootZ, s_Result[10], s_Held
                )
              : fmt::format("SET-DOWN PROBE: it hits NOTHING down to {:.1f} m below his feet; PhysX: {}.", kProbeDepth, s_Held));

    if (!s_Piece) {
        return;
    }

    // The piece he is on: its box, and the query filter data of each of its shapes, judged by both rules.
    PieceBox s_Box;
    std::string s_Why;
    const bool s_HaveBox = ReadPieceBox(s_Piece, s_Box, s_Why);

    uintptr_t s_Actor = 0, s_ActorVTable = 0, s_GetShapes = 0, s_Physics = 0, s_Scene = 0, s_SceneVTable = 0;
    uintptr_t s_Lock = 0, s_Unlock = 0;
    uint32_t s_Words[16] = {};
    uint32_t s_Count = 0;
    std::string s_Shapes;

    if (ReadRaw(reinterpret_cast<uintptr_t>(s_Piece) + kActorOffset, s_Actor) && ReadRaw(s_Actor, s_ActorVTable) &&
        ReadRaw(s_ActorVTable + kGetShapesOffset, s_GetShapes) && s_GetShapes >= m_PhysXBase && s_GetShapes < m_PhysXEnd &&
        ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) && ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) &&
        ReadRaw(s_Scene, s_SceneVTable) && ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) &&
        ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) && s_Lock >= m_PhysXBase && s_Lock < m_PhysXEnd &&
        s_Unlock >= m_PhysXBase && s_Unlock < m_PhysXEnd &&
        CallShapeFilters(s_Scene, reinterpret_cast<SceneLockFn>(s_Lock), reinterpret_cast<SceneUnlockFn>(s_Unlock),
            reinterpret_cast<GetShapesFn>(s_GetShapes), s_Actor, m_PhysXBase, m_PhysXEnd, s_Words, s_Count)) {
        for (uint32_t i = 0; i < s_Count; ++i) {
            const uint32_t* s_W = s_Words + 4 * i;
            const bool s_MoveSees = (s_W[0] & s_Mask) != 0;
            const bool s_SweepSees = (s_W[1] & s_Word1Table) != 0 && (s_Word3Table == 0 || (s_W[3] & s_Word3Table) != 0);

            s_Shapes += fmt::format(
                " | shape {}: words 0x{:08x} 0x{:08x} 0x{:08x} 0x{:08x} -> PhysX's move {} it, the set-down sweep {} it",
                i, s_W[0], s_W[1], s_W[2], s_W[3], s_MoveSees ? "SEES" : "ignores", s_SweepSees ? "SEES" : "IGNORES"
            );
        }
    }
    else {
        s_Shapes = " | its shapes could not be read";
    }

    std::string s_Name;

    {
        std::unique_lock<std::mutex> s_Lock2(m_DataMutex, std::defer_lock);
        if (!p_Locked) {
            s_Lock2.lock();
        }

        s_Name = BoostName(s_Piece);
    }

    std::string s_Verdict = "no box for the piece";

    if (s_HaveBox) {
        constexpr float kSlack = 0.03f;
        const bool s_InsideXY = s_HitX >= s_Box.MinX - kSlack && s_HitX <= s_Box.MaxX + kSlack && s_HitY >= s_Box.MinY - kSlack &&
            s_HitY <= s_Box.MaxY + kSlack;

        if (!s_Hit) {
            s_Verdict = "the sweep hit nothing at all";
        }
        else if (s_InsideXY && s_HitZ >= s_Box.MinZ - kSlack && s_HitZ <= s_Box.MaxZ + kSlack) {
            s_Verdict = "THE SWEEP LANDS ON THE PIECE: after a ride he would stay on it";
        }
        else if (s_HitZ < s_Box.MinZ - kSlack) {
            s_Verdict = fmt::format(
                "THE SWEEP GOES THROUGH THE PIECE and lands {:.3f} m below its bottom: after a ride he would drop to the "
                "first solid thing below", s_Box.MinZ - s_HitZ
            );
        }
        else {
            s_Verdict = "the sweep hit something else, above or beside the piece";
        }
    }

    Log(fmt::format(
        "SET-DOWN PROBE: piece {} ({}), box z {:.3f} to {:.3f}{}. VERDICT: {}.", s_Name, PieceFacts(s_Piece),
        s_Box.MinZ, s_Box.MaxZ, s_Shapes, s_Verdict
    ));
}

std::string DebrisTracer::DescribeBox(void* p_Piece) {
    PieceBox s_Box;
    std::string s_Why;

    if (!p_Piece || !LooksLikeGameObject(p_Piece)) {
        return "piece gone";
    }

    if (!ReadPieceBox(p_Piece, s_Box, s_Why)) {
        return "no box: " + s_Why;
    }

    return fmt::format(
        "box bottom {:+.3f} top {:+.3f} from his feet, middle ({:+.2f}, {:+.2f}) from him, origin {:.2f} m from the "
        "middle, game's rotation {:.1f} deg off the real one", s_Box.MinZ - m_LastFeet.z, s_Box.MaxZ - m_LastFeet.z,
        (s_Box.MinX + s_Box.MaxX) * 0.5f - m_LastFeet.x, (s_Box.MinY + s_Box.MaxY) * 0.5f - m_LastFeet.y,
        s_Box.OriginToMiddle, s_Box.RotationGapDeg
    );
}

// Every move of a piece goes through here. With "keep the real rotation" on, it goes through the
// game's own pose setter (slot 8) with the rotation PhysX really has, instead of SetPosition, which
// sends the game's copy of the rotation along with the new position.
void DebrisTracer::MovePiece(void* p_Piece, const float4& p_Position) {
    auto* s_Body = static_cast<IPhysicsObject*>(p_Piece);

    if (m_KeepRealRotation) {
        PieceBox s_Box;
        std::string s_Why;
        uintptr_t s_VTable = 0, s_SetPose = 0;

        if (ReadPieceBox(p_Piece, s_Box, s_Why) && ReadRaw(reinterpret_cast<uintptr_t>(p_Piece), s_VTable) &&
            ReadRaw(s_VTable + kSetPoseSlot * sizeof(void*), s_SetPose) &&
            s_SetPose - m_ModuleDelta == kSetPoseFunction) {
            GamePose s_Pose { s_Box.Q[0], s_Box.Q[1], s_Box.Q[2], s_Box.Q[3], p_Position.x, p_Position.y,
                p_Position.z, p_Position.w };

            reinterpret_cast<void (*)(void*, const GamePose*)>(s_SetPose)(p_Piece, &s_Pose);
            return;
        }

        LogFile("Could not move the piece keeping its rotation (" + s_Why + "); used SetPosition.");
    }

    s_Body->SetPosition(p_Position);
}

// Results written down without the settings that produced them cannot be attributed to anything
// afterwards - the error made three times over. Every launch says what it was told to do, first.
void DebrisTracer::LogSettings() {
    LogFile(fmt::format(
        "LAUNCH by {:.2f} m | tries {} | gap to {:.3f} | hop {:.2f} | footing tries {} | support ray {:+.3f} to "
        "-{:.2f} | place {} | clearance {:.3f} | always stage {} | real rotation {} | hop margin {:.2f} | tallest {}", m_LiftAmount, m_MaxRetries, m_GapAllowed,
        m_HopUp, m_MaxAttempts, m_SupportStart, m_SupportReach, m_PlaceMode, m_PlaceClearance, m_AlwaysStage,
        m_KeepRealRotation, m_HopMargin, m_PreferBig
    ));
}

// Sends the piece up, and sets up for another go if the first does not take. Both ways in - the
// piece he was already standing on, and the one whose gap had to be closed - end here, so the
// retrying is in one place rather than two.
void DebrisTracer::FireLaunch(void* p_Body, float p_HomeZ, const std::string& p_Why) {
    m_AutoUsedPiece = p_Body;
    LogSettings();
    Log(p_Why);

    m_LaunchedPiece = p_Body;
    m_PieceHomeZ = p_HomeZ;
    m_LaunchBaseZ = m_LastFeet.z;
    m_RetriesLeft = m_LaunchSideways > 0.f ? 1 : m_MaxRetries;
    m_RetryPutBack = false;
    m_Sends = 0;
    m_WaitFrames = 0;
    m_Stage = Stage::Retrying;

    // Straight away if this very frame is already the right one.
    TrySend();
}

bool DebrisTracer::ReadyToSend(void* p_Piece) const {
    return PhysXHolds(p_Piece) && m_Px.Step != m_Px.RideStep;
}

// Sends the piece up if the carry can happen this frame, and says so in the log with what PhysX
// held at that moment. Otherwise it only counts the frame waited.
bool DebrisTracer::TrySend() {
    if (!m_LaunchedPiece || !ReadyToSend(m_LaunchedPiece)) {
        ++m_WaitFrames;
        return false;
    }

    auto* s_Piece = static_cast<IPhysicsObject*>(m_LaunchedPiece);
    const float4 s_At = s_Piece->GetPosition();

    LogFile(fmt::format("PX at {}: ", m_Sends == 0 ? "launch" : "retry") + PxLine() +
        fmt::format(" | waited {} frames", m_WaitFrames));
    m_PxTraceUntil = m_Time + 1.f;

    LogFile("Before the send: " + DescribeBox(m_LaunchedPiece));

    // Before the piece moves: where the set-down sphere would look, and what it finds there.
    if (m_MeasuringTools && m_ToolThirdFactor) {
        Log(fmt::format("THIRD FACTOR at send {}: {}.", m_Sends + 1, ThirdFactorLine()));
        ProbeSetDown(true);
    }

    MovePiece(m_LaunchedPiece, float4(s_At.x + m_LaunchSideways, s_At.y, m_PieceHomeZ + m_LiftAmount, s_At.w));
    m_CheckPiece = m_LaunchedPiece;
    m_CheckWhat = "Frame after the send: ";
    m_CheckIn = 1;

    if (m_Sends == 0) {
        Log(fmt::format("Sent piece up to z={:.2f}, {:.2f} m sideways, after waiting {} frames.", m_PieceHomeZ + m_LiftAmount, m_LaunchSideways,
            m_WaitFrames));
    }

    ++m_Sends;
    --m_RetriesLeft;
    m_WaitFrames = 0;
    m_RetryPutBack = true;

    return true;
}

void DebrisTracer::RunSequence() {
    // Where the last move really put the piece, a frame later.
    if (m_CheckPiece && --m_CheckIn <= 0) {
        LogFile(m_CheckWhat + DescribeBox(m_CheckPiece) + fmt::format(" | 47 (game) feet z={:.3f} | PhysX: ", m_LastFeet.z) + PxLine());
        m_CheckPiece = nullptr;
    }

    if (m_SequenceRequested.exchange(false)) {
        m_StageDX = m_StageDY = 0.f;

        // Not following the pieces, they are looked for now - and only when PhysX does not already
        // have him on one, which is when a piece has to be put under him. Before the lock: the rays
        // take it themselves.
        if (!m_FollowPieces && !m_Px.TouchedFragment && !m_AutoPiece) {
            FindPiecesNow();
        }

        std::scoped_lock s_Lock(m_DataMutex);

        if (m_Stage != Stage::Idle) {
            Log("Still working on the last one.");
            return;
        }

        // If PhysX already holds one of our pieces as his footing, that is the one the carry will use:
        // it is launched as it lies. This is asked of PhysX itself, which decides with the sweep of his
        // whole capsule - not of a single ray down his middle, which saw the floor where PhysX saw
        // the piece for ten launches running.
        if (!m_AutoPiece && m_Px.TouchedFragment && LooksLikeGameObject(m_Px.TouchedFragment)) {
            const float4 s_At = static_cast<IPhysicsObject*>(m_Px.TouchedFragment)->GetPosition();

            FireLaunch(m_Px.TouchedFragment, s_At.z, fmt::format(
                "PhysX already has him on a fragment, found through its actor. Launching it as it lies, from "
                "z={:.2f}.", s_At.z
            ));

            return;
        }

        // If the engine already counts a fragment as his footing, that fragment is the answer: it
        // needs no moving, no hop and no waiting, and it is what happens in the game anyway.
        if (!m_AutoPiece && m_SupportId > 0 && m_SupportBody && LooksLikeGameObject(m_SupportBody)) {
            const float s_Gap = -m_SupportDrop;

            // A piece a couple of centimetres below his feet is what he is standing on and still
            // too far away to take him anywhere, so the gap is closed first.
            if (s_Gap > m_GapAllowed) {
                m_StagedPiece = m_SupportBody;
                m_Stage = Stage::Tightening;
                m_GapFrames = 0;

                Log(fmt::format(
                    "Piece #{} is his footing but sits {:.3f} m below his feet. Closing that before launching.",
                    m_SupportId, s_Gap
                ));

                return;
            }

            auto* s_Footing = static_cast<IPhysicsObject*>(m_SupportBody);
            const float4 s_At = s_Footing->GetPosition();

            FireLaunch(m_SupportBody, s_At.z, fmt::format(
                "Launched piece #{} straight away - flush with his feet ({:.3f} m), from z={:.2f}.",
                m_SupportId, s_Gap, s_At.z
            ));

            return;
        }

        // The piece under his feet, or else the nearest one lying still near him.
        void* s_Chosen = m_AutoPiece ? m_AutoPiece : (m_UnderObject ? m_UnderObject : PickRestingPiece(m_LastFeet));
        m_AutoPiece = nullptr;

        if (!s_Chosen || !LooksLikeGameObject(s_Chosen)) {
            Log("Nothing under 47 to stage.");
            return;
        }

        m_StagedPiece = s_Chosen;
        m_AutoUsedPiece = s_Chosen;

        int s_UnderId = 0;

        for (const auto& s_Item : m_Tracked) {
            if (s_Item.Object == m_StagedPiece) {
                s_UnderId = s_Item.Id;
                break;
            }
        }

        const std::string s_Standing = m_SupportId > 0
            ? fmt::format("piece #{}", m_SupportId)
            : m_SupportName;

        // Under his feet. The old way put its origin level with them - but the origin is the
        // middle of the piece, so it sat half in the floor. By its box, its bottom goes just above
        // his feet and its middle right under him.
        auto* s_Body = static_cast<IPhysicsObject*>(m_StagedPiece);
        const float4 s_Now = s_Body->GetPosition();
        float s_Height = m_LastFeet.z;
        float s_Hop = m_HopUp;
        std::string s_How = "origin at his feet";

        const bool s_WantBox = m_PlaceMode == 1 || (m_PlaceMode == 2 && m_StageCount % 2 == 1);
        ++m_StageCount;
        m_StageByBox = false;
        m_StageDX = m_StageDY = 0.f;

        if (s_WantBox) {
            PieceBox s_Box;
            std::string s_Why;

            if (ReadPieceBox(m_StagedPiece, s_Box, s_Why)) {
                const float s_Below = s_Now.z - s_Box.MinZ;
                const float s_Tall = s_Box.MaxZ - s_Box.MinZ;

                m_StageByBox = true;
                m_StageDX = s_Now.x - (s_Box.MinX + s_Box.MaxX) * 0.5f;
                m_StageDY = s_Now.y - (s_Box.MinY + s_Box.MaxY) * 0.5f;
                s_Height = m_LastFeet.z + s_Below + m_PlaceClearance;
                s_Hop = std::max(m_HopUp, s_Tall + m_PlaceClearance + m_HopMargin);
                s_How = fmt::format(
                    "BY ITS BOX: {:.2f} x {:.2f} x {:.2f} m, reaching {:.3f} m below its origin; origin {:+.3f} "
                    "from his feet, hop {:.2f}", s_Box.MaxX - s_Box.MinX, s_Box.MaxY - s_Box.MinY, s_Tall, s_Below,
                    s_Height - m_LastFeet.z, s_Hop
                );
            }
            else {
                s_How = fmt::format("origin at his feet (box unusable: {})", s_Why);
            }
        }

        ++m_StageTried[m_StageByBox ? 1 : 0];

        LogFile("Before staging: " + DescribeBox(m_StagedPiece));
        MovePiece(m_StagedPiece, float4(m_LastFeet.x + m_StageDX, m_LastFeet.y + m_StageDY, s_Height, s_Now.w));
        m_CheckPiece = m_StagedPiece;
        m_CheckWhat = "Frame after staging: ";
        m_CheckIn = 1;

        // A piece cannot be pushed under him - every collision is settled in his favour, so a
        // fragment shoved upwards is brushed aside. He can be moved, though: lifted above the piece,
        // he falls back onto it, and from then on the engine counts it as his footing of its own
        // accord. Placed by its box the piece stands above his feet, so he is lifted over its top.
        if (s_Hop > 0.f) {
            auto s_Hitman = SDK()->GetLocalPlayer();
            auto* s_Spatial = s_Hitman ? s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

            if (s_Spatial) {
                SMatrix s_Transform = s_Spatial->GetObjectToWorldMatrix();
                s_Transform.Trans = float4(s_Transform.Trans.x, s_Transform.Trans.y,
                    s_Transform.Trans.z + s_Hop, 1.f);

                s_Spatial->SetObjectToWorldMatrixFromEditor(s_Transform);
            }
        }

        m_Stage = Stage::Settling;
        m_Attempts = 0;

        Log(fmt::format(
            "Step 1: piece #{} moved under his feet at z={:.2f}, {}. He is currently standing on {}.", s_UnderId,
            s_Height, s_How, s_Standing
        ));

        return;
    }

    if (m_Stage == Stage::Idle) {
        return;
    }

    std::scoped_lock s_Lock(m_DataMutex);

    // Another go. Either he went up, in which case we are done, or the piece is fetched back to
    // where it started and sent again.
    if (m_Stage == Stage::Retrying) {
        if (m_LastFeet.z - m_LaunchBaseZ > 0.5f) {
            Log(fmt::format("Carried him up {:.2f} m on send {} ({} tries to spare).", m_LastFeet.z - m_LaunchBaseZ,
                m_Sends, m_RetriesLeft));

            m_Stage = Stage::Idle;
            m_LaunchedPiece = nullptr;
            return;
        }

        if (!m_LaunchedPiece || !LooksLikeGameObject(m_LaunchedPiece)) {
            m_Stage = Stage::Idle;
            m_LaunchedPiece = nullptr;
            return;
        }

        auto* s_Piece = static_cast<IPhysicsObject*>(m_LaunchedPiece);
        const float4 s_At = s_Piece->GetPosition();

        if (m_RetryPutBack) {
            if (m_MeasuringTools && m_ToolThirdFactor && m_LastTouchOffset >= 0.f) {
                Log(fmt::format(
                    "THIRD FACTOR: no lift after send {} - PhysX had touched the piece {:.3f} m from his axis, the "
                    "set-down sphere reaches {:.3f} ({}).", m_Sends, m_LastTouchOffset, m_LastSphereRadius,
                    m_LastTouchOffset <= m_LastSphereRadius ? "inside" : "OUTSIDE"
                ));
                m_LastTouchOffset = -1.f;
            }

            if (m_RetriesLeft <= 0) {
                Log(fmt::format("Sent it {} times and he never went up.", m_Sends));
                m_Stage = Stage::Idle;
                m_LaunchedPiece = nullptr;
                return;
            }

            MovePiece(m_LaunchedPiece, float4(m_LastFeet.x + m_StageDX, m_LastFeet.y + m_StageDY, m_PieceHomeZ, s_At.w));
            m_RetryPutBack = false;
            return;
        }

        if (!TrySend() && m_WaitFrames > m_MaxWaitFrames) {
            Log(fmt::format(
                "Waited {} frames and the moment never came: PhysX {} the piece{}. Gave up after {} sends.",
                m_WaitFrames, PhysXHolds(m_LaunchedPiece) ? "held" : "did not hold",
                m_Px.Step == m_Px.RideStep ? ", and the step never moved on" : "", m_Sends
            ));

            m_Stage = Stage::Idle;
            m_LaunchedPiece = nullptr;
        }

        return;
    }

    // Raising the piece a little at a time until it is flush with his feet, then launching it from
    // there. Doing it gradually rather than in one jump keeps it from being shoved aside: the engine
    // settles collisions in his favour, and a piece driven into him is a piece that goes sideways.
    if (m_Stage == Stage::Tightening) {
        if (m_StagedPiece && LooksLikeGameObject(m_StagedPiece) && PhysXHolds(m_StagedPiece)) {
            void* s_Piece = m_StagedPiece;
            const float4 s_At = static_cast<IPhysicsObject*>(s_Piece)->GetPosition();
            m_StagedPiece = nullptr;

            FireLaunch(s_Piece, s_At.z, fmt::format(
                "PhysX holds the piece after {} frames of closing the gap. Launched it from z={:.2f}.",
                m_GapFrames, s_At.z
            ));

            return;
        }

        if (!m_StagedPiece || !LooksLikeGameObject(m_StagedPiece) || m_SupportBody != m_StagedPiece) {
            Log("Lost the footing while closing the gap.");
            m_Stage = Stage::Idle;
            m_StagedPiece = nullptr;
            return;
        }

        auto* s_Footing = static_cast<IPhysicsObject*>(m_StagedPiece);
        const float4 s_At = s_Footing->GetPosition();
        const float s_Gap = -m_SupportDrop;

        if (s_Gap <= m_GapAllowed) {
            m_StagedPiece = nullptr;

            FireLaunch(m_SupportBody, s_At.z, fmt::format(
                "Gap closed to {:.3f} m in {} frames. Launched piece #{} from z={:.2f}.", s_Gap, m_GapFrames,
                m_SupportId, s_At.z
            ));

            return;
        }

        if (++m_GapFrames > m_GapMaxFrames) {
            Log(fmt::format("Gave up closing the gap: still {:.3f} m after {} frames.", s_Gap, m_GapMaxFrames));
            m_Stage = Stage::Idle;
            m_StagedPiece = nullptr;
            return;
        }

        MovePiece(m_StagedPiece, float4(s_At.x, s_At.y, s_At.z + std::min(s_Gap, m_GapStep), s_At.w));

        return;
    }

    if (!m_StagedPiece || !LooksLikeGameObject(m_StagedPiece)) {
        Log("The piece was gone before it could be launched.");
        m_Stage = Stage::Idle;
        return;
    }

    auto* s_Body = static_cast<IPhysicsObject*>(m_StagedPiece);

    // Standing on it is read, not deduced: the ray from his feet names the body he is resting on.
    // One frame of it is enough, and waiting longer was measured to let the moment pass.
    // Only PhysX says: the ray from his feet called pieces his footing that PhysX never held.
    const bool s_Up = PhysXHolds(m_StagedPiece);

    if (m_Stage == Stage::Confirming) {
        if (!s_Up) {
            m_Stage = Stage::Settling;
            return;
        }
    }
    else if (s_Up) {
        m_Stage = Stage::Confirming;
        return;
    }

    if (m_Stage == Stage::Settling) {
        if (++m_Attempts > m_MaxAttempts) {
            const float4 s_At = s_Body->GetPosition();
            const bool s_Sank = s_At.z - m_LastFeet.z < -0.1f;

            m_StageSank[m_StageByBox ? 1 : 0] += s_Sank ? 1 : 0;

            Log(fmt::format(
                "Gave up after {} tries ({}): the piece sits at {:+.2f} m from his feet{}, and he is standing on {} "
                "({:.2f} m under them).", m_MaxAttempts, m_StageByBox ? "placed by its box" : "origin at his feet",
                s_At.z - m_LastFeet.z, s_Sank ? " - THROUGH THE FLOOR" : "",
                m_SupportId > 0 ? fmt::format("piece #{}", m_SupportId) : m_SupportName, -m_SupportDrop
            ));

            m_Stage = Stage::Idle;
            m_StagedPiece = nullptr;
            return;
        }

        // The piece is kept under him while he comes down: he drifts as he falls, and a piece left
        // behind is a piece he lands beside instead of on.
        const float4 s_At = s_Body->GetPosition();
        MovePiece(m_StagedPiece, float4(m_LastFeet.x + m_StageDX, m_LastFeet.y + m_StageDY, s_At.z, s_At.w));

        return;
    }

    // Through the same gate as every other launch: it goes the moment the carry can happen.
    const float4 s_Now = s_Body->GetPosition();
    void* s_Piece = m_StagedPiece;
    m_StagedPiece = nullptr;
    ++m_StageHeld[m_StageByBox ? 1 : 0];

    FireLaunch(s_Piece, s_Now.z, fmt::format(
        "Step 2: piece #{} is his footing after {} nudges ({}). Launching by {:.2f} m from z={:.2f}.",
        m_Px.TouchedPiece, m_Attempts, m_StageByBox ? "placed by its box" : "origin at his feet", m_LiftAmount,
        s_Now.z
    ));
}

// Rays around 47's feet; the physics body in each result is checked against the known vtables, and
// the ones that are really bodies get followed.
void DebrisTracer::Probe(bool p_Verbose) {
    m_LastProbe = m_Time;

    auto s_Hitman = SDK()->GetLocalPlayer();
    const auto* s_Spatial = s_Hitman ? s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

    if (!s_Spatial || !Globals::CollisionManager || !*Globals::CollisionManager) {
        if (p_Verbose) {
            Log("Probe: no hitman or no collision manager.");
        }

        return;
    }

    const SVector3 s_Feet = ToVec3(s_Spatial->GetObjectToWorldMatrix().Trans);

    // A ray fired downwards from above 47 is stopped by 47 himself at zero distance, so it can never
    // see what lies directly under his feet - which is the one piece that matters. The close-in rings
    // are therefore fired upwards from below his feet, where nothing of his is in the path. Only
    // movable bodies are asked for, so the floor cannot get in the way.
    struct Segment {
        SVector3 From;
        SVector3 To;
        float Radius;
        bool Upward;
    };

    // A ring only samples the points it passes through, and a violin breaks into far more pieces
    // than that. Turning the rings a little on every probe sweeps the ground between them.
    const float s_Phase = m_Time * 1.7f;
    constexpr int kSpokes = 12;

    std::vector<Segment> s_Segments;

    const auto s_AddRing = [&](float p_Radius, bool p_Upward) {
        const int s_Spokes = p_Radius <= 0.f ? 1 : kSpokes;

        for (int i = 0; i < s_Spokes; ++i) {
            const float s_Angle = s_Phase + 6.2831853f * static_cast<float>(i) / static_cast<float>(kSpokes);
            const float s_X = s_Feet.x + std::cos(s_Angle) * p_Radius;
            const float s_Y = s_Feet.y + std::sin(s_Angle) * p_Radius;

            if (p_Upward) {
                s_Segments.push_back({
                    SVector3(s_X, s_Y, s_Feet.z - 0.45f), SVector3(s_X, s_Y, s_Feet.z + 0.3f), p_Radius, true
                });
            }
            else {
                s_Segments.push_back({
                    SVector3(s_X, s_Y, s_Feet.z + 0.9f), SVector3(s_X, s_Y, s_Feet.z - 0.6f), p_Radius, false
                });
            }
        }
    };

    for (const float s_Radius : { 0.f, 0.12f, 0.25f, 0.4f, 0.55f }) {
        s_AddRing(s_Radius, true);
    }

    for (const float s_Radius : { 0.3f, 0.45f, 0.6f, 0.8f, 1.f, m_ProbeReach }) {
        s_AddRing(s_Radius, false);
    }

    int s_Found = 0, s_Hits = 0, s_Rays = 0;

    for (const auto& s_Segment : s_Segments) {
        ZRayQueryInput s_Input {
            .m_vFrom = float4(s_Segment.From.x, s_Segment.From.y, s_Segment.From.z, 1.f),
            .m_vTo = float4(s_Segment.To.x, s_Segment.To.y, s_Segment.To.z, 1.f),
            .m_eType = ECollidablesType::ECST_DYNAMIC_ONLY,
        };
        ZRayQueryOutput s_Out {};

        ++s_Rays;

        if (!(*Globals::CollisionManager)->RayCastClosestHit(s_Input, &s_Out)) {
            continue;
        }

        ++s_Hits;

        // The result holds a proxy, not the body: an opaque wrapper whose first word is the physics
        // object itself.
        auto* s_Proxy = s_Out.m_pBlockingPhysicsObject.m_pProxy;
        void* s_Body = nullptr;

        if (Readable(s_Proxy, sizeof(void*))) {
            auto* s_Candidate = *reinterpret_cast<void* const*>(s_Proxy);

            if (LooksLikeGameObject(s_Candidate)) {
                s_Body = s_Candidate;
            }
        }

        if (!s_Body) {
            continue;
        }

        std::scoped_lock s_Lock(m_DataMutex);

        const bool s_Known = std::any_of(m_Tracked.begin(), m_Tracked.end(),
            [s_Body](const Tracked& p_Item) { return p_Item.Object == s_Body; });

        if (s_Known) {
            continue;
        }

        const SVector3 s_Position = ToVec3(static_cast<IPhysicsObject*>(s_Body)->GetPosition());

        // When the list is full, the piece to drop is the one furthest from 47: a violin leaves far
        // more pieces around than we can follow, and the ones beside him are the ones that can carry
        // him. Refusing newcomers outright kept the nearest pieces off the list.
        if (static_cast<int>(m_Tracked.size()) >= kMaxTracked) {
            const float s_NewSide = std::sqrt(
                (s_Position.x - s_Feet.x) * (s_Position.x - s_Feet.x) +
                (s_Position.y - s_Feet.y) * (s_Position.y - s_Feet.y)
            );

            auto s_Furthest = m_Tracked.end();
            float s_FurthestSide = s_NewSide;

            for (auto it = m_Tracked.begin(); it != m_Tracked.end(); ++it) {
                if (it->Path.empty()) {
                    continue;
                }

                const SVector3 s_At = it->Path.back();
                const float s_Side = std::sqrt(
                    (s_At.x - s_Feet.x) * (s_At.x - s_Feet.x) + (s_At.y - s_Feet.y) * (s_At.y - s_Feet.y)
                );

                if (s_Side > s_FurthestSide) {
                    s_FurthestSide = s_Side;
                    s_Furthest = it;
                }
            }

            if (s_Furthest == m_Tracked.end()) {
                continue;
            }

            m_Tracked.erase(s_Furthest);
        }

        Tracked s_Item;
        s_Item.Id = m_NextId++;
        s_Item.Object = s_Body;
        ReadVTable(s_Body, s_Item.VTable);
        s_Item.TypeName = DescribePointer(s_Body);
        s_Item.Start = s_Position;
        s_Item.LastSide = std::sqrt(
            (s_Position.x - s_Feet.x) * (s_Position.x - s_Feet.x) +
            (s_Position.y - s_Feet.y) * (s_Position.y - s_Feet.y)
        );
        s_Item.Path.push_back(s_Position);

        m_Tracked.push_back(std::move(s_Item));
        m_TrackedCount = static_cast<int>(m_Tracked.size());
        ++s_Found;

        Log(fmt::format(
            "Piece #{} found with a ray {} at {:.2f} m: {} at ({:.2f}, {:.2f}, {:.2f}), {:+.2f} m from the feet.",
            m_Tracked.back().Id, s_Segment.Upward ? "upwards" : "downwards", s_Segment.Radius,
            m_Tracked.back().TypeName, s_Position.x, s_Position.y, s_Position.z, s_Position.z - s_Feet.z
        ));
    }

    if (p_Verbose) {
        Log(fmt::format("Probe: {} rays, {} hit something, {} new bodies.", s_Rays, s_Hits, s_Found));
    }
}

// Follows every piece on the list, and notes which one is under his feet - the piece staging puts
// under him when he is on the floor.
void DebrisTracer::UpdateTracked() {
    std::scoped_lock s_Lock(m_DataMutex);

    auto s_Hitman = SDK()->GetLocalPlayer();
    const auto* s_Spatial = s_Hitman ? s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>() : nullptr;

    if (!s_Spatial) {
        return;
    }

    const SVector3 s_Feet = ToVec3(s_Spatial->GetObjectToWorldMatrix().Trans);

    // Where 47 is, every frame, whatever else is switched off: everything else reads it.
    m_LastFeet = s_Feet;

    // His path, only while "Track 47" is on.
    if (m_TraceHero) {
        const SVector3 s_Last = m_HeroPath.empty() ? SVector3(1e9f, 1e9f, 1e9f) : m_HeroPath.back();
        const float s_Hx = s_Feet.x - s_Last.x, s_Hy = s_Feet.y - s_Last.y, s_Hz = s_Feet.z - s_Last.z;

        if (m_HeroPath.empty() || std::sqrt(s_Hx * s_Hx + s_Hy * s_Hy + s_Hz * s_Hz) > 0.02f) {
            m_HeroPath.push_back(s_Feet);

            while (m_HeroPath.size() > 600) {
                m_HeroPath.erase(m_HeroPath.begin());
            }
        }
    }

    if (!m_FollowPieces) {
        m_HasUnder = false;
        m_UnderObject = nullptr;
        m_TrackedCount = static_cast<int>(m_Tracked.size());
        return;
    }

    float s_BestDist = 1e9f;
    void* s_UnderObject = nullptr;
    int s_Measured = 0;
    int s_BoxReads = 0;

    for (auto& s_Item : m_Tracked) {
        if (!s_Item.Alive) {
            continue;
        }

        // The body can be destroyed at any time; once it stops looking like an object, stop touching it.
        uintptr_t s_NowVTable = 0;

        if (!ReadVTable(s_Item.Object, s_NowVTable) || s_NowVTable != s_Item.VTable) {
            s_Item.Alive = false;
            Log(fmt::format("A tracked {} is gone.", s_Item.TypeName));
            continue;
        }

        const SVector3 s_Position = ToVec3(static_cast<IPhysicsObject*>(s_Item.Object)->GetPosition());

        // The real box, for pieces near him, read again every second and a few pieces a frame at most:
        // each read takes the scene's lock.
        if (m_ShowRealBoxes && s_BoxReads < 4 && m_Time - s_Item.BoxReadAt > 1.f) {
            const float s_Bx = s_Position.x - s_Feet.x, s_By = s_Position.y - s_Feet.y, s_Bz = s_Position.z - s_Feet.z;

            if (s_Bx * s_Bx + s_By * s_By < 36.f && std::abs(s_Bz) < 6.f) {
                PieceBox s_Box;
                std::string s_Why;

                s_Item.BoxReadAt = m_Time;
                ++s_BoxReads;
                s_Item.HasBox = ReadPieceBox(s_Item.Object, s_Box, s_Why);

                if (s_Item.HasBox) {
                    s_Item.BoxMin = SVector3(s_Box.MinX - s_Position.x, s_Box.MinY - s_Position.y, s_Box.MinZ - s_Position.z);
                    s_Item.BoxMax = SVector3(s_Box.MaxX - s_Position.x, s_Box.MaxY - s_Position.y, s_Box.MaxZ - s_Position.z);
                }
            }
        }

        if (s_Item.Path.empty()) {
            s_Item.Path.push_back(s_Position);
            continue;
        }

        ++s_Measured;

        const SVector3 s_Last = s_Item.Path.back();
        const float s_Dx = s_Position.x - s_Last.x, s_Dy = s_Position.y - s_Last.y, s_Dz = s_Position.z - s_Last.z;
        const float s_Step = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz);

        const float s_SideX = s_Position.x - s_Feet.x, s_SideY = s_Position.y - s_Feet.y;
        const float s_Side = std::sqrt(s_SideX * s_SideX + s_SideY * s_SideY);
        const float s_Height = s_Position.z - s_Feet.z;

        // "Under him" is judged on where the piece was before this frame's move, not after: a piece
        // that has just been launched is already far away by the time we look at it.
        const float s_Was = s_Item.LastSide;
        s_Item.LastSide = s_Side;

        const bool s_IsUnder = s_Was < m_UnderRadius && s_Height > -m_UnderBelow && s_Height < m_UnderAbove;

        if (s_IsUnder && s_Was < s_BestDist) {
            s_BestDist = s_Was;
            s_UnderObject = s_Item.Object;
            m_UnderDist = s_Was;
            m_UnderDz = s_Height;
        }

        // A rocket: written once, the frame it goes.
        if (m_MeasuringTools && m_ToolRockets && s_Step > m_RocketStep && !s_Item.Rocketed) {
            s_Item.Rocketed = true;
            ++m_RocketCount;

            const float s_WasX = s_Last.x - s_Feet.x, s_WasY = s_Last.y - s_Feet.y;

            // What PhysX had him on in the frame before - read after tracking, so it is the moment
            // before the piece went.
            const std::string s_Held = !m_Px.Controller ? "PhysX not read"
                : m_Px.TouchedFragment == s_Item.Object ? "PhysX HAD 47 ON THIS PIECE"
                : fmt::format("PhysX had 47 on {}", m_Px.TouchedFragment ? BoostName(m_Px.TouchedFragment)
                    : m_Px.TouchedType == 7 ? "something static" : m_Px.TouchedActor ? "another body" : "nothing");

            Log(fmt::format(
                "ROCKET #{} ({}, {}): {:.1f} m in one frame ({:+.1f} up), from z={:.2f}, where it was {:.2f} m from 47 "
                "sideways and {:+.2f} from his feet; {}; {}.", s_Item.Id, PieceFacts(s_Item.Object), EscapeFacts(s_Item.Object),
                s_Step, s_Dz, s_Last.z, std::sqrt(s_WasX * s_WasX + s_WasY * s_WasY), s_Last.z - s_Feet.z, s_Held,
                OwnerFacts(s_Item.Object)
            ));

        }

        if (s_Step > 0.002f) {
            s_Item.Path.push_back(s_Position);

            while (s_Item.Path.size() > 600) {
                s_Item.Path.erase(s_Item.Path.begin());
            }
        }
    }

    m_HasUnder = s_UnderObject != nullptr;
    m_UnderObject = s_UnderObject;
    m_TrackedCount = s_Measured;
    m_LastFeet = s_Feet;
}

// Every fragment in the scene, as (actor, object) pairs. The scene must be the NpScene Ghidra found,
// its methods inside PhysX, and every body a fragment by its exact vtable.
bool DebrisTracer::ListFragments(std::vector<std::pair<uintptr_t, void*>>& p_Out, std::string& p_Why) {
    p_Out.clear();

    if (!m_PhysXBase && !ModuleRange(kPhysXModuleName, m_PhysXBase, m_PhysXEnd)) {
        p_Why = "PhysX is not loaded";
        return false;
    }

    const auto s_InPhysX = [this](uintptr_t p_Address) {
        return p_Address >= m_PhysXBase && p_Address < m_PhysXEnd;
    };

    uintptr_t s_Physics = 0, s_PhysicsVTable = 0, s_Scene = 0, s_SceneVTable = 0;
    uintptr_t s_Lock = 0, s_Unlock = 0, s_Count = 0, s_List = 0;

    if (!ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) || !ReadRaw(s_Physics, s_PhysicsVTable) ||
        s_PhysicsVTable - m_ModuleDelta != kPhysicsManagerVTable) {
        p_Why = "no physics manager";
        return false;
    }

    if (!ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) || !ReadRaw(s_Scene, s_SceneVTable) ||
        s_SceneVTable - m_PhysXBase + kPhysXImageBase != kNpSceneVTable ||
        !ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) ||
        !ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) ||
        !ReadRaw(s_SceneVTable + kGetNbActorsSlot * sizeof(void*), s_Count) ||
        !ReadRaw(s_SceneVTable + kGetActorsSlot * sizeof(void*), s_List) || !s_InPhysX(s_Lock) ||
        !s_InPhysX(s_Unlock) || !s_InPhysX(s_Count) || !s_InPhysX(s_List)) {
        p_Why = "the scene is not the one Ghidra found";
        return false;
    }

    // First how many there are, then all of them: a fixed buffer of 8192 missed every piece of a violin
    // broken during the game - they are added at the end of the scene's list, past the first 16000.
    const auto s_LockFn = reinterpret_cast<SceneLockFn>(s_Lock);
    const auto s_UnlockFn = reinterpret_cast<SceneUnlockFn>(s_Unlock);
    const auto s_CountFn = reinterpret_cast<GetNbActorsFn>(s_Count);
    const auto s_ListFn = reinterpret_cast<GetActorsFn>(s_List);
    uint32_t s_Total = 0, s_Got = 0;

    if (!CallListActors(s_Scene, s_LockFn, s_UnlockFn, s_CountFn, s_ListFn, nullptr, 0, s_Total, s_Got) ||
        s_Total > kMaxSceneBodies) {
        p_Why = fmt::format("could not count the bodies ({})", s_Total);
        return false;
    }

    // Room for a few more, in case some are added between the two calls.
    m_ActorBuffer.resize(s_Total + 1024);
    auto& s_Actors = m_ActorBuffer;

    if (!CallListActors(s_Scene, s_LockFn, s_UnlockFn, s_CountFn, s_ListFn, s_Actors.data(),
        static_cast<uint32_t>(s_Actors.size()), s_Total, s_Got)) {
        p_Why = "the call failed";
        return false;
    }

    m_SceneBodies = static_cast<int>(s_Total);

    for (uint32_t i = 0; i < s_Got && i < s_Actors.size(); ++i) {
        uint16_t s_Type = 0;

        if (!ReadRaw(s_Actors[i] + kConcreteTypeOffset, s_Type) || s_Type != 6) {
            continue;
        }

        if (void* s_Object = FragmentFromActor(s_Actors[i])) {
            p_Out.emplace_back(s_Actors[i], s_Object);
        }
    }

    return true;
}

// What the level holds when it starts: every fragment and where it is. Taken once per level, a few
// seconds in, with the mod on.
void DebrisTracer::TakeBaseline() {
    std::vector<std::pair<uintptr_t, void*>> s_Fragments;
    std::string s_Why;

    m_HaveBaseline = true;

    if (!ListFragments(s_Fragments, s_Why)) {
        Log("Baseline: " + s_Why + ".");
        return;
    }

    std::scoped_lock s_Lock(m_DataMutex);
    m_Baseline.clear();

    for (const auto& [s_Actor, s_Object] : s_Fragments) {
        m_Baseline[s_Actor] = ToVec3(static_cast<IPhysicsObject*>(s_Object)->GetPosition());
    }

    Log(fmt::format(
        "Baseline: {} fragments already in the level ({} moving bodies); only new or moved ones are debris.",
        m_Baseline.size(), m_SceneBodies
    ));
}

// Debris: a fragment that was not in the level at the start, or has moved from where it was - a prop
// that broke, or a body the game took from its pool for the pieces of something that broke.
bool DebrisTracer::IsDebris(uintptr_t p_Actor, const SVector3& p_Position) const {
    const auto s_It = m_Baseline.find(p_Actor);

    if (s_It == m_Baseline.end()) {
        return true;
    }

    const float s_Dx = p_Position.x - s_It->second.x, s_Dy = p_Position.y - s_It->second.y;
    const float s_Dz = p_Position.z - s_It->second.z;

    return s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz > 0.05f * 0.05f;
}

// New fragments within reach of 47 go on the list; ones sent under the map never do. Pieces already
// on it are followed wherever they go.
void DebrisTracer::ScanScene() {
    m_LastScan = m_Time;

    std::vector<std::pair<uintptr_t, void*>> s_Fragments;
    std::string s_Why;

    if (!ListFragments(s_Fragments, s_Why)) {
        LogFile("Scan: " + s_Why);
        return;
    }

    std::scoped_lock s_Lock(m_DataMutex);

    // Gone pieces off the list first, so they do not hold places.
    std::erase_if(m_Tracked, [](const Tracked& p_Item) { return !p_Item.Alive; });

    int s_New = 0;

    for (const auto& [s_Actor, s_Object] : s_Fragments) {
        if (static_cast<int>(m_Tracked.size()) >= kMaxTracked) {
            break;
        }

        const auto s_WasRemoved = std::find(m_Removed.begin(), m_Removed.end(), s_Actor);

        // A removed piece back above ground is the game using that body again - the pieces come from
        // a pool. It gets its gravity back and counts as a new piece.
        if (s_WasRemoved != m_Removed.end()) {
            if (static_cast<IPhysicsObject*>(s_Object)->GetPosition().z < m_LastFeet.z - kGraveDepth * 0.5f) {
                continue;
            }

            m_Removed.erase(s_WasRemoved);
            SetGravity(s_Actor, true);
            Log("A removed fragment is back in use by the game: its gravity is back on.");
        }

        if (std::any_of(m_Tracked.begin(), m_Tracked.end(), [s_Object](const Tracked& p_Item) {
                return p_Item.Object == s_Object;
            })) {
            continue;
        }

        const SVector3 s_Position = ToVec3(static_cast<IPhysicsObject*>(s_Object)->GetPosition());
        const float s_Dx = s_Position.x - m_LastFeet.x, s_Dy = s_Position.y - m_LastFeet.y;
        const float s_Dz = s_Position.z - m_LastFeet.z;

        if (std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz) > m_TrackRadius || !IsDebris(s_Actor, s_Position)) {
            continue;
        }

        Tracked s_Item;
        s_Item.Id = m_NextId++;
        s_Item.Object = s_Object;
        ReadVTable(s_Object, s_Item.VTable);
        s_Item.TypeName = "ZDestructiblePhysicsObject";
        s_Item.Start = s_Position;
        s_Item.LastSide = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy);
        s_Item.Path.push_back(s_Position);
        m_Tracked.push_back(std::move(s_Item));
        ++s_New;
    }

    m_TrackedCount = static_cast<int>(m_Tracked.size());

    if (s_New > 0) {
        Log(fmt::format(
            "Scan: {} new fragments within {:.0f} m ({} fragments and {} moving bodies in the scene).", s_New,
            m_TrackRadius, s_Fragments.size(), m_SceneBodies
        ));
    }
}

// Every fragment within reach of 47, and every one on the list, goes far under the map with its
// gravity off, and is never tracked again.
void DebrisTracer::RemoveAllFragments() {
    if (!m_HaveBaseline) {
        Log("Remove: not ready yet - the level's own fragments are noted a few seconds after it starts.");
        return;
    }

    std::vector<std::pair<uintptr_t, void*>> s_Fragments;
    std::string s_Why;

    if (!ListFragments(s_Fragments, s_Why)) {
        Log("Remove: " + s_Why + ".");
        return;
    }

    std::scoped_lock s_DataLock(m_DataMutex);

    std::vector<void*> s_OnList;

    for (const auto& s_Item : m_Tracked) {
        if (s_Item.Alive) {
            s_OnList.push_back(s_Item.Object);
        }
    }

    int s_Moved = 0, s_Floating = 0;

    for (const auto& [s_Actor, s_Object] : s_Fragments) {
        if (std::find(m_Removed.begin(), m_Removed.end(), s_Actor) != m_Removed.end()) {
            continue;
        }

        const float4 s_At = static_cast<IPhysicsObject*>(s_Object)->GetPosition();
        const float s_Dx = s_At.x - m_LastFeet.x, s_Dy = s_At.y - m_LastFeet.y, s_Dz = s_At.z - m_LastFeet.z;
        const bool s_Near = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz) <= m_TrackRadius &&
            IsDebris(s_Actor, ToVec3(s_At));

        if (!s_Near && std::find(s_OnList.begin(), s_OnList.end(), s_Object) == s_OnList.end()) {
            continue;
        }

        static_cast<IPhysicsObject*>(s_Object)->SetPosition(float4(s_At.x, s_At.y, s_At.z - kGraveDepth, s_At.w));
        ++s_Moved;

        s_Floating += SetGravity(s_Actor, false) ? 1 : 0;

        m_Removed.push_back(s_Actor);
    }

    m_Tracked.clear();
    m_TrackedCount = 0;

    Log(fmt::format(
        "Removed {} fragments: sent {:.0f} m under the map, gravity off for {}. Tracking starts afresh.", s_Moved,
        kGraveDepth, s_Floating
    ));
}

// Gravity off for a removed piece, so it stays where it was sent, or back on for one the game uses
// again. Under the scene's write lock; the method must lie inside PhysX.
bool DebrisTracer::SetGravity(uintptr_t p_Actor, bool p_On) {
    uintptr_t s_Physics = 0, s_Scene = 0, s_SceneVTable = 0, s_Lock = 0, s_Unlock = 0;
    uintptr_t s_ActorVTable = 0, s_SetFlag = 0;

    if (!ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) ||
        !ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) || !ReadRaw(s_Scene, s_SceneVTable) ||
        !ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) ||
        !ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) || !ReadRaw(p_Actor, s_ActorVTable) ||
        !ReadRaw(s_ActorVTable + kSetActorFlagSlot * sizeof(void*), s_SetFlag)) {
        return false;
    }

    const auto s_InPhysX = [this](uintptr_t p_Address) {
        return p_Address >= m_PhysXBase && p_Address < m_PhysXEnd;
    };

    if (!s_InPhysX(s_Lock) || !s_InPhysX(s_Unlock) || !s_InPhysX(s_SetFlag)) {
        return false;
    }

    // The flag turns gravity OFF: set it to remove, clear it to give gravity back.
    return CallSetActorFlag(s_Scene, reinterpret_cast<SceneLockFn>(s_Lock), reinterpret_cast<SceneUnlockFn>(s_Unlock),
        reinterpret_cast<SetActorFlagFn>(s_SetFlag), p_Actor, kDisableGravityFlag, !p_On);
}

// The addresses the escape-speed calls need, each checked to be inside PhysX.
bool DebrisTracer::EscapeAccess(void* p_Piece, uintptr_t& p_Actor, uintptr_t& p_Scene, uintptr_t& p_Lock,
    uintptr_t& p_Unlock, uintptr_t& p_Get, uintptr_t& p_Set, uintptr_t& p_Flags) const {
    uintptr_t s_ActorVTable = 0, s_Physics = 0, s_SceneVTable = 0;
    uint16_t s_Type = 0;

    const auto s_InPhysX = [this](uintptr_t p_Address) {
        return p_Address >= m_PhysXBase && p_Address < m_PhysXEnd;
    };

    return p_Piece && ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kActorOffset, p_Actor) &&
        ReadRaw(p_Actor + kConcreteTypeOffset, s_Type) && s_Type == 6 && ReadRaw(p_Actor, s_ActorVTable) &&
        ReadRaw(s_ActorVTable + kGetMaxDepenOffset, p_Get) && ReadRaw(s_ActorVTable + kSetMaxDepenOffset, p_Set) &&
        ReadRaw(s_ActorVTable + kGetBodyFlagsOffset, p_Flags) && ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) &&
        ReadRaw(s_Physics + kPhysicsSceneOffset, p_Scene) && ReadRaw(p_Scene, s_SceneVTable) &&
        ReadRaw(s_SceneVTable + kSceneLockWriteOffset, p_Lock) && ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, p_Unlock) &&
        s_InPhysX(p_Get) && s_InPhysX(p_Set) && s_InPhysX(p_Flags) && s_InPhysX(p_Lock) && s_InPhysX(p_Unlock);
}

// "escape max X m/s, CCD off" for one piece.
std::string DebrisTracer::EscapeFacts(void* p_Piece) const {
    uintptr_t s_Actor = 0, s_Scene = 0, s_Lock = 0, s_Unlock = 0, s_Get = 0, s_Set = 0, s_Flags = 0;
    float s_Speed = 0.f;
    uint8_t s_BodyFlags = 0;

    if (!EscapeAccess(p_Piece, s_Actor, s_Scene, s_Lock, s_Unlock, s_Get, s_Set, s_Flags) ||
        !CallEscapeRead(s_Scene, reinterpret_cast<SceneLockFn>(s_Lock), reinterpret_cast<SceneUnlockFn>(s_Unlock),
            reinterpret_cast<GetMaxDepenFn>(s_Get), reinterpret_cast<GetBodyFlagsFn>(s_Flags), s_Actor, s_Speed,
            s_BodyFlags)) {
        return "escape speed unreadable";
    }

    const std::string s_Limit = s_Speed > 1e20f ? "unlimited" : fmt::format("{:.1f} m/s", s_Speed);

    return fmt::format(
        "escape max {}, CCD {}{}", s_Limit, (s_BodyFlags & kBodyFlagCcd) ? "on" : "off",
        (s_BodyFlags & kBodyFlagSpeculativeCcd) ? " (speculative on)" : ""
    );
}

// Who the piece belongs to. Read-only walk of the same tables the game's FUN_140efa8a0 walks, every
// pointer checked by its vtable before it is followed.
DebrisTracer::OwnerInfo DebrisTracer::ReadOwner(void* p_Piece) const {
    OwnerInfo s_Info;
    const auto s_Object = reinterpret_cast<uintptr_t>(p_Piece);

    if (!ReadRaw(s_Object + kObjectOwnerOffset, s_Info.Owner) || s_Info.Owner < 0x10000) {
        s_Info.Owner = 0;
        s_Info.Why = "no owner";
        return s_Info;
    }

    // The owner's interfaces: (type id, offset) pairs, in an array that is either out of line
    // (begin, end) or inline with its count in the low byte of the third word.
    uintptr_t s_Type = 0, s_Table = 0;
    uint64_t s_Flags = 0;

    if (!ReadRaw(s_Info.Owner, s_Type) || !ReadRaw(s_Type + kEntityInterfacesOffset, s_Table) || s_Table < 0x10000 ||
        !ReadRaw(s_Table + 0x10, s_Flags)) {
        s_Info.Why = "its interfaces could not be read";
        return s_Info;
    }

    uintptr_t s_Begin = s_Table;
    size_t s_Count = s_Flags & 0xff;

    if (((s_Flags >> 62) & 1) == 0) {
        uintptr_t s_End = 0;

        if (!ReadRaw(s_Table, s_Begin) || !ReadRaw(s_Table + 8, s_End) || s_End < s_Begin) {
            s_Info.Why = "its interfaces could not be read";
            return s_Info;
        }

        s_Count = (s_End - s_Begin) / 16;
    }

    uintptr_t s_Access = 0;

    for (size_t i = 0; i < s_Count && i < 64; ++i) {
        uintptr_t s_Id = 0, s_Offset = 0;

        if (ReadRaw(s_Begin + 16 * i, s_Id) && s_Id - m_ModuleDelta == kPhysicsAccessTypeId &&
            ReadRaw(s_Begin + 16 * i + 8, s_Offset)) {
            s_Access = s_Info.Owner + s_Offset;
            break;
        }
    }

    uintptr_t s_AccessVTable = 0;

    if (!s_Access || !ReadRaw(s_Access, s_AccessVTable)) {
        s_Info.Why = "no physics access interface";
        return s_Info;
    }

    if (s_AccessVTable - m_ModuleDelta != kG2AccessVTable) {
        s_Info.Why = fmt::format("not a destructible (access vtable ghidra 0x{:x})", s_AccessVTable - m_ModuleDelta);
        return s_Info;
    }

    s_Info.Destructible = true;
    uintptr_t s_System = 0, s_Holder = 0, s_HolderVTable = 0;
    uintptr_t s_PiecesBegin = 0, s_PiecesEnd = 0;
    uint64_t s_PiecesFlags = 0;

    if (!ReadRaw(s_Access + kAccessSystemOffset, s_System) || !ReadRaw(s_System + kSystemPiecesOffset, s_PiecesBegin) ||
        !ReadRaw(s_System + kSystemPiecesOffset + 8, s_PiecesEnd) ||
        !ReadRaw(s_System + kSystemPiecesOffset + 0x10, s_PiecesFlags)) {
        s_Info.Why = "a destructible whose pieces could not be read";
        return s_Info;
    }

    s_Info.Pieces = ((s_PiecesFlags >> 62) & 1) ? (s_PiecesFlags & 0xff)
        : s_PiecesEnd >= s_PiecesBegin ? (s_PiecesEnd - s_PiecesBegin) / 8 : 0;

    if (ReadRaw(s_System + kSystemFirstPieceOffset, s_Holder) && ReadRaw(s_Holder, s_HolderVTable) &&
        s_HolderVTable - m_ModuleDelta == kPieceHolderVTable) {
        s_Info.First = reinterpret_cast<void*>(s_Holder - kHolderToObject);
    }

    return s_Info;
}

std::string DebrisTracer::OwnerFacts(void* p_Piece) {
    const OwnerInfo s_Info = ReadOwner(p_Piece);

    if (!s_Info.Owner) {
        return s_Info.Why;
    }

    // How many of the tracked pieces share this owner.
    int s_Siblings = 0;

    for (const auto& s_Item : m_Tracked) {
        uintptr_t s_Other = 0;

        if (s_Item.Alive && ReadRaw(reinterpret_cast<uintptr_t>(s_Item.Object) + kObjectOwnerOffset, s_Other) &&
            s_Other == s_Info.Owner) {
            ++s_Siblings;
        }
    }

    const std::string s_Head = fmt::format("owner 0x{:x} ({} tracked pieces share it)", s_Info.Owner, s_Siblings);

    if (!s_Info.Destructible || !s_Info.Why.empty()) {
        return s_Head + ", " + s_Info.Why;
    }

    const std::string s_First = !s_Info.First ? "its first piece could not be read"
        : s_Info.First == p_Piece ? "THIS IS ITS FIRST PIECE (the one the explosion pushes)"
        : "its first piece is " + BoostName(s_Info.First);

    return s_Head + fmt::format(", a destructible with {} pieces, {}", s_Info.Pieces, s_First);
}

// Up to four shapes of a PhysX actor and one kind of filter data of each (query or simulation), under the
// scene's lock, every method checked to be inside PhysX first.
bool DebrisTracer::ReadShapeWords(uintptr_t p_Actor, size_t p_Getter, uint32_t* p_Words, uint32_t& p_Count) {
    uintptr_t s_ActorVTable = 0, s_GetShapes = 0, s_Physics = 0, s_Scene = 0, s_SceneVTable = 0;
    uintptr_t s_Lock = 0, s_Unlock = 0;
    p_Count = 0;

    return p_Actor && ReadRaw(p_Actor, s_ActorVTable) && ReadRaw(s_ActorVTable + kGetShapesOffset, s_GetShapes) &&
        s_GetShapes >= m_PhysXBase && s_GetShapes < m_PhysXEnd && ReadRaw(kPhysicsManagerGlobal + m_ModuleDelta, s_Physics) &&
        ReadRaw(s_Physics + kPhysicsSceneOffset, s_Scene) && ReadRaw(s_Scene, s_SceneVTable) &&
        ReadRaw(s_SceneVTable + kSceneLockWriteOffset, s_Lock) && ReadRaw(s_SceneVTable + kSceneUnlockWriteOffset, s_Unlock) &&
        s_Lock >= m_PhysXBase && s_Lock < m_PhysXEnd && s_Unlock >= m_PhysXBase && s_Unlock < m_PhysXEnd &&
        CallShapeFilters(s_Scene, reinterpret_cast<SceneLockFn>(s_Lock), reinterpret_cast<SceneUnlockFn>(s_Unlock),
            reinterpret_cast<GetShapesFn>(s_GetShapes), p_Actor, m_PhysXBase, m_PhysXEnd, p_Words, p_Count, p_Getter);
}

// 47's kinematic capsule against every shape of the piece, through the game's own filter shader.
std::string DebrisTracer::CollisionVerdict(void* p_Piece) {
    if (!m_Px.Controller) {
        return "47's controller not found";
    }

    uintptr_t s_ControllerVTable = 0, s_GetActor = 0, s_Capsule = 0;

    if (!ReadRaw(reinterpret_cast<uintptr_t>(m_Px.Controller), s_ControllerVTable) ||
        !ReadRaw(s_ControllerVTable + kPxGetActorOffset, s_GetActor) || s_GetActor < m_CctBase || s_GetActor >= m_CctEnd ||
        !CallGetActor(reinterpret_cast<GetActorFn>(s_GetActor), m_Px.Controller, s_Capsule) || !s_Capsule) {
        return "47's capsule actor could not be read";
    }

    uint32_t s_CapsuleWords[16] = {}, s_PieceWords[16] = {}, s_PieceQuery[16] = {};
    uint32_t s_CapsuleCount = 0, s_PieceCount = 0, s_QueryCount = 0;
    uintptr_t s_PieceActor = 0;

    if (!ReadShapeWords(s_Capsule, kGetSimulationFilterDataOffset, s_CapsuleWords, s_CapsuleCount) || s_CapsuleCount == 0) {
        return "the simulation filter data of 47's capsule could not be read";
    }

    if (!ReadRaw(reinterpret_cast<uintptr_t>(p_Piece) + kActorOffset, s_PieceActor) ||
        !ReadShapeWords(s_PieceActor, kGetSimulationFilterDataOffset, s_PieceWords, s_PieceCount) || s_PieceCount == 0) {
        return "the simulation filter data of the piece could not be read";
    }

    ReadShapeWords(s_PieceActor, kGetQueryFilterDataOffset, s_PieceQuery, s_QueryCount);

    // A fixed address with no vtable to check it by: only on the build it was read from.
    if (!m_KnownGame) {
        return "not checked: unknown game build";
    }

    const auto s_Shader = reinterpret_cast<FilterShaderFn>(kFilterShaderFunction + m_ModuleDelta);
    const uint32_t* s_C = s_CapsuleWords;
    std::string s_Out = fmt::format("47's capsule sim 0x{:x} 0x{:x} 0x{:x} 0x{:x}", s_C[0], s_C[1], s_C[2], s_C[3]);

    for (uint32_t i = 0; i < s_PieceCount; ++i) {
        const uint32_t* s_P = s_PieceWords + 4 * i;
        uint16_t s_Result = 0, s_Flags = 0;

        const std::string s_Answer = !CallFilterShader(s_Shader, kFilterDynamic | kFilterKinematic, s_C, kFilterDynamic,
            s_P, s_Result, s_Flags) ? "the shader could not be called"
            : s_Result == 2 ? "KILL - PhysX never lets them touch"
            : s_Result == 4 ? "SUPPRESS - no contact"
            : s_Flags & 1 ? fmt::format("THEY COLLIDE, contact solved (flags 0x{:x}): his body pushes the piece", s_Flags)
            : fmt::format("touch detected but not solved (flags 0x{:x})", s_Flags);

        s_Out += fmt::format(" | piece shape {} sim 0x{:x} 0x{:x} 0x{:x} 0x{:x}{} -> {}", i, s_P[0], s_P[1], s_P[2], s_P[3],
            i < s_QueryCount ? fmt::format(" (query word3 0x{:x})", s_PieceQuery[4 * i + 3]) : "", s_Answer);
    }

    return s_Out;
}

// The button: the piece PhysX has him on, and the nearest fragments of the whole scene within 2.5 m of him -
// the level's own too, so an unbroken vase can be asked about before anything breaks it.
void DebrisTracer::CollideCheck() {
    std::vector<std::pair<uintptr_t, void*>> s_Fragments;
    std::string s_Why;
    const bool s_Listed = ListFragments(s_Fragments, s_Why);

    std::scoped_lock s_Lock(m_DataMutex);

    std::vector<void*> s_Pieces;

    if (m_Px.TouchedFragment && LooksLikeGameObject(m_Px.TouchedFragment)) {
        s_Pieces.push_back(m_Px.TouchedFragment);
    }

    std::vector<std::pair<float, void*>> s_Near;

    for (const auto& [s_Actor, s_Object] : s_Fragments) {
        const SVector3 s_At = ToVec3(static_cast<IPhysicsObject*>(s_Object)->GetPosition());
        const float s_Dx = s_At.x - m_LastFeet.x, s_Dy = s_At.y - m_LastFeet.y, s_Dz = s_At.z - m_LastFeet.z;
        const float s_Dist = s_Dx * s_Dx + s_Dy * s_Dy + s_Dz * s_Dz;

        if (s_Dist < 2.5f * 2.5f && s_Object != m_Px.TouchedFragment) {
            s_Near.emplace_back(s_Dist, s_Object);
        }
    }

    std::sort(s_Near.begin(), s_Near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [s_Dist, s_Object] : s_Near) {
        if (s_Pieces.size() >= 12) {
            break;
        }

        s_Pieces.push_back(s_Object);
    }

    if (s_Pieces.empty()) {
        Log(fmt::format("COLLIDE CHECK: no fragment under 47 nor within 2.5 m of him{}.",
            s_Listed ? "" : " (the scene's fragments could not be listed: " + s_Why + ")"));
        return;
    }

    Log(fmt::format("COLLIDE CHECK: {} fragments within 2.5 m of 47 (the nearest {} are checked).", s_Near.size(),
        s_Pieces.size()));

    for (void* s_Piece : s_Pieces) {
        const SVector3 s_At = ToVec3(static_cast<IPhysicsObject*>(s_Piece)->GetPosition());

        Log(fmt::format("COLLIDE CHECK {}{} at ({:+.2f}, {:+.2f}, {:+.2f}) from his feet ({}; {}): {}.", BoostName(s_Piece),
            s_Piece == m_Px.TouchedFragment ? " (UNDER HIM)" : "", s_At.x - m_LastFeet.x, s_At.y - m_LastFeet.y,
            s_At.z - m_LastFeet.z, PieceFacts(s_Piece), OwnerFacts(s_Piece), CollisionVerdict(s_Piece)));
    }
}

// Where PhysX touches the piece against the set-down sphere, from what the controller holds now.
std::string DebrisTracer::ThirdFactorLine() {
    m_LastTouchOffset = -1.f;

    if (!m_Px.Controller) {
        return "no controller";
    }

    const auto s_P = reinterpret_cast<uintptr_t>(m_Px.Controller);
    Vec3f s_Touch {};
    Vec3d s_Centre {};
    float s_Radius = 0.f;

    if (!ReadRaw(s_P + kTouchedPosWorldOffset, s_Touch) || !ReadRaw(s_P + kCentreOffset, s_Centre) ||
        !ReadRaw(s_P + kPxRadiusOffset, s_Radius)) {
        return "the controller could not be read";
    }

    const double s_Dx = s_Touch.X - s_Centre.X, s_Dy = s_Touch.Y - s_Centre.Y;
    m_LastTouchOffset = static_cast<float>(std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy));
    m_LastSphereRadius = s_Radius * 0.5f; // DAT_141f9cd84 in FUN_1410d78c0

    return fmt::format(
        "PhysX touches the piece {:.3f} m from his axis; capsule radius {:.3f}, set-down sphere radius {:.3f} -> {}",
        m_LastTouchOffset, s_Radius, m_LastSphereRadius,
        m_LastTouchOffset <= m_LastSphereRadius ? "under the sphere (it can see the piece)"
            : "OUTSIDE THE SPHERE (it may miss the piece and find the floor)"
    );
}

// ---- The boost recorder ----

namespace {
constexpr float kBoostKeepBefore = 1.5f;   // Seconds kept before the rise.
constexpr float kBoostKeepAfter = 2.5f;    // Seconds recorded after the last rise.
constexpr float kBoostLongest = 8.f;       // No record runs longer than this.
constexpr float kBoostNearSide = 3.f;      // Tracked pieces this close to him sideways go in each line.
}

// One frame: where 47 is, what PhysX has him on, the pieces around him and, once he has risen, the
// piece that carried him. Everything read here was already read this frame, except the positions of
// the fragment under him and of the carrier. Runs only while "Record boosts" is on.
void DebrisTracer::RecordBoostFrame() {
    std::scoped_lock s_Lock(m_DataMutex);

    // A fragment's height, if it is still the fragment it was: the object must still carry the
    // fragment's vtable and, when the actor is known, still hold the same PhysX actor.
    const auto s_FragmentZ = [this](void* p_Object, uintptr_t p_Actor, float& p_Z) {
        uintptr_t s_VTable = 0, s_Actor = 0;

        if (!p_Object || !ReadRaw(reinterpret_cast<uintptr_t>(p_Object), s_VTable) ||
            s_VTable - m_ModuleDelta != kFragmentVTable ||
            !ReadRaw(reinterpret_cast<uintptr_t>(p_Object) + kActorOffset, s_Actor) ||
            (p_Actor && s_Actor != p_Actor)) {
            return false;
        }

        p_Z = static_cast<IPhysicsObject*>(p_Object)->GetPosition().z;
        return true;
    };

    BoostFrame s_Frame;
    s_Frame.T = m_Time;
    s_Frame.FeetZ = m_LastFeet.z;
    s_Frame.CentreZ = m_Px.CentreZ;
    s_Frame.Step = m_Px.Step;
    s_Frame.RideStep = m_Px.RideStep;
    s_Frame.Grounded = m_Px.Grounded;
    s_Frame.RideSpeedZ = m_Px.RideSpeedZ;
    s_Frame.TouchZ = m_Px.TouchZ;

    if (!m_Px.Controller || !m_Px.TouchedActor) {
        s_Frame.OnKind = 0;
    }
    else if (m_Px.TouchedFragment) {
        s_Frame.OnKind = 2;
        s_Frame.Fragment = m_Px.TouchedFragment;

        if (!s_FragmentZ(s_Frame.Fragment, 0, s_Frame.FragmentZ)) {
            s_Frame.Fragment = nullptr;
        }
    }
    else {
        s_Frame.OnKind = m_Px.TouchedType == 7 ? 1 : 3;
    }

    // The tracked pieces nearest to him, from where tracking last saw them - nothing more is read.
    if (m_FollowPieces) {
        float s_Side[6] = {};

        for (const auto& s_Item : m_Tracked) {
            if (!s_Item.Alive || s_Item.Path.empty()) {
                continue;
            }

            const SVector3& s_At = s_Item.Path.back();
            const float s_Dx = s_At.x - m_LastFeet.x, s_Dy = s_At.y - m_LastFeet.y;
            const float s_D = std::sqrt(s_Dx * s_Dx + s_Dy * s_Dy);

            if (s_D > kBoostNearSide) {
                continue;
            }

            int s_Slot = s_Frame.NearCount;

            while (s_Slot > 0 && s_Side[s_Slot - 1] > s_D) {
                if (s_Slot < 6) {
                    s_Side[s_Slot] = s_Side[s_Slot - 1];
                    s_Frame.Near[s_Slot] = s_Frame.Near[s_Slot - 1];
                    s_Frame.NearZ[s_Slot] = s_Frame.NearZ[s_Slot - 1];
                }

                --s_Slot;
            }

            if (s_Slot < 6) {
                s_Side[s_Slot] = s_D;
                s_Frame.Near[s_Slot] = s_Item.Object;
                s_Frame.NearZ[s_Slot] = s_At.z;
                s_Frame.NearCount = std::min(6, s_Frame.NearCount + 1);
            }
        }
    }

    // His rise since the frame before - only between frames close together, not across a pause.
    const BoostFrame* s_Previous = m_BoostOpen && !m_BoostPost.empty() ? &m_BoostPost.back()
        : !m_BoostOpen && !m_BoostPre.empty() ? &m_BoostPre.back() : nullptr;
    // A save being loaded moves 47 too, but with the game clock standing still and PhysX's capsule left
    // where it was: a rise only counts with the clock running and the capsule where his feet are.
    const bool s_CapsuleWithHim = std::abs(static_cast<float>(s_Frame.CentreZ) - s_Frame.FeetZ - 0.8f) < 0.5f;
    const bool s_Rose = s_Previous && m_Time > s_Previous->T && m_Time - s_Previous->T < 0.25f && s_CapsuleWithHim &&
        s_Frame.FeetZ - s_Previous->FeetZ > m_BoostRiseThreshold;

    if (!m_BoostOpen) {
        if (!s_Rose) {
            m_BoostPre.push_back(s_Frame);

            while (!m_BoostPre.empty() && m_BoostPre.front().T < m_Time - kBoostKeepBefore) {
                m_BoostPre.pop_front();
            }

            return;
        }

        // The carrier: the fragment he is on now, or else the last one he was on just before.
        m_BoostCarrier = s_Frame.Fragment;

        for (auto it = m_BoostPre.rbegin(); !m_BoostCarrier && it != m_BoostPre.rend() && it - m_BoostPre.rbegin() < 10;
             ++it) {
            m_BoostCarrier = it->Fragment;
        }

        m_BoostCarrierActor = 0;

        m_BoostCarrierFacts.clear();

        if (m_BoostCarrier) {
            ReadRaw(reinterpret_cast<uintptr_t>(m_BoostCarrier) + kActorOffset, m_BoostCarrierActor);
            m_BoostCarrierFacts = PieceFacts(m_BoostCarrier);
        }

        m_BoostOpen = true;
        m_BoostStart = m_Time;
        m_BoostUntil = m_Time + kBoostKeepAfter;
        m_BoostPost.clear();

        Log(fmt::format(
            "Boost: 47 rose {:+.2f} m in one frame - recording until {:.1f} s after his last rise.",
            s_Frame.FeetZ - s_Previous->FeetZ, kBoostKeepAfter
        ));
    }
    else if (s_Rose) {
        m_BoostUntil = std::min(m_BoostStart + kBoostLongest, m_Time + kBoostKeepAfter);
    }

    if (m_BoostCarrier) {
        if (s_FragmentZ(m_BoostCarrier, m_BoostCarrierActor, s_Frame.CarrierZ)) {
            s_Frame.Carrier = m_BoostCarrier;
        }
        else {
            m_BoostCarrier = nullptr;   // Gone or reused: not touched again.
        }
    }

    m_BoostPost.push_back(s_Frame);

    if (m_Time >= m_BoostUntil || m_BoostPost.size() > 3000) {
        WriteBoost();

        m_BoostOpen = false;
        m_BoostCarrier = nullptr;
        m_BoostPre.clear();
        m_BoostPost.clear();
    }
}

// A tracked piece keeps its number; any other fragment gets an F number of its own, for this level.
std::string DebrisTracer::BoostName(void* p_Fragment) {
    for (const auto& s_Item : m_Tracked) {
        if (s_Item.Object == p_Fragment) {
            return fmt::format("#{}", s_Item.Id);
        }
    }

    auto [s_It, s_New] = m_BoostNames.try_emplace(p_Fragment, static_cast<int>(m_BoostNames.size()) + 1);
    return fmt::format("F{}", s_It->second);
}

// The whole record, frame by frame, and a summary after it. Called with the data lock held.
void DebrisTracer::WriteBoost() {
    std::vector<const BoostFrame*> s_All;
    s_All.reserve(m_BoostPre.size() + m_BoostPost.size());

    for (const auto& s_Frame : m_BoostPre) {
        s_All.push_back(&s_Frame);
    }

    for (const auto& s_Frame : m_BoostPost) {
        s_All.push_back(&s_Frame);
    }

    if (m_BoostPost.empty()) {
        return;
    }

    ++m_BoostCount;
    const size_t s_RiseAt = m_BoostPre.size();
    const BoostFrame& s_Rise = m_BoostPost.front();
    const BoostFrame* s_Before = m_BoostPre.empty() ? nullptr : &m_BoostPre.back();

    LogFile(fmt::format(
        "=== BOOST {}: 47 rose {:+.3f} m in one frame at t={:.3f}. {} frames before, {} from the rise on. ===",
        m_BoostCount, s_Before ? s_Rise.FeetZ - s_Before->FeetZ : 0.f, s_Rise.T, m_BoostPre.size(), m_BoostPost.size()
    ));

    float s_Peak = s_Rise.FeetZ, s_BiggestRise = 0.f, s_BiggestAt = s_Rise.T, s_CarrierPeak = -1e9f;
    int s_Rising = 0;
    std::string s_CarrierName;

    for (size_t i = 0; i < s_All.size(); ++i) {
        const BoostFrame& s_F = *s_All[i];
        const BoostFrame* s_P = i > 0 ? s_All[i - 1] : nullptr;
        const float s_Dz = s_P ? s_F.FeetZ - s_P->FeetZ : 0.f;

        std::string s_On;

        switch (s_F.OnKind) {
            case 1: s_On = "static"; break;
            case 2:
                if (s_F.Fragment) {
                    s_On = fmt::format("FRAGMENT {} z={:.3f}", BoostName(s_F.Fragment), s_F.FragmentZ);

                    if (s_P && s_P->Fragment == s_F.Fragment) {
                        s_On += fmt::format(" ({:+.3f})", s_F.FragmentZ - s_P->FragmentZ);
                    }
                }
                else {
                    s_On = "a fragment (unreadable)";
                }
                break;
            case 3: s_On = "another moving body"; break;
            default: s_On = "nothing"; break;
        }

        std::string s_Carrier;

        if (s_F.Carrier) {
            s_CarrierName = BoostName(s_F.Carrier);
            s_Carrier = fmt::format(" | carrier {} z={:.3f}", s_CarrierName, s_F.CarrierZ);

            if (s_P && s_P->Carrier == s_F.Carrier) {
                s_Carrier += fmt::format(" ({:+.3f})", s_F.CarrierZ - s_P->CarrierZ);
            }

            s_CarrierPeak = std::max(s_CarrierPeak, s_F.CarrierZ);
        }

        // Where PhysX last touched the piece, against his feet. On the vase's pieces it was 4 to 16 cm
        // above them: the piece inside his capsule, not under it. mDeltaXP is not written here: in
        // this game it reads infinite (the time the ride divides by is zero).
        std::string s_Ride = fmt::format(" | touch z={:.3f} ({:+.3f} from his feet)", s_F.TouchZ, s_F.TouchZ - s_F.FeetZ);

        std::string s_Near;

        for (int k = 0; k < s_F.NearCount; ++k) {
            s_Near += fmt::format(" {} z={:.2f}", BoostName(s_F.Near[k]), s_F.NearZ[k]);
        }

        LogFile(fmt::format(
            "{} t={:.3f} 47 z={:.3f} ({:+.3f}) centre z={:.3f} | on {} | step {} ride {}{} | {}{}{}{}",
            i < s_RiseAt ? "pre " : i == s_RiseAt ? "RISE" : "post", s_F.T, s_F.FeetZ, s_Dz, s_F.CentreZ, s_On,
            s_F.Step, s_F.RideStep, s_F.Step == s_F.RideStep ? " (same)" : "", s_F.Grounded ? "grounded" : "in the air",
            s_Ride, s_Carrier, s_Near.empty() ? "" : " | near:" + s_Near
        ));

        if (i >= s_RiseAt) {
            s_Peak = std::max(s_Peak, s_F.FeetZ);

            if (s_Dz > s_BiggestRise) {
                s_BiggestRise = s_Dz;
                s_BiggestAt = s_F.T;
            }

            s_Rising += s_Dz > 0.05f ? 1 : 0;
        }
    }

    const float s_From = s_Before ? s_Before->FeetZ : s_Rise.FeetZ;
    std::string s_WasOn = "no frame before it";

    if (s_Before) {
        s_WasOn = s_Before->OnKind == 2 && s_Before->Fragment ? "fragment " + BoostName(s_Before->Fragment)
            : s_Before->OnKind == 1 ? "something static" : s_Before->OnKind == 3 ? "another moving body" : "nothing";
    }

    std::string s_CarrierLine = "no fragment under him in the frames before the rise";

    if (!s_CarrierName.empty()) {
        const float s_CarrierRise = s_Before && s_Before->Fragment == s_Rise.Carrier && s_Rise.Carrier
            ? s_Rise.CarrierZ - s_Before->FragmentZ : 0.f;

        s_CarrierLine = fmt::format(
            "carried by {} ({}), which moved {:+.3f} m in the rise frame and got as high as z={:.3f}", s_CarrierName,
            m_BoostCarrierFacts.empty() ? "not read" : m_BoostCarrierFacts, s_CarrierRise, s_CarrierPeak
        );
    }

    LogFile(fmt::format(
        "BOOST {} summary: 47 from z={:.3f} up to z={:.3f} ({:+.3f} m), ended at z={:.3f}. Biggest rise in one frame "
        "{:+.3f} m at t={:.3f}; {} frames rising more than 5 cm. Before the rise PhysX had him on {}; {}.",
        m_BoostCount, s_From, s_Peak, s_Peak - s_From, m_BoostPost.back().FeetZ, s_BiggestRise, s_BiggestAt, s_Rising,
        s_WasOn, s_CarrierLine
    ));

    Log(fmt::format(
        "Boost {} written to the log: {:+.2f} m, up to z={:.2f}, ended at z={:.2f}.", m_BoostCount, s_Peak - s_From,
        s_Peak, m_BoostPost.back().FeetZ
    ));
}

void DebrisTracer::ResetBoostRecorder() {
    std::scoped_lock s_Lock(m_DataMutex);

    if (m_BoostOpen) {
        Log("Boost recorder switched off in the middle of a boost: that record is dropped.");
    }

    m_BoostPre.clear();
    m_BoostPost.clear();
    m_BoostOpen = false;
    m_BoostCarrier = nullptr;
}

// A new scene (another level, or the same one restarted). The cripple box is switched off here, while 47
// and the scene are still there: left to the scene, it would be deleted without Deactivate and the ammo
// would stay infinite.
DEFINE_PLUGIN_DETOUR(DebrisTracer, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene) {
    RemoveCrippleBox();
    m_InfiniteAmmo = false;
    m_HitmanInfiniteAmmo = false;

    std::scoped_lock s_Lock(m_DataMutex);
    m_Tracked.clear();
    m_HeroPath.clear();
    m_TrackedCount = 0;
    m_Px.Controller = nullptr;
    m_Removed.clear();
    m_Baseline.clear();
    m_HaveBaseline = false;
    m_BaselineAt = -1.f;
    m_BoostPre.clear();
    m_BoostPost.clear();
    m_BoostOpen = false;
    m_BoostCarrier = nullptr;
    m_BoostNames.clear();
    m_AutoPiece = nullptr;

    return HookResult<void>(HookAction::Continue());
}

DEFINE_ZHM_PLUGIN(DebrisTracer);
