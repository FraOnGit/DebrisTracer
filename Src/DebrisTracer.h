#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "IPluginInterface.h"

#include <Glacier/ZEntity.h>
#include <Glacier/ZMath.h>

/**
 * DebrisTracer - traces fragments and 47, and measures what happens under him.
 *
 * The main part, each piece only when its switch is on: tracking every fragment around 47, found
 * through PhysX's own list of the scene's bodies (no need to walk over them); tracking 47; giving
 * violins and infinite ammo; and sending every fragment around him under the map to start afresh.
 * Below, behind "Measuring tools", the instruments of the research - timers, PhysX's state, the ray
 * under his feet, the rocket detector, the boost recorder and the launch by hand - each off, and costing
 * nothing, until asked for.
 *
 * The research, for the record:
 *
 * What the measurements settled: this is PhysX's own "ride on the touched object". Each time PhysX
 * moves 47 it looks at what he touched last, and if that has moved up it moves him up by as much,
 * straight through anything in the way - but only when it holds the piece as his footing AND the
 * scene has taken a physics step since it last looked. The launch waits for that moment, then
 * moves the piece up; the engine does the carrying. Measured: 13 sends of 13 at that moment, 0 of
 * 31 at any other.
 *
 * Tried and dropped, all measured: where in the frame the plugin runs (three priorities, same rate),
 * which frames the launch goes out on (with and without physics, same rate), leaving the piece at
 * its own height (worse), and giving the piece a real velocity instead of moving it (the contact
 * under his feet eats it before it moves). They are in git history, not here.
 *
 * Pieces are found by rays around his feet: the result hands over a proxy whose first word is the
 * physics body, and bodies are recognised by the vtable addresses Ghidra read out of HITMAN3.exe.
 */
class DebrisTracer : public IPluginInterface {
public:
    ~DebrisTracer() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawMenu() override;
    void OnDrawUI(bool p_HasFocus) override;
    void OnDraw3D(IRenderer* p_Renderer) override;

private:
    struct Tracked {
        int Id = 0;   // Stays with the piece for life, so it can be followed from line to line.
        void* Object = nullptr;
        std::string TypeName;
        std::vector<SVector3> Path;
        SVector3 Start;
        float LastSide = 0.f;   // How far it was from 47 on the previous frame.
        bool Alive = true;
        uintptr_t VTable = 0;   // What it was when found. Anything else there later is another object.
        bool Rocketed = false;  // Already written to the log as a rocket.

        // The box the piece really fills, against its origin. The origin is where the marker used to be
        // drawn, and on the flowers it is 0.7 m away from the flowers themselves.
        bool HasBox = false;
        SVector3 BoxMin;
        SVector3 BoxMax;
        float BoxReadAt = -100.f;
    };

    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);
    void DrawMeasuringTools();
    void DrawLaunchTools();

    void Probe(bool p_Verbose);
    void UpdateTracked();

    bool LooksLikeGameObject(const void* p_Pointer) const;
    std::string DescribePointer(const void* p_Pointer) const;

    // What PhysX itself says 47 is standing on. The carry is PhysX's own "ride on the touched
    // object": every time it moves 47 it checks what he touched last, and if that moved up since,
    // it moves him up by the same amount - but only if the scene has taken a physics step since it
    // last looked. This reads that state, and nothing is written.
    struct PxView {
        void* Controller = nullptr;   // 47's capsule controller, the pointer the game itself holds.
        std::string Why;              // Why it was not found, when it was not.
        void* TouchedActor = nullptr;
        int TouchedType = 0;          // PhysX's concrete type: 6 is a moving body, 7 a static one.
        int TouchedPiece = 0;         // Our number for it, when it is one of the pieces we follow.
        uint32_t Step = 0;            // The scene's step counter now.
        uint32_t RideStep = 0;        // The step at which the ride was last worked out.
        bool Moving = false;          // PhysX's own "standing on something that moves".
        bool Grounded = false;        // Collision below, from the last move.
        double CentreZ = 0.0;         // The centre of the capsule.
        void* TouchedFragment = nullptr; // The game's own object for the fragment he stands on, if one.
        float RideSpeedZ = 0.f;       // mDeltaXP: the ride's last delta divided by the time since the ride before.
        float TouchZ = 0.f;           // mTouchedPosShape_World: the point of the piece he touched, rewritten after each move.
    };

    bool ModuleRange(const char* p_Name, uintptr_t& p_Base, uintptr_t& p_End) const;
    void FindController();
    void ReadPhysX();
    std::string PxLine() const;

    // Whether PhysX itself holds this piece as 47's footing - the thing the carry actually depends
    // on. The first automatic test showed our own ray from his feet disagreeing with PhysX for ten
    // launches running: PhysX had him on the piece, the ray saw the floor, and nothing was launched.
    bool PhysXHolds(void* p_Piece) const;

    // The box a piece fills in the world, as PhysX has it, and its rotation - the real one, and the
    // game's copy that SetPosition sends back with every move. A piece's origin is not its middle:
    // it can lie outside the piece altogether, so a stale rotation swings the piece around it.
    struct PieceBox {
        float MinX = 0.f, MinY = 0.f, MinZ = 0.f;
        float MaxX = 0.f, MaxY = 0.f, MaxZ = 0.f;
        float Q[4] = {};                 // Real rotation (x, y, z, w).
        float CachedQ[4] = {};           // The game's copy.
        float RotationGapDeg = 0.f;      // Between the two.
        float OriginToMiddle = 0.f;      // From the origin to the middle of the box, m.
    };

    bool ReadPieceBox(void* p_Piece, PieceBox& p_Out, std::string& p_Why) const;
    void MovePiece(void* p_Piece, const float4& p_Position);
    std::string DescribeBox(void* p_Piece);
    std::string PieceFacts(void* p_Piece);

    // Moves pieces with the rotation PhysX really has for them, instead of the game's copy.
    bool m_KeepRealRotation = false;

    // How far over the top of a piece placed by its box 47 is lifted. At 2 cm the big pieces were
    // never taken: PhysX never saw him move at all, as if the lift had been refused.
    float m_HopMargin = 0.15f;
    bool m_PreferBig = true;         // The test picks the tallest piece in reach, not the nearest.
    void* m_CheckPiece = nullptr;    // Looked at again on the next frame, to see where the move put it.
    std::string m_CheckWhat;
    int m_CheckIn = 0;

    // The game's object for the actor PhysX has him on, reached the way the game's own ray code
    // reaches it: the actor's userData, which for a fragment is the object's second base, 8 bytes in.
    void* FragmentFromActor(uintptr_t p_Actor) const;

    // Fires the rays once, now, on a fresh list: pieces found only when one is needed.
    void FindPiecesNow();

    // The whole rule of the carry, measured over 44 sends: PhysX must hold the piece as his footing,
    // and the scene must have taken a physics step since the ride last looked. With both, 13 of 13
    // carried him; with the step unchanged, 0 of 10; without the footing, 0 of 21. So the piece is
    // only ever sent when both hold, and otherwise the launch simply waits a frame or two.
    bool ReadyToSend(void* p_Piece) const;
    bool TrySend();

    void FindSupport();

    // Every fragment in the scene around 47, asked of PhysX itself: the scene's list of moving bodies
    // (getNbActors/getActors, slots 18 and 19 of NpScene, read out with Ghidra), each checked to be a
    // fragment by its exact vtable. No rays, and no need to walk over the pieces to find them.
    bool ListFragments(std::vector<std::pair<uintptr_t, void*>>& p_Out, std::string& p_Why);
    void ScanScene();
    void TakeBaseline();
    bool IsDebris(uintptr_t p_Actor, const SVector3& p_Position) const;

    // Sends every fragment around 47 far under the map, with gravity off so it stays there, and
    // forgets them. The game's own pieces are not destroyed: that could take the game down.
    void RemoveAllFragments();
    bool SetGravity(uintptr_t p_Actor, bool p_On);
    void RunSequence();
    void FireLaunch(void* p_Body, float p_HomeZ, const std::string& p_Why);
    void LogSettings();

    // Conveniences for testing, done the way the SDK's own mods do them: a violin straight into 47's
    // inventory, as the Assets mod gives items, and infinite ammo through a cripple box with limited
    // ammo turned off, as the Player mod does it.
    void GiveViolin();
    void SetInfiniteAmmo();
    bool CreateCrippleBox();
    void RemoveCrippleBox();

    // Whether this is the game build the addresses were read from: a few vtable slots are compared with
    // the functions Ghidra found in them. The one call that has no vtable to check it by (the filter
    // shader of "Collide check") is only made when they all match.
    bool CheckGameBuild() const;
    bool m_KnownGame = false;


    void* PickRestingPiece(const SVector3& p_Around) const;

    void Log(const std::string& p_Message);
    void LogFile(const std::string& p_Message);

    DECLARE_PLUGIN_DETOUR(DebrisTracer, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene);

private:
    bool m_ShowWindow = true;
    bool m_Enabled = true;           // Off: nothing is read, searched or drawn.
    bool m_AlwaysVisible = false;    // The window with the mod menu closed too.

    // Tracking the fragments around 47, found through PhysX's own list of the scene's bodies.
    float m_TrackRadius = 20.f;
    float m_ScanInterval = 0.5f;
    float m_LastScan = -100.f;
    int m_SceneBodies = 0;           // Moving bodies in the scene at the last scan, for the window.
    std::vector<uintptr_t> m_Removed; // Actors sent under the map: never tracked again.

    // The fragments already in the level when it started - the props that can break, and the pool the
    // pieces come from: some two thousand in Miami. Only a fragment that is not among them, or has
    // moved from where it was, is debris worth tracking or removing.
    std::unordered_map<uintptr_t, SVector3> m_Baseline;
    bool m_HaveBaseline = false;
    float m_BaselineAt = -1.f;
    std::vector<uintptr_t> m_ActorBuffer; // Sized to the scene: Miami has sixteen thousand moving bodies.
    std::atomic<bool> m_RemoveRequested = false;
    int m_ViolinCount = 1;

    // The measuring tools, each off until asked for, so none of them costs anything until then.
    bool m_MeasuringTools = false;
    bool m_ToolTimers = false;
    bool m_ToolPhysX = false;
    bool m_ToolAutoTest = false;
    bool m_AutoProbe = true;     // Keep looking for new pieces under 47 while he walks around.

    // The boost recorder: what 47, PhysX and the pieces around him did in the frames before and after
    // he rises in one frame - for the boosts the game does by itself, like the vase and the breaching
    // charge that took 47 through a floor in Bangkok. It reads and keeps nothing while it is off.
    struct BoostFrame {
        float T = 0.f;
        float FeetZ = 0.f;
        double CentreZ = 0.0;
        int OnKind = 0;               // 0 nothing, 1 static, 2 fragment, 3 another moving body.
        void* Fragment = nullptr;     // The fragment PhysX has him on, if one.
        float FragmentZ = 0.f;
        uint32_t Step = 0;
        uint32_t RideStep = 0;
        bool Grounded = false;
        float RideSpeedZ = 0.f;
        float TouchZ = 0.f;
        void* Carrier = nullptr;      // After the rise: the piece that carried him, followed.
        float CarrierZ = 0.f;
        int NearCount = 0;
        void* Near[6] = {};           // The tracked pieces nearest to him, and how high they are.
        float NearZ[6] = {};
    };

    void RecordBoostFrame();
    void WriteBoost();
    void ResetBoostRecorder();
    std::string BoostName(void* p_Fragment);

    std::atomic<bool> m_DescribeRequested = false;

    // Runs the game's own set-down sweep (collision manager slot 110) from 1 m above 47's feet, the way
    // FUN_1410d78c0 runs it after a ride, and writes what it hits: the piece under him, or what is below
    // it. Moves nothing. With the query filter data of the piece's shapes and the tables it is judged by.
    // p_Locked: the caller already holds m_DataMutex (the launch sequence does).
    void ProbeSetDown(bool p_Locked = false);
    std::atomic<bool> m_ProbeSetDownRequested = false;

    // The rocket detector: every tracked piece that moves more than m_RocketStep in one frame goes to the
    // log once, with its mass, size and where it was against 47. Reads nothing of its own: tracking
    // already reads every piece's position each frame.
    bool m_ToolRockets = false;
    bool m_ShowRealBoxes = false;    // Draw the pieces near 47 where they really are, box and all.
    float m_RocketStep = 5.f;
    int m_RocketCount = 0;

    // Who a piece belongs to (the owner entity at object+0x18) and, if that owner is a destructible, how
    // many pieces it has and which one is its "first piece" - the only one the explosion pushes, once
    // per piece of it in range (Ghidra, 27/09: FUN_140317810 -> accessor slot 7).
    struct OwnerInfo {
        uintptr_t Owner = 0;
        bool Destructible = false;
        size_t Pieces = 0;
        void* First = nullptr;  // The destructible's first piece, when it could be read.
        std::string Why;        // Set when the walk stopped early.
    };

    OwnerInfo ReadOwner(void* p_Piece) const;
    std::string OwnerFacts(void* p_Piece);


    // Whether PhysX lets a piece and 47's capsule touch at all, asked of the game's own simulation filter shader
    // with the simulation filter data both really have. On 27/09 the violin's first piece was stopped by his body;
    // the vase's pieces had been seen inside his capsule. The same layer for both: the answer is in the rest.
    bool ReadShapeWords(uintptr_t p_Actor, size_t p_Getter, uint32_t* p_Words, uint32_t& p_Count);
    std::string CollisionVerdict(void* p_Piece);
    void CollideCheck();
    std::atomic<bool> m_CollideCheckRequested = false;


    // The third factor: at every send, how far from his axis PhysX touches the piece against the
    // game's set-down sphere (half the capsule radius, centred on him), plus the set-down probe itself.
    bool m_ToolThirdFactor = false;
    float m_LastTouchOffset = -1.f;
    float m_LastSphereRadius = 0.f;
    std::string ThirdFactorLine();


    // The escape speed of the tracked pieces: how fast PhysX may push a piece out of whatever it is
    // stuck in. Unlimited by default in PhysX 3.4 - the suspect behind the rockets. Read into every
    // rocket line.
    bool EscapeAccess(void* p_Piece, uintptr_t& p_Actor, uintptr_t& p_Scene, uintptr_t& p_Lock, uintptr_t& p_Unlock,
        uintptr_t& p_Get, uintptr_t& p_Set, uintptr_t& p_Flags) const;
    std::string EscapeFacts(void* p_Piece) const;

    bool m_ToolRecordBoosts = false;
    std::deque<BoostFrame> m_BoostPre;       // The last second and a half, always.
    std::vector<BoostFrame> m_BoostPost;     // From the rise on, while a record is open.
    bool m_BoostOpen = false;
    float m_BoostStart = 0.f;                // When the rise was seen.
    float m_BoostUntil = 0.f;                // Recording goes on until then, pushed on by each new rise.
    float m_BoostRiseThreshold = 0.3f;       // One frame's rise that counts as a boost, m.
    void* m_BoostCarrier = nullptr;
    uintptr_t m_BoostCarrierActor = 0;
    std::string m_BoostCarrierFacts;         // Its mass and size, read the moment the rise was seen.
    int m_BoostCount = 0;
    std::unordered_map<void*, int> m_BoostNames; // Fragments that are not tracked get a number of their own.

    // Following every piece frame by frame - reading its position, keeping its path, drawing it -
    // cost about ten frames a second with forty pieces, and the launch no longer needs it: PhysX
    // names the piece 47 stands on, and the rays are fired only when a piece has to be found. It is
    // kept, off, for when the pieces themselves need watching.
    bool m_FollowPieces = false;

    // Where the frame time goes. With an early DebrisTracer loaded the game ran at 40 frames a second,
    // without it at 60 to 80: some 11 ms more every frame, far more than the few reads it does should
    // cost. So each part is timed rather than guessed at - averaged over a second, shown in the
    // window, written to the log every five seconds.
    enum TimedPart { kTimePlayer, kTimeRay, kTimePhysX, kTimeLaunch, kTimeTest, kTimeFrame, kTimeWindow, kTimeDraw3D,
        kTimedParts };
    std::atomic<long long> m_TimeNs[kTimedParts] {};
    std::atomic<int> m_TimeFrames = 0;
    double m_TimeShownMs[kTimedParts] {};
    float m_TimeWindowStart = 0.f;
    float m_LastTimingLog = 0.f;

    // The ray from his feet, fired every frame. Every ray takes the physics scene's lock, and if it
    // has to wait for the simulation each time, this alone could be the missing frame rate. With it
    // off, the ray is fired only while a launch is being pressed or run.
    bool m_SupportRayEachFrame = false;   // Measured: 3 to 4.5 ms a frame, all of it waiting for the lock.
    bool m_TraceHero = false;
    float m_ProbeInterval = 0.25f;
    float m_ProbeReach = 1.2f;   // How far around 47 the rays are fired.

    uintptr_t m_ModuleDelta = 0; // Runtime address minus the address Ghidra saw.

    float m_Time = 0.f;
    float m_LastProbe = -100.f;
    int m_NextId = 1;
    SVector3 m_LastFeet;

    std::mutex m_DataMutex;
    std::vector<Tracked> m_Tracked;
    std::vector<SVector3> m_HeroPath;

    // What counts as "under his feet": close enough to the side, and within reach below and above.
    float m_UnderRadius = 0.55f;
    float m_UnderBelow = 0.6f;
    float m_UnderAbove = 0.5f;

    // What 47 is actually standing on, asked rather than guessed. A ray that starts inside him is
    // stopped at zero distance and never sees the ground; one that starts level with his feet is at
    // the boundary, and what it hits is what he is standing on.
    void* m_SupportBody = nullptr;
    int m_SupportId = 0;
    std::string m_SupportName;
    float m_SupportDrop = 0.f;
    float m_SupportStart = 0.03f;
    float m_SupportReach = 0.4f;

    std::atomic<bool> m_HasUnder = false;   // For the window: is there a piece under him right now?
    std::atomic<float> m_UnderDist = 0.f;
    std::atomic<float> m_UnderDz = 0.f;
    void* m_UnderObject = nullptr;

    float m_LiftAmount = 10.f;  // How far the launch takes him. Tests are run at 8-10 m.

    // How far the launch also moves the piece sideways. The ride carries 47 only upwards, and the game's
    // set-down looks for his footing straight below where he was: a piece that ends up beside him is not
    // found. One send only, as a launch that leaves him behind is the point, not a failure to retry.
    float m_LaunchSideways = 0.f;

    // With the send gated on the right moment a retry should rarely be needed; it stays as a safety
    // net. The piece is put back under his feet, and sent again at the next right moment.
    int m_MaxRetries = 20;
    int m_RetriesLeft = 0;
    bool m_RetryPutBack = false;
    void* m_LaunchedPiece = nullptr;
    float m_PieceHomeZ = 0.f;
    float m_LaunchBaseZ = 0.f;
    int m_Sends = 0;                // How many times this launch has sent the piece up.
    int m_WaitFrames = 0;           // Frames waited for the moment the carry can happen.
    int m_MaxWaitFrames = 45;       // The physics steps about 30 times a second; this is plenty.

    // The engine holds a character a skin's width above his footing, and from a few centimetres
    // off, a piece that leaves does not take him with it. So the gap is closed first.
    float m_GapAllowed = 0.004f;
    float m_GapStep = 0.02f;      // The most the piece is raised in one frame while closing it.
    int m_GapFrames = 0;
    int m_GapMaxFrames = 30;

    // When he is on the floor rather than on a fragment: a piece is put under his feet, level with
    // them, and he is hopped up a few centimetres so he comes down on it - the one move the engine
    // does not fight, because it is his own. Once the ray says it is his footing, it is launched.
    enum class Stage { Idle, Settling, Confirming, Tightening, Retrying };

    Stage m_Stage = Stage::Idle;
    void* m_StagedPiece = nullptr;
    int m_Attempts = 0;
    int m_MaxAttempts = 12;
    float m_HopUp = 0.05f;

    // Where a staged piece goes. Its origin is its middle: put level with his feet, it sat half in
    // the floor, and PhysX pushed it out up or down as it happened - 8 of 15 fell through. By its
    // box, its bottom goes just above his feet and he is hopped over its top.
    // 0 = origin at his feet (the old way), 1 = by its box, 2 = the two in turn, for comparing.
    int m_PlaceMode = 1;
    float m_PlaceClearance = 0.01f;
    bool m_AlwaysStage = true;       // The test puts a piece under him even when he already stands on one.
    int m_StageCount = 0;
    bool m_StageByBox = false;       // How the piece being staged now was placed.
    float m_StageDX = 0.f;           // Its origin against the middle of its box, so the box is centred under him.
    float m_StageDY = 0.f;
    int m_StageTried[2] = {};        // Per way of placing: pieces staged,
    int m_StageHeld[2] = {};         // ... taken by PhysX as his footing,
    int m_StageSank[2] = {};         // ... and gone down through the floor.

    PxView m_Px;
    uintptr_t m_CctBase = 0;
    uintptr_t m_CctEnd = 0;
    uintptr_t m_PhysXBase = 0;
    uintptr_t m_PhysXEnd = 0;
    int m_ControllerCount = 0;
    float m_LastControllerSearch = -100.f;
    float m_PxTraceUntil = -100.f;   // Until when every frame's reading goes to the log.

    void* m_AutoPiece = nullptr;     // The piece staging should use, instead of the one under his feet.

    // Pieces that have already failed in this test, and are not picked again. Without this the test
    // picked the same unusable fragment ten times running: one lying flush with the floor stays the
    // nearest resting piece after every try, and 47 stands on the floor beside it rather than on it.
    std::vector<void*> m_AutoRejected;
    void* m_AutoUsedPiece = nullptr; // The piece the current launch actually used.


    // The cripple box goes with the scene, so a level change turns infinite ammo off.
    bool m_InfiniteAmmo = false;
    bool m_HitmanInfiniteAmmo = false; // What the game says, read every frame: shown in the tick too.
    ZEntityRef m_CrippleBoxEntity;
    std::atomic<bool> m_AmmoToggleRequested = false;
    std::atomic<bool> m_ViolinRequested = false;

    std::atomic<bool> m_PxTraceRequested = false;
    std::atomic<bool> m_ProbeRequested = false;
    std::atomic<bool> m_SequenceRequested = false;
    std::atomic<bool> m_ClearRequested = false;
    std::atomic<int> m_TrackedCount = 0;

    std::mutex m_LogMutex;
    std::deque<std::string> m_Log;
    std::ofstream m_File;    // Kept open: reopening it per line was costing frames.
};

DECLARE_ZHM_PLUGIN(DebrisTracer)
