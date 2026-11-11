// ==============================================================================
// WalkFreePhysics.dll - ATS / ETS2 統合歩行プラグイン (完全動的PhysX解決 + 高精度物理統合版)
// 
// 【主な仕様】
// 1. 完全動的PhysXパイプライン (STAGE 1 ~ STAGE 4):
//    - STAGE 1: PEセクション走査 (.rdata) による NpScene vtable の動的解決
//    - STAGE 2: K32QueryWorkingSetEx による非同期ヒープスキャン (SceneScannerThread)
//    - STAGE 3: テスト Raycast による PxScene の厳格バリデーション (g_pVerifiedScene 確定)
//    - STAGE 4: Telemetry (truck.placement) と RigidDynamic の照合による原点オフセット確定
//
// 2. 高精度物理・障害物当たり判定・壁スライド:
//    - 5点クロスサンプリング (中心 + 前後左右 0.25m) による縁石・段差の取りこぼし防止
//    - マルチハイト判定 (足元 +0.40m 膝判定 ＆ 胸/腰 +0.85m 障害物判定)
//    - 壁衝突法線 (Normal) を取得し、壁に沿って滑らかに滑る Wall-Slide (v - (v・n)n)
//    - 時定数 0.06秒の指数平滑補間による滑らかな目線高さ追従
//
// 3. 長距離コライダー維持機構 (focus2 + region_query):
//    - focus2 (scene->vtable[0x178]): エンジンフォーカス座標を歩行者へ常時供給
//    - region_query (box_builder + region_query): 250ms毎に能動的コライダー強制ロード (80m)
//
// 4. オリジナル操作性 & 安全機能の完全維持:
//    - WASD 移動、Shift ダッシュ、Space/Ctrl 垂直移動 (Noclip時)
//    - Alt キー: 通常歩行 / Noclip (壁抜け) のワンタッチ切替
//    - F9 キー: キャビン脇からのスムーズなスポーン / 運転席への復帰
//    - F10 キー: リアルタイムモニター表示の ON/OFF
//    - Anti-Void: 奈落落下・床抜け時の安全な自動緊急脱出
//    - VEH (ベクタ例外ハンドラ) + SafeRead による二重の例外保護
// ==============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#define SCS_EXPORT __declspec(dllexport)

// -----------------------------------------------------------------------------
// TruckerWalkAround ライブログ出力システム
// -----------------------------------------------------------------------------
static void TruckerLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 標準出力
    printf("%s", buf);

    // %TEMP%\TruckerWalkAround_Live.log への追記（即時フラッシュ）
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) > 0) {
        char logFile[MAX_PATH];
        snprintf(logFile, sizeof(logFile), "%sTruckerWalkAround_Live.log", tempPath);
        FILE* fp = fopen(logFile, "a");
        if (fp) {
            fputs(buf, fp);
            fflush(fp);
            fclose(fp);
        }
    }
}


// -----------------------------------------------------------------------------
// SCS Telemetry SDK 定義
// -----------------------------------------------------------------------------
typedef const char* scs_string_t;
typedef uint32_t scs_u32_t;
typedef void* scs_context_t;
typedef uint32_t scs_event_t;

enum scs_value_type_t {
    SCS_VALUE_TYPE_INVALID = 0,
    SCS_VALUE_TYPE_bool = 1,
    SCS_VALUE_TYPE_s32 = 2,
    SCS_VALUE_TYPE_u32 = 3,
    SCS_VALUE_TYPE_u64 = 4,
    SCS_VALUE_TYPE_float = 5,
    SCS_VALUE_TYPE_double = 6,
    SCS_VALUE_TYPE_fvector = 7,
    SCS_VALUE_TYPE_dvector = 8,
    SCS_VALUE_TYPE_euler = 9,
    SCS_VALUE_TYPE_fplacement = 10,
    SCS_VALUE_TYPE_dplacement = 11,
    SCS_VALUE_TYPE_string = 12
};

struct scs_value_dplacement_t {
    struct {
        double x, y, z;
    } position;
    struct {
        float heading, pitch, roll;
    } orientation;
};

struct scs_value_t {
    scs_u32_t type;
    scs_u32_t _pad;
    union {
        bool value_bool;
        float value_float;
        scs_value_dplacement_t value_dplacement;
    };
};

typedef void (*scs_telemetry_channel_callback_t)(const scs_string_t name, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context);
typedef scs_u32_t (*scs_telemetry_register_for_channel_t)(const scs_string_t name, const scs_u32_t index, const scs_u32_t type, const scs_u32_t flags, const scs_telemetry_channel_callback_t callback, const scs_context_t context);

typedef void (*scs_telemetry_event_callback_t)(const scs_event_t event, const void* const event_info, const scs_context_t context);
typedef scs_u32_t (*scs_telemetry_register_for_event_t)(const scs_event_t event, const scs_telemetry_event_callback_t callback, const scs_context_t context);
typedef scs_u32_t (*scs_telemetry_unregister_from_event_t)(const scs_event_t event, const scs_telemetry_event_callback_t callback, const scs_context_t context);

#define SCS_TELEMETRY_EVENT_frame_start 1
#define SCS_TELEMETRY_EVENT_frame_end   2
#define SCS_TELEMETRY_EVENT_paused      3
#define SCS_TELEMETRY_EVENT_started     4

#pragma pack(push, 8)
struct scs_telemetry_init_params_v100_t {
    scs_u32_t sdk_version;
    scs_u32_t _pad1;
    void* log_fn;
    scs_context_t context;
    void* _reserved18;
    scs_telemetry_register_for_event_t register_for_event;
    scs_telemetry_unregister_from_event_t unregister_from_event;
    scs_telemetry_register_for_channel_t register_for_channel;
    void* unregister_from_channel;
};
#pragma pack(pop)

// -----------------------------------------------------------------------------
// 3次元ベクトル構造体
// -----------------------------------------------------------------------------
struct Vec3 {
    float x, y, z;
    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

// -----------------------------------------------------------------------------
// PhysX 3.4 Raycast ABI 定義
// -----------------------------------------------------------------------------
struct DummyRaycastCallbackVtable {
    void* processTouches;
    void* finalizeQuery;
    void* destructor;
};
static bool __fastcall DummyProcessTouches(void*, const void*, uint32_t) { return false; }
static void __fastcall DummyFinalizeQuery(void*) {}
static void* __fastcall DummyDestructor(void* thisPtr, unsigned int) { return thisPtr; }
static DummyRaycastCallbackVtable g_dummyVtable = {
    (void*)DummyProcessTouches,
    (void*)DummyFinalizeQuery,
    (void*)DummyDestructor
};

#pragma pack(push, 1)
struct PxRaycastHitNative {
    void* actor;
    void* shape;
    uint32_t faceIndex;
    uint32_t _pad0;
    uint16_t flags;
    uint16_t _pad1;
    Vec3 position;
    Vec3 normal;
    float distance;
    float u, v;
};

struct PxRaycastBufferNative {
    void* vtable;
    PxRaycastHitNative block;
    uint8_t hasBlock;
    uint8_t _pad2[7];
    void* touches;
    uint32_t maxTouches;
    uint32_t nbTouches;
};

struct PxQueryFilterDataNative {
    uint32_t data[4];
    uint16_t flags; // 1: eSTATIC, 2: eDYNAMIC, 3: BOTH
    uint8_t pad[6];
};
#pragma pack(pop)

typedef bool (__fastcall *FnPxSceneRaycastNative)(
    void* thisScene,
    const Vec3* origin,
    const Vec3* unitDir,
    float distance,
    void* hitCallback,
    const uint16_t* hitFlagsPtr,
    void* queryFilterData,
    void* filterCallback,
    void* queryCache
);

// -----------------------------------------------------------------------------
// Scene Focus (focus2) & Region Query 構造体
// -----------------------------------------------------------------------------
#pragma pack(push, 1)
struct SceneFocusResultNative {
    uint8_t isValid; // +0x00: 1 = valid
    uint8_t _pad[3]; // +0x01 ~ +0x03
    float x;         // +0x04: sector-local X
    float y;         // +0x08: sector-local Y
    float z;         // +0x0C: sector-local Z
    int16_t secX;    // +0x10: sector X
    int16_t secZ;    // +0x12: sector Z
};
#pragma pack(pop)

typedef void* (__fastcall *FnBoxBuilder)(void* unused, void* outBox, const void* coords, const void* extents);
typedef void* (__fastcall *FnRegionQuery)(void* scene, const void* inBox, uint32_t flags);
typedef SceneFocusResultNative* (__fastcall *FnSceneFocusRoutine)(void* scene, SceneFocusResultNative* outResult);

// -----------------------------------------------------------------------------
// グローバル変数
// -----------------------------------------------------------------------------
static uintptr_t g_gameBase = 0;
static HANDLE g_hThread = NULL;
static bool g_running = true;
static PVOID g_pVehHandler = NULL;

// SCS Telemetry 受信座標
static double g_truckWorldX = 0.0;
static double g_truckWorldY = 0.0;
static double g_truckWorldZ = 0.0;
static float  g_truckSpeed  = 0.0f;
static bool   g_hasTruckWorld = false;
static bool   g_gamePaused = false;
static bool   g_parkingBrakeOn = false;

// カメラ構造体
static uintptr_t g_camMgrSingletonPtr = 0;
static uintptr_t g_camMgrAddr = 0;
static uintptr_t g_slotsArrAddr = 0;
static uintptr_t g_debugCamAddr = 0;

// SEH保護メモリ書き込みヘルパー
static inline bool SafeWriteMemory(uintptr_t dest, const void* src, size_t size) {
    if (!dest || !src || size == 0) return false;
    __try {
        memcpy((void*)dest, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <typename T>
static inline bool SafeWrite(uintptr_t dest, const T& val) {
    return SafeWriteMemory(dest, &val, sizeof(T));
}

// 歩行ステート
static bool g_isWalking = false;
static bool g_isNoclip = false;
static bool g_lastAltState = false;
static bool g_lastF9State = false;
static bool g_lastF10State = false;
static bool g_monitorEnabled = false; // デフォルトOFF

static Vec3 g_currentPos;
static Vec3 g_lastSafePos;
static Vec3 g_lastRegionPos(1e9f, 1e9f, 1e9f);
static int16_t g_spawnSectorX = 0;
static int16_t g_spawnSectorZ = 0;
static float g_walkStartTime = 0.0f;

// 物理・シーン解決状態
static uintptr_t g_physxSceneVtable = 0;
static void* g_pVerifiedScene = NULL;
static std::vector<void*> g_sceneCandidates;
static double g_originOffsetX = 0.0;
static double g_originOffsetZ = 0.0;
static bool   g_originOffsetSolved = false;

static float g_cachedGroundY = 0.0f;
static bool  g_groundValid = false;
static bool  g_wallHit = false;
static float g_wallDistance = 0.0f;
static Vec3  g_wallNormal(0.0f, 0.0f, 0.0f);

// focus2 & region_query
static uintptr_t g_sceneGlobalPtr = 0;
static uintptr_t g_focusRoutineTarget = 0;
static uintptr_t g_boxBuilderTarget = 0;
static uintptr_t g_regionQueryTarget = 0;
static void*     g_originalFocusRoutine = nullptr;
static void**    g_hookedVtableSlot = nullptr;
static volatile bool g_focusHookActive = false;
static SceneFocusResultNative g_hookWalkerFocus = { 0 };
static uint32_t g_redirectedFocusCalls = 0;
static uint32_t g_regionQueryCount = 0;

// 設定定数
static const float STANDING_EYE_HEIGHT = 1.75f;
static const float WALK_SPEED = 3.5f;
static const float SPRINT_MULTIPLIER = 2.2f;
static const float VERTICAL_SPEED = 3.0f;
static const float SPAWN_OFFSET_SIDE = 3.5f;

// -----------------------------------------------------------------------------
// 安全なメモリ読み取り関数 (VEH保護付き)
// -----------------------------------------------------------------------------
template <typename T>
static bool SafeRead(uintptr_t address, T* outVal) {
    if (!address || address < 0x10000 || address > 0x7FFFFFFFFFFF) return false;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, outVal, sizeof(T), &bytesRead)) {
        return bytesRead == sizeof(T);
    }
    return false;
}

static bool SafeReadBuffer(uintptr_t address, void* outBuf, size_t size) {
    if (!address || address < 0x10000 || address > 0x7FFFFFFFFFFF) return false;
    SIZE_T bytesRead = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)address, outBuf, size, &bytesRead)) {
        return bytesRead == size;
    }
    return false;
}

// -----------------------------------------------------------------------------
// 最上位ベクタ例外ハンドラ (VEH)
// -----------------------------------------------------------------------------
static LONG WINAPI CrashShieldVectoredHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    DWORD code = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == 0x80000001 /* STATUS_GUARD_PAGE_VIOLATION */) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// -----------------------------------------------------------------------------
// 専用コンソールの作成 (UTF-8 対応)
// -----------------------------------------------------------------------------
static void InitConsole() {
    // コンソールウィンドウは表示せず、ランチャーのリアルタイムログ出力へ一元化
    TruckerLog("[INFO] TruckerWalkAround プラグイン初期化開始\n");
    TruckerLog("[INFO] F9: 歩行モード切替 | 左Alt: Noclip切替 | F10: モニター切替\n");
}

// -----------------------------------------------------------------------------
// SCS Telemetry コールバック
// -----------------------------------------------------------------------------
static void TelemetryPlacementCallback(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t) {
    if (value && value->type == SCS_VALUE_TYPE_dplacement) {
        g_truckWorldX = value->value_dplacement.position.x;
        g_truckWorldY = value->value_dplacement.position.y;
        g_truckWorldZ = value->value_dplacement.position.z;
        g_hasTruckWorld = true;
    }
}

static void TelemetrySpeedCallback(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t) {
    if (value && value->type == SCS_VALUE_TYPE_float) {
        g_truckSpeed = std::abs(value->value_float);
    }
}

static void TelemetryPauseCallback(const scs_event_t event, const void* const event_info, const scs_context_t context) {
    if (event == SCS_TELEMETRY_EVENT_paused) {
        g_gamePaused = true;
        TruckerLog("[telemetry] ゲームポーズ/メニュー画面検知 (SCS_TELEMETRY_EVENT_paused)\n");
    } else if (event == SCS_TELEMETRY_EVENT_started) {
        g_gamePaused = false;
        TruckerLog("[telemetry] 走行状態復帰検知 (SCS_TELEMETRY_EVENT_started)\n");
    }
}

// ゲームウィンドウ上でマウスカーソルが表示されているか判定（メニュー画面検知）
static bool IsInGameMenuCursorVisible() {
    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&ci)) {
        if (ci.flags & CURSOR_SHOWING) {
            return true;
        }
    }
    return false;
}

static void TelemetryParkingBrakeCallback(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t) {
    if (value && value->type == SCS_VALUE_TYPE_bool) {
        g_parkingBrakeOn = (value->value_bool != 0);
    }
}

// -----------------------------------------------------------------------------
// PEセクション走査と文字列参照による NpScene vtable の動的解決 (STAGE 1)
// -----------------------------------------------------------------------------
static uintptr_t FindStringInSection(const uint8_t* secBase, size_t secSize, const char* needle) {
    size_t nLen = strlen(needle);
    if (secSize < nLen) return 0;
    for (size_t i = 0; i <= secSize - nLen; ++i) {
        if (memcmp(secBase + i, needle, nLen) == 0) {
            return (uintptr_t)(secBase + i);
        }
    }
    return 0;
}

static uintptr_t FindFunctionReferencingString(const uint8_t* textBase, size_t textSize, uintptr_t targetStrAddr) {
    if (textSize < 8) return 0;
    for (size_t offset = 2; offset <= textSize - 5; ++offset) {
        const uint8_t* p = textBase + offset;
        uint8_t rex = p[-2];
        if ((rex == 0x48 || rex == 0x4C) && p[-1] == 0x8D && (p[0] & 0xC7) == 0x05) {
            int32_t disp = *(const int32_t*)(p + 1);
            uintptr_t nextRip = (uintptr_t)(p + 5);
            uintptr_t refAddr = (uintptr_t)((intptr_t)nextRip + (intptr_t)disp);
            if (refAddr == targetStrAddr) {
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION rtFunc = RtlLookupFunctionEntry((DWORD64)(p - 2), &imageBase, NULL);
                if (rtFunc && imageBase) {
                    return (uintptr_t)(imageBase + rtFunc->BeginAddress);
                }
            }
        }
    }
    return 0;
}

static void LocatePhysxVtable() {
    if (!g_gameBase) return;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_gameBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_gameBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint16_t numSec = nt->FileHeader.NumberOfSections;

    const uint8_t* rdataBase = nullptr;
    size_t rdataSize = 0;
    const uint8_t* textBase = nullptr;
    size_t textSize = 0;

    for (uint16_t i = 0; i < numSec; ++i) {
        char name[9] = {0};
        memcpy(name, sec[i].Name, 8);
        size_t sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        const uint8_t* base = (const uint8_t*)(g_gameBase + sec[i].VirtualAddress);

        if (strcmp(name, ".rdata") == 0) {
            rdataBase = base;
            rdataSize = sz;
        } else if (strcmp(name, ".text") == 0) {
            textBase = base;
            textSize = sz;
        }
    }

    if (!rdataBase || !textBase) return;

    const char* strReleaseLiteral = "PxScene::release(): Scene is still being simulated";
    const char* strShiftLiteral   = "PxScene::shiftOrigin() not allowed";

    uintptr_t strRelease = FindStringInSection(rdataBase, rdataSize, strReleaseLiteral);
    uintptr_t strShift   = FindStringInSection(rdataBase, rdataSize, strShiftLiteral);
    if (!strRelease || !strShift) return;

    uintptr_t fnRelease = FindFunctionReferencingString(textBase, textSize, strRelease);
    uintptr_t fnShift   = FindFunctionReferencingString(textBase, textSize, strShift);
    if (!fnRelease || !fnShift) return;

    if (rdataSize >= 0x3A8) {
        size_t limit = rdataSize - 0x3A0;
        for (size_t offset = 8; offset <= limit; offset += 8) {
            const uintptr_t* ptr = (const uintptr_t*)(rdataBase + offset);
            if (ptr[0] == fnRelease && *(const uintptr_t*)((uintptr_t)ptr + 0x398) == fnShift) {
                g_physxSceneVtable = (uintptr_t)ptr - 8;
                TruckerLog("[physx] STAGE 1: NpScene vtable 解決成功: 0x%llX\n", (unsigned long long)g_physxSceneVtable);
                return;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// K32QueryWorkingSetEx による非同期ヒープスキャナ (STAGE 2)
// -----------------------------------------------------------------------------
static DWORD WINAPI SceneScannerThread(LPVOID) {
    if (!g_physxSceneVtable) return 0;
    HANDLE hProcess = GetCurrentProcess();

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uintptr_t minAddr = (uintptr_t)sysInfo.lpMinimumApplicationAddress;
    uintptr_t maxAddr = (uintptr_t)sysInfo.lpMaximumApplicationAddress;

    PSAPI_WORKING_SET_EX_INFORMATION* wsInfo = new PSAPI_WORKING_SET_EX_INFORMATION[2048];
    uintptr_t* readBuf = new uintptr_t[512];

    for (int pass = 1; pass <= 8 && g_running; ++pass) {
        std::vector<void*> foundInPass;
        uintptr_t currAddr = minAddr;
        MEMORY_BASIC_INFORMATION mbi;

        while (g_running && currAddr < maxAddr && VirtualQuery((void*)currAddr, &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
                uintptr_t regionBase = (uintptr_t)mbi.BaseAddress;
                size_t totalPages = mbi.RegionSize / 4096;

                for (size_t pageStart = 0; pageStart < totalPages && g_running; pageStart += 2048) {
                    size_t batchPages = totalPages - pageStart;
                    if (batchPages > 2048) batchPages = 2048;

                    for (size_t p = 0; p < batchPages; ++p) {
                        wsInfo[p].VirtualAddress = (PVOID)(regionBase + (pageStart + p) * 4096);
                        wsInfo[p].VirtualAttributes.Flags = 0;
                    }

                    if (K32QueryWorkingSetEx(hProcess, wsInfo, (DWORD)(batchPages * sizeof(PSAPI_WORKING_SET_EX_INFORMATION)))) {
                        for (size_t p = 0; p < batchPages; ++p) {
                            if (wsInfo[p].VirtualAttributes.Valid) {
                                uintptr_t pageAddr = (uintptr_t)wsInfo[p].VirtualAddress;
                                if (SafeReadBuffer(pageAddr, readBuf, 4096)) {
                                    size_t words = 4096 / sizeof(uintptr_t);
                                    for (size_t w = 0; w < words; ++w) {
                                        if (readBuf[w] == g_physxSceneVtable) {
                                            void* cand = (void*)(pageAddr + w * sizeof(uintptr_t));
                                            foundInPass.push_back(cand);
                                            if (foundInPass.size() >= 16) break;
                                        }
                                    }
                                }
                            }
                            if (foundInPass.size() >= 16) break;
                        }
                    }
                    if (foundInPass.size() >= 16) break;
                }
            }
            if (foundInPass.size() >= 16) break;
            uintptr_t nextAddr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (nextAddr <= currAddr) break;
            currAddr = nextAddr;
        }

        if (!foundInPass.empty()) {
            g_sceneCandidates = foundInPass;
            TruckerLog("[physx] STAGE 2: ヒープスキャン パス %d で %zu 個のシーン候補を発見\n", pass, foundInPass.size());
            break;
        }

        if (pass < 8 && g_running) Sleep(4000);
    }

    delete[] wsInfo;
    delete[] readBuf;
    return 0;
}

// -----------------------------------------------------------------------------
// PhysX Raycast (vtable +0x2F0) 実行関数
// -----------------------------------------------------------------------------
static bool TMRealWalkRaycastSafe(
    void* scene,
    const Vec3& origin,
    const Vec3& unitDir,
    float maxDist,
    PxRaycastBufferNative* hitCallback,
    uint16_t hitFlags,
    PxQueryFilterDataNative* queryFilterData)
{
    if (!scene) return false;

    uintptr_t vtable = 0;
    if (!SafeRead((uintptr_t)scene, &vtable) || !vtable) return false;

    uintptr_t fnRaycastPtr = 0;
    if (!SafeRead(vtable + 0x2F0, &fnRaycastPtr) || !fnRaycastPtr) return false;

    FnPxSceneRaycastNative fnRaycast = (FnPxSceneRaycastNative)fnRaycastPtr;
    uint16_t flags = hitFlags;

    __try {
        return fnRaycast(scene, &origin, &unitDir, maxDist, hitCallback, &flags, queryFilterData, NULL, NULL);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// -----------------------------------------------------------------------------
// Telemetry と RigidDynamic による原点オフセット確定 (STAGE 4)
// -----------------------------------------------------------------------------
static bool SolveOriginOffsetViaTelemetry(void* scene, double truckX, double truckY, double truckZ) {
    if (!scene) return false;

    typedef uint32_t (__fastcall *GetNbActorsFn)(void* thisScene, const uint16_t* typesPtr);
    typedef uint32_t (__fastcall *GetActorsFn)(void* thisScene, const uint16_t* typesPtr, void** buffer, uint32_t bufSize, uint32_t startIndex);
    typedef float*   (__fastcall *GetGlobalPoseFn)(void* thisActor, void* outPoseBuf);

    uintptr_t vtableAddr = 0;
    if (!SafeRead((uintptr_t)scene, &vtableAddr) || !vtableAddr) return false;

    uintptr_t fnGetNbActorsAddr = 0;
    uintptr_t fnGetActorsAddr = 0;
    SafeRead(vtableAddr + 0x90, &fnGetNbActorsAddr);
    SafeRead(vtableAddr + 0x98, &fnGetActorsAddr);
    if (!fnGetNbActorsAddr || !fnGetActorsAddr) return false;

    GetNbActorsFn getNbActors = (GetNbActorsFn)fnGetNbActorsAddr;
    GetActorsFn getActors = (GetActorsFn)fnGetActorsAddr;

    uint16_t typeFlags = 2; // eRIGID_DYNAMIC
    uint32_t nbDynamic = 0;
    __try {
        nbDynamic = getNbActors(scene, &typeFlags);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (nbDynamic == 0) return false;
    if (nbDynamic > 1024) nbDynamic = 1024;

    void* actorsBuffer[256];
    double bestRem = 6.0;
    double bestOffX = 0.0;
    double bestOffZ = 0.0;
    bool solved = false;

    for (uint32_t startIdx = 0; startIdx < nbDynamic; startIdx += 256) {
        uint32_t batchCount = ((nbDynamic - startIdx) < 256) ? (nbDynamic - startIdx) : 256;
        uint32_t retrieved = 0;
        __try {
            retrieved = getActors(scene, &typeFlags, actorsBuffer, batchCount, startIdx);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }

        for (uint32_t i = 0; i < retrieved; ++i) {
            void* actor = actorsBuffer[i];
            if (!actor) continue;

            uintptr_t aVtable = 0;
            if (!SafeRead((uintptr_t)actor, &aVtable) || !aVtable) continue;

            uintptr_t fnGetPose = 0;
            if (!SafeRead(aVtable + 0xA0, &fnGetPose) || !fnGetPose) continue;

            GetGlobalPoseFn getGlobalPose = (GetGlobalPoseFn)fnGetPose;
            alignas(16) float poseBuf[16] = {0};
            float* ret = getGlobalPose(actor, poseBuf);
            if (!ret) ret = poseBuf;

            float px = ret[4], py = ret[5], pz = ret[6];
            if (std::isnan(px) || std::isnan(py) || std::isnan(pz)) continue;
            if (std::abs((double)py - truckY) > 6.0) continue;

            double diffX = truckX - (double)px;
            double diffZ = truckZ - (double)pz;
            double testOffX = std::round(diffX * (1.0 / 512.0)) * 512.0;
            double testOffZ = std::round(diffZ * (1.0 / 512.0)) * 512.0;
            double rem = std::abs(diffX - testOffX) + std::abs(diffZ - testOffZ);

            if (rem < bestRem) {
                bestRem = rem;
                bestOffX = testOffX;
                bestOffZ = testOffZ;
                solved = true;
            }
        }
    }

    if (solved) {
        bool changed = (!g_originOffsetSolved || bestOffX != g_originOffsetX || bestOffZ != g_originOffsetZ);
        g_originOffsetX = bestOffX;
        g_originOffsetZ = bestOffZ;
        if (changed) {
            TruckerLog("[physx] STAGE 4: 原点オフセット確定 = (%.0f, %.0f) [残差: %.2fm]\n", bestOffX, bestOffZ, bestRem);
        }
        g_originOffsetSolved = true;
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// 【精巧化】単一地点の地面 Raycast (静的地形 + 動的オブジェクト)
// -----------------------------------------------------------------------------
static bool ExecuteGroundRaycastSingle(void* scene, float sx, float sz, float startY, float* outGroundY) {
    Vec3 origin(sx, startY, sz);
    Vec3 unitDir(0.0f, -1.0f, 0.0f);

    float bestGroundY = -1e9f;
    bool found = false;

    // 1. 静的地形 (eSTATIC: flags = 1)
    {
        PxRaycastBufferNative buf;
        memset(&buf, 0, sizeof(buf));
        buf.vtable = &g_dummyVtable;
        buf.block.faceIndex = 0xFFFFFFFF;
        buf.block.distance = 1e30f;

        PxQueryFilterDataNative filterData;
        memset(&filterData, 0, sizeof(filterData));
        filterData.flags = 1;

        bool hit = TMRealWalkRaycastSafe(scene, origin, unitDir, 2.5f, &buf, 0x0083, &filterData);
        if (hit && buf.hasBlock && std::abs(buf.block.normal.y) >= 0.30f) {
            bestGroundY = buf.block.position.y;
            found = true;
        }
    }

    // 2. 動的ボディ (eDYNAMIC: flags = 2)
    {
        PxRaycastBufferNative buf2;
        memset(&buf2, 0, sizeof(buf2));
        buf2.vtable = &g_dummyVtable;
        buf2.block.faceIndex = 0xFFFFFFFF;
        buf2.block.distance = 1e30f;

        PxQueryFilterDataNative filterData2;
        memset(&filterData2, 0, sizeof(filterData2));
        filterData2.flags = 2;

        bool hit2 = TMRealWalkRaycastSafe(scene, origin, unitDir, 2.5f, &buf2, 0x0083, &filterData2);
        if (hit2 && buf2.hasBlock && buf2.block.distance >= 0.02f && std::abs(buf2.block.normal.y) >= 0.50f) {
            if (!found || buf2.block.position.y > bestGroundY) {
                bestGroundY = buf2.block.position.y;
                found = true;
            }
        }
    }

    if (found) {
        if (outGroundY) *outGroundY = bestGroundY;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// 【精巧化】5点クロスサンプリング付き 地面 Raycast
// -----------------------------------------------------------------------------
static bool ExecuteGroundRaycast5Points(void* scene, float sx, float sz, float startY, float* outGroundY) {
    if (!scene) return false;

    const float sampleOffsets[5][2] = {
        {  0.00f,  0.00f },
        {  0.25f,  0.00f },
        { -0.25f,  0.00f },
        {  0.00f,  0.25f },
        {  0.00f, -0.25f }
    };

    float bestY = -1e9f;
    bool anyHit = false;

    for (int i = 0; i < 5; ++i) {
        float curY = 0.0f;
        if (ExecuteGroundRaycastSingle(scene, sx + sampleOffsets[i][0], sz + sampleOffsets[i][1], startY, &curY)) {
            if (!anyHit || curY > bestY) {
                bestY = curY;
                anyHit = true;
            }
        }
    }

    if (anyHit) {
        if (outGroundY) *outGroundY = bestY;
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// 【安全分離版】障害物判定 Raycast (静的 flags=1 と 動的 flags=2 を完全分離)
// -----------------------------------------------------------------------------
static bool ExecuteObstacleRaycast(void* scene, float sx, float sz, float startY, float dirX, float dirZ, float checkDist, float* outDist, Vec3* outNormal) {
    if (!scene) return false;

    Vec3 origin(sx, startY, sz);
    Vec3 unitDir(dirX, 0.0f, dirZ);

    float bestDist = 1e30f;
    Vec3 bestNormal(0.0f, 0.0f, 0.0f);
    bool hitFound = false;

    // 1. 静的オブジェクト (flags = 1: 壁・建物・ガードレール等)
    {
        PxRaycastBufferNative bufStatic;
        memset(&bufStatic, 0, sizeof(bufStatic));
        bufStatic.vtable = &g_dummyVtable;
        bufStatic.block.faceIndex = 0xFFFFFFFF;
        bufStatic.block.distance = 1e30f;

        PxQueryFilterDataNative filterStatic;
        memset(&filterStatic, 0, sizeof(filterStatic));
        filterStatic.flags = 1; // eSTATIC

        bool hit = TMRealWalkRaycastSafe(scene, origin, unitDir, checkDist, &bufStatic, 0x0083, &filterStatic);
        if (hit && bufStatic.hasBlock) {
            bestDist = bufStatic.block.distance;
            bestNormal = bufStatic.block.normal;
            hitFound = true;
        }
    }

    // 2. 動的オブジェクト (flags = 2: トラック・トレーラー・他車等)
    {
        PxRaycastBufferNative bufDynamic;
        memset(&bufDynamic, 0, sizeof(bufDynamic));
        bufDynamic.vtable = &g_dummyVtable;
        bufDynamic.block.faceIndex = 0xFFFFFFFF;
        bufDynamic.block.distance = 1e30f;

        PxQueryFilterDataNative filterDynamic;
        memset(&filterDynamic, 0, sizeof(filterDynamic));
        filterDynamic.flags = 2; // eDYNAMIC

        bool hit2 = TMRealWalkRaycastSafe(scene, origin, unitDir, checkDist, &bufDynamic, 0x0083, &filterDynamic);
        if (hit2 && bufDynamic.hasBlock) {
            if (!hitFound || bufDynamic.block.distance < bestDist) {
                bestDist = bufDynamic.block.distance;
                bestNormal = bufDynamic.block.normal;
                hitFound = true;
            }
        }
    }

    if (hitFound) {
        if (outDist) *outDist = bestDist;
        if (outNormal) *outNormal = bestNormal;
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// camera_manager 動的特定
// -----------------------------------------------------------------------------
static bool LocateCameraManager() {
    if (!g_gameBase) return false;

    IMAGE_DOS_HEADER dos;
    if (!SafeRead(g_gameBase, &dos)) return false;

    IMAGE_NT_HEADERS nt;
    if (!SafeRead(g_gameBase + dos.e_lfanew, &nt)) return false;

    uintptr_t secStart = g_gameBase + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    uint16_t numSec = nt.FileHeader.NumberOfSections;

    uintptr_t camMgrVa = 0;
    for (int i = 0; i < numSec; ++i) {
        IMAGE_SECTION_HEADER sec;
        if (!SafeRead(secStart + i * sizeof(IMAGE_SECTION_HEADER), &sec)) continue;
        if (strncmp((char*)sec.Name, ".rdata", 6) == 0) {
            uintptr_t start = g_gameBase + sec.VirtualAddress;
            size_t size = sec.Misc.VirtualSize;
            for (size_t k = 0; k < size - 16; k += 4) {
                char buf[16];
                if (SafeReadBuffer(start + k, buf, 15)) {
                    if (memcmp(buf, "camera_manager\0", 15) == 0) {
                        camMgrVa = start + k;
                        break;
                    }
                }
            }
            break;
        }
    }
    if (!camMgrVa) return false;

    uintptr_t descVa = 0;
    for (int i = 0; i < numSec; ++i) {
        IMAGE_SECTION_HEADER sec;
        if (!SafeRead(secStart + i * sizeof(IMAGE_SECTION_HEADER), &sec)) continue;
        if (strncmp((char*)sec.Name, ".data", 5) == 0) {
            uintptr_t start = g_gameBase + sec.VirtualAddress;
            size_t count = sec.Misc.VirtualSize / sizeof(uintptr_t);
            for (size_t k = 0; k < count; ++k) {
                uintptr_t val = 0;
                if (SafeRead(start + k * sizeof(uintptr_t), &val) && val == camMgrVa) {
                    descVa = start + k * sizeof(uintptr_t);
                    break;
                }
            }
            break;
        }
    }
    if (!descVa) return false;

    uintptr_t creatorDesc = 0;
    if (!SafeRead(descVa + 8, &creatorDesc) || !creatorDesc) return false;

    uintptr_t creatorFunc = 0;
    if (!SafeRead(creatorDesc + 8, &creatorFunc) || !creatorFunc) return false;

    uint8_t code[0x150];
    if (!SafeReadBuffer(creatorFunc, code, sizeof(code))) return false;

    uintptr_t singletonPtr = 0;
    for (int i = 0; i < 0x150 - 7; ++i) {
        if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x89 && (code[i + 2] & 0xC7) == 0x05) {
            int32_t disp = *(int32_t*)&code[i + 3];
            singletonPtr = creatorFunc + i + 7 + disp;
            break;
        }
    }
    if (!singletonPtr) return false;
    g_camMgrSingletonPtr = singletonPtr;

    uintptr_t mgrInst = 0;
    if (!SafeRead(singletonPtr, &mgrInst) || !mgrInst) return false;
    g_camMgrAddr = mgrInst;

    uintptr_t slotsArr = 0;
    if (!SafeRead(g_camMgrAddr + 0x38, &slotsArr) || !slotsArr) return false;
    g_slotsArrAddr = slotsArr;

    uintptr_t debugCam = 0;
    if (SafeRead(g_slotsArrAddr, &debugCam) && debugCam > 0x10000) {
        g_debugCamAddr = debugCam;
        TruckerLog("[OK] camera_manager を解決: 0x%llX | Slots: 0x%llX\n", (unsigned long long)g_camMgrAddr, (unsigned long long)g_slotsArrAddr);
        return true;
    }

    return false;
}

// F9押下時専用: 3回のSafeReadで最新の動的カメラ構造体を高速更新 (0.001ms)
static bool RefreshDynamicCameraOnSpawn() {
    if (!g_camMgrSingletonPtr) {
        if (!LocateCameraManager()) return false;
    }
    uintptr_t mgrInst = 0;
    if (!SafeRead(g_camMgrSingletonPtr, &mgrInst) || !mgrInst) return false;
    g_camMgrAddr = mgrInst;

    uintptr_t slotsArr = 0;
    if (!SafeRead(g_camMgrAddr + 0x38, &slotsArr) || !slotsArr) return false;
    g_slotsArrAddr = slotsArr;

    uintptr_t debugCam = 0;
    if (SafeRead(g_slotsArrAddr, &debugCam) && debugCam > 0x10000) {
        g_debugCamAddr = debugCam;
    }
    return (g_slotsArrAddr != 0);
}

static uintptr_t GetCameraObjectBySlot(uint32_t slot) {
    if (!g_slotsArrAddr && !RefreshDynamicCameraOnSpawn()) return 0;
    if (slot >= 16) return 0;
    uintptr_t camObj = 0;
    if (SafeRead(g_slotsArrAddr + slot * sizeof(uintptr_t), &camObj) && camObj > 0x10000) {
        return camObj;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// focus2 & region_query ターゲット解決 (ATS 1.62.1.1 確定アドレス直接バインド)
// -----------------------------------------------------------------------------
static void LocateSceneFocusModule() {
    // [ATS 1.62.1.1]
    if (!g_gameBase) return;

    // ATS 1.62.1.1 確定アドレスを直接設定 (機械語解析により 0x143719CA8 と確定)
    g_focusRoutineTarget = g_gameBase + 0x496340;
    g_sceneGlobalPtr     = g_gameBase + 0x3719CA8;
    g_boxBuilderTarget   = g_gameBase + 0x478040;
    g_regionQueryTarget  = g_gameBase + 0x4949E0;

    TruckerLog("[focus2] ターゲット解決完了 (focus_routine: +0x%llX, region_query: +0x%llX, scene_global: +0x%llX)\n",
           (unsigned long long)(g_focusRoutineTarget - g_gameBase),
           (unsigned long long)(g_regionQueryTarget - g_gameBase),
           (unsigned long long)(g_sceneGlobalPtr - g_gameBase));
}

// -----------------------------------------------------------------------------
// focus2 フック関数 (Hooked_SceneFocusRoutine)
// -----------------------------------------------------------------------------
static SceneFocusResultNative* __fastcall Hooked_SceneFocusRoutine(void* scene, SceneFocusResultNative* outResult) {
    if (g_focusHookActive && outResult) {
        *outResult = g_hookWalkerFocus;
        outResult->isValid = 1;
        InterlockedIncrement((volatile LONG*)&g_redirectedFocusCalls);
        return outResult;
    }
    if (g_originalFocusRoutine) {
        return ((FnSceneFocusRoutine)g_originalFocusRoutine)(scene, outResult);
    }
    if (outResult) {
        memset(outResult, 0, sizeof(SceneFocusResultNative));
    }
    return outResult;
}

static bool EnableSceneFocusHook() {
    if (!g_sceneGlobalPtr || !g_focusRoutineTarget) return false;
    uintptr_t scene = 0;
    if (!SafeRead(g_sceneGlobalPtr, &scene) || !scene) return false;

    uintptr_t vtable = 0;
    if (!SafeRead(scene, &vtable) || !vtable) return false;

    void** slot = (void**)(vtable + 0x178);
    void* currentRoutine = nullptr;
    if (!SafeRead((uintptr_t)slot, &currentRoutine)) return false;

    if (currentRoutine == (void*)Hooked_SceneFocusRoutine) {
        g_focusHookActive = true;
        return true;
    }

    if ((uintptr_t)currentRoutine != g_focusRoutineTarget) return false;

    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        g_originalFocusRoutine = *slot;
        *slot = (void*)Hooked_SceneFocusRoutine;
        VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

        g_hookedVtableSlot = slot;
        g_focusHookActive = true;
        g_redirectedFocusCalls = 0;
        TruckerLog("[focus2] エンジンフォーカスを歩行者へリダイレクト開始\n");
        return true;
    }

    return false;
}

static void DisableSceneFocusHook() {
    g_focusHookActive = false;
    if (g_hookedVtableSlot && g_originalFocusRoutine) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_hookedVtableSlot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            if (*g_hookedVtableSlot == (void*)Hooked_SceneFocusRoutine) {
                *g_hookedVtableSlot = g_originalFocusRoutine;
            }
            VirtualProtect(g_hookedVtableSlot, sizeof(void*), oldProtect, &oldProtect);
        }
        TruckerLog("[focus2] エンジンフォーカスをトラックへ復帰 (リダイレクト回数: %u)\n", g_redirectedFocusCalls);
        g_hookedVtableSlot = nullptr;
        g_originalFocusRoutine = nullptr;
    }
}

// -----------------------------------------------------------------------------
// 能動的コライダー強制ロード (1000ms毎に15mボックスを発行)
// -----------------------------------------------------------------------------
static bool ExecuteRegionAroundWalker(float boxSize = 15.0f) {
    if (!g_sceneGlobalPtr || !g_boxBuilderTarget || !g_regionQueryTarget) return false;
    uintptr_t scene = 0;
    if (!SafeRead(g_sceneGlobalPtr, &scene) || !scene) return false;

    alignas(16) uint8_t queryBox[0x40] = { 0 };

    struct WalkerCoords {
        float x, y, z;
        int16_t secX, secZ;
    } coords = {
        g_hookWalkerFocus.x,
        g_hookWalkerFocus.y,
        g_hookWalkerFocus.z,
        g_hookWalkerFocus.secX,
        g_hookWalkerFocus.secZ
    };

    struct BoxExtents {
        float halfX, halfY, halfZ;
    } extents = { boxSize, boxSize, boxSize };

    FnBoxBuilder fnBox = (FnBoxBuilder)g_boxBuilderTarget;
    FnRegionQuery fnQuery = (FnRegionQuery)g_regionQueryTarget;

    __try {
        fnBox(NULL, queryBox, &coords, &extents);
        fnQuery((void*)scene, queryBox, 0);
        g_regionQueryCount++;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// -----------------------------------------------------------------------------
// カメラスロット切替
// -----------------------------------------------------------------------------
static void SwitchCameraSlot(int slot) {
    if (g_camMgrAddr) {
        *(int32_t*)(g_camMgrAddr + 0x14) = slot;
    }
}

static uint32_t GetActiveCameraSlot() {
    if (g_camMgrAddr) {
        return *(uint32_t*)(g_camMgrAddr + 0x10);
    }
    return 2;
}

// =============================================================================
// 1. 車両操作系 (Space / WASD 等) の入力保持メモリ・ブランク化システム
// =============================================================================
struct SavedMixNode {
    uintptr_t itemAddr;
    uintptr_t originalAstNode;
    char      name[32];
};

static uintptr_t* g_mixTableBase = nullptr;
static uint64_t*  g_mixCountPtr  = nullptr;
static bool       g_mixLocked    = false;
static std::vector<SavedMixNode> g_savedMixNodes;

static void DecodePrismToken(uint64_t token, char* outStr, size_t maxLen) {
    static const char kCharset[38] = {
        '\0',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
        'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
        'u', 'v', 'w', 'x', 'y', 'z', '_'
    };
    if (!outStr || maxLen == 0) return;
    size_t pos = 0;
    while (token > 0 && pos + 1 < maxLen) {
        uint64_t rem = token % 38ULL;
        token /= 38ULL;
        if (rem == 0) break;
        outStr[pos++] = kCharset[rem];
    }
    outStr[pos] = '\0';
}

static bool IsTargetVehicleControlMixName(const char* name) {
    if (!name || !name[0]) return false;
    static const char* const kTargetNames[] = {
        // ハンドル (A / D / ステアリング軸)
        "steering", "dsteering", "dsteerleft", "dsteerright",
        "steer_left", "steer_right", "steerleft", "steerright",
        "left", "right",
        // アクセル・ブレーキ (W / S / ペダル軸)
        "aforward", "abackward", "dforward", "dbackward",
        "forward", "backward", "throttle", "brake",
        // ハンドブレーキ (Space) / 補助ブレーキ
        "parkingbrake", "motorbrake", "exhaustbrake",
        // 歩行中の誤操作防止
        "engine", "activate", "trailer"
    };
    for (size_t i = 0; i < sizeof(kTargetNames) / sizeof(kTargetNames[0]); ++i) {
        if (strcmp(name, kTargetNames[i]) == 0) {
            return true;
        }
    }
    return false;
}

static uintptr_t FindPatternInMemory(uintptr_t start, size_t size, const uint8_t* pattern, const char* mask) {
    size_t maskLen = strlen(mask);
    if (size < maskLen) return 0;
    for (size_t i = 0; i <= size - maskLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < maskLen; ++j) {
            if (mask[j] != '?' && *(const uint8_t*)(start + i + j) != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return start + i;
    }
    return 0;
}

static void LocateInputMixModule() {
    if (!g_gameBase) return;

    // amtrucks.exe 内の Mix テーブル参照パターン:
    // 48 8B 05 ?? ?? ?? ?? 4A 8B 1C F0 8B 83 8C
    const uint8_t pat[] = {
        0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x4A, 0x8B, 0x1C, 0xF0, 0x8B, 0x83, 0x8C
    };
    const char mask[] = "xxx????xxxxxx";

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_gameBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_gameBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    uintptr_t textStart = g_gameBase + 0x1000;
    size_t textSize = (size_t)nt->OptionalHeader.SizeOfCode;

    uintptr_t match = FindPatternInMemory(textStart, textSize, pat, mask);
    if (match) {
        int32_t disp = *(int32_t*)(match + 3);
        uintptr_t mixTableAddr = match + 7 + disp;
        g_mixTableBase = (uintptr_t*)mixTableAddr;
        g_mixCountPtr  = (uint64_t*)(mixTableAddr + 8);
        TruckerLog("[MixInput] Mix入力テーブル解決成功: Table=+0x%llX, Count=+0x%llX\n",
               (unsigned long long)(mixTableAddr - g_gameBase),
               (unsigned long long)(mixTableAddr + 8 - g_gameBase));
    } else {
        g_mixTableBase = (uintptr_t*)(g_gameBase + 0x27A1818);
        g_mixCountPtr  = (uint64_t*)(g_gameBase + 0x27A1820);
        TruckerLog("[MixInput] フォールバックアドレスを使用: +0x27A1818\n");
    }
}

static void LockVehicleInput() {
    if (!g_mixTableBase || !g_mixCountPtr) {
        LocateInputMixModule();
    }
    if (g_mixLocked || !g_mixTableBase || !g_mixCountPtr) return;

    uintptr_t table = 0;
    uint64_t count = 0;
    if (!SafeRead((uintptr_t)g_mixTableBase, &table) || !SafeRead((uintptr_t)g_mixCountPtr, &count)) return;
    if (!table || count == 0 || count > 5000) return;

    g_savedMixNodes.clear();

    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t item = 0;
        if (!SafeRead(table + i * sizeof(uintptr_t), &item) || !item) continue;

        uint64_t token = 0;
        if (!SafeRead(item + 0x08, &token) || token == 0) continue;

        char mixName[32] = { 0 };
        DecodePrismToken(token, mixName, sizeof(mixName));

        if (IsTargetVehicleControlMixName(mixName)) {
            uintptr_t origAst = 0;
            SafeRead(item + 0x98, &origAst);

            SavedMixNode entry = {};
            entry.itemAddr = item;
            entry.originalAstNode = origAst;
            strncpy_s(entry.name, mixName, _TRUNCATE);
            g_savedMixNodes.push_back(entry);

            // 入力評価ASTポインタ (+0x98) をブランク (0) にし、現在値メモリ (+0x50) も 0.0f にクリア
            SafeWrite<uintptr_t>(item + 0x98, 0);
            SafeWrite<float>(item + 0x50, 0.0f);
        }
    }

    g_mixLocked = true;
    TruckerLog("[MixInput] 車両操作系 %zu 項目の入力保持メモリをブランク化しました\n", g_savedMixNodes.size());
}

// 歩行モード中に毎フレーム呼び出し、対象項目の入力保持メモリ (+0x50, +0x98) を 0 に固定
static inline void UpdateLockedVehicleInputEveryFrame() {
    if (!g_mixLocked || g_savedMixNodes.empty()) return;
    __try {
        const size_t n = g_savedMixNodes.size();
        const SavedMixNode* nodes = g_savedMixNodes.data();
        for (size_t i = 0; i < n; ++i) {
            uintptr_t item = nodes[i].itemAddr;
            if (item) {
                *(uintptr_t*)(item + 0x98) = 0;
                *(float*)(item + 0x50) = 0.0f;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void UnlockVehicleInput() {
    if (!g_mixLocked) return;

    for (size_t i = 0; i < g_savedMixNodes.size(); ++i) {
        const SavedMixNode& entry = g_savedMixNodes[i];
        if (entry.itemAddr) {
            if (entry.originalAstNode != 0) {
                SafeWrite<uintptr_t>(entry.itemAddr + 0x98, entry.originalAstNode);
            }
            SafeWrite<float>(entry.itemAddr + 0x50, 0.0f);
        }
    }

    TruckerLog("[MixInput] 車両操作系 %zu 項目の入力保持メモリを復元しました\n", g_savedMixNodes.size());
    g_savedMixNodes.clear();
    g_mixLocked = false;
}

// =============================================================================
// 2. ウィジェット (Route Advisor) & 通知 (HUD Notifications) 非表示システム
// =============================================================================
typedef void (__fastcall *FnCvarSetString)(void* cvarThis, const char* valueStr);

static uintptr_t g_cvarAdviserKeepHidden = 0;
static uintptr_t g_cvarHudNotificationsKeepHidden = 0;
static bool      g_cvarLocatorRun = false;
static bool      g_hudHiddenActive = false;
static bool      g_savedAdviserHidden = false;
static bool      g_savedNotificationsHidden = false;

static bool SafeCheckInlineCvarName(uintptr_t cvarCandidate, const char* cvarName, size_t nameLen, uintptr_t rdataStart, uintptr_t rdataEnd) {
    __try {
        uintptr_t vtable = *(const uintptr_t*)cvarCandidate;
        if (vtable >= rdataStart && vtable < rdataEnd) {
            const char* inlineName = (const char*)(cvarCandidate + 8);
            if (strncmp(inlineName, cvarName, nameLen) == 0) {
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static uintptr_t FindCvarObjectByName(const char* cvarName) {
    if (!g_gameBase || !cvarName || !cvarName[0]) return 0;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)g_gameBase;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(g_gameBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    uint16_t numSec = nt->FileHeader.NumberOfSections;

    const uint8_t* rdataBase = nullptr;
    size_t rdataSize = 0;
    const uint8_t* textBase = nullptr;
    size_t textSize = 0;
    const uint8_t* dataBase = nullptr;
    size_t dataSize = 0;

    for (uint16_t i = 0; i < numSec; ++i) {
        char name[9] = { 0 };
        memcpy(name, sec[i].Name, 8);
        if (strcmp(name, ".rdata") == 0) {
            rdataBase = (const uint8_t*)(g_gameBase + sec[i].VirtualAddress);
            rdataSize = sec[i].Misc.VirtualSize;
        } else if (strcmp(name, ".text") == 0) {
            textBase = (const uint8_t*)(g_gameBase + sec[i].VirtualAddress);
            textSize = sec[i].Misc.VirtualSize;
        } else if (strcmp(name, ".data") == 0) {
            dataBase = (const uint8_t*)(g_gameBase + sec[i].VirtualAddress);
            dataSize = sec[i].Misc.VirtualSize;
        }
    }
    if (!rdataBase || !textBase || rdataSize < 16 || textSize < 64) return 0;

    uintptr_t rdataStart = (uintptr_t)rdataBase;
    uintptr_t rdataEnd = rdataStart + rdataSize;

    size_t nameLen = strlen(cvarName);
    std::vector<uintptr_t> strAddrs;
    for (size_t i = 1; i + nameLen + 1 <= rdataSize; ++i) {
        if (rdataBase[i - 1] == 0 &&
            rdataBase[i + nameLen] == 0 &&
            memcmp(rdataBase + i, cvarName, nameLen) == 0) {
            strAddrs.push_back((uintptr_t)(rdataBase + i));
        }
    }

    // Method 1: Scan .text for:
    // 4C 8D 05 <disp32> (lea r8, [rip+disp32] -> cvar name in .rdata)
    // and preceding 7..32 bytes:
    // 48 8D 05 <disp32> (lea rax, [rip+disp32] -> cvar_ptr + 8 in amtrucks)
    // or 48 8D 0D <disp32> (lea rcx, [rip+disp32] -> cvar_ptr)
    if (!strAddrs.empty()) {
        for (size_t offset = 32; offset + 7 <= textSize; ++offset) {
            const uint8_t* p = textBase + offset;
            if (p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x05) {
                int32_t dispR8 = *(const int32_t*)(p + 3);
                uintptr_t refStr = (uintptr_t)(p + 7) + (intptr_t)dispR8;

                bool matchStr = false;
                for (uintptr_t s : strAddrs) {
                    if (s == refStr) {
                        matchStr = true;
                        break;
                    }
                }
                if (!matchStr) continue;

                for (size_t back = 7; back <= 32 && back <= offset; ++back) {
                    const uint8_t* q = p - back;
                    if (q[0] == 0x48 && q[1] == 0x8D && (q[2] == 0x05 || q[2] == 0x0D)) {
                        int32_t dispTarget = *(const int32_t*)(q + 3);
                        uintptr_t targetAddr = (uintptr_t)(q + 7) + (intptr_t)dispTarget;
                        if (targetAddr > g_gameBase) {
                            uintptr_t cvarCandidate = (q[2] == 0x05) ? (targetAddr - 8) : targetAddr;
                            if (SafeCheckInlineCvarName(cvarCandidate, cvarName, nameLen, rdataStart, rdataEnd)) {
                                return cvarCandidate;
                            }
                        }
                    }
                }
            }
        }
    }

    // Method 2: Direct .data fallback scan
    if (dataBase && dataSize >= 0x40) {
        uintptr_t dataStart = (uintptr_t)dataBase;
        uintptr_t dataEnd = dataStart + dataSize;
        for (uintptr_t cur = dataStart; cur + 0x40 <= dataEnd; cur += 8) {
            if (SafeCheckInlineCvarName(cur, cvarName, nameLen + 1, rdataStart, rdataEnd)) {
                return cur;
            }
        }
    }

    return 0;
}

static bool ReadCvarBoolValue(uintptr_t cvarAddr, bool* outVal) {
    if (!cvarAddr || !outVal) return false;
    uintptr_t node = cvarAddr;
    for (int depth = 0; depth < 8; ++depth) {
        uintptr_t nextNode = 0;
        if (!SafeRead(node + 0x120, &nextNode) || !nextNode) break;
        node = nextNode;
    }
    uint8_t ssoFlag = 0;
    if (!SafeRead(node + 0xA1, &ssoFlag)) return false;

    uintptr_t strAddr = 0;
    if (ssoFlag != 0) {
        if (!SafeRead(node + 0x28, &strAddr) || !strAddr) return false;
    } else {
        strAddr = node + 0xB1;
    }

    char buf[32] = { 0 };
    for (size_t i = 0; i < sizeof(buf) - 1; ++i) {
        char c = 0;
        if (!SafeRead(strAddr + i, &c)) break;
        buf[i] = c;
        if (c == '\0') break;
    }
    if (buf[0] == '\0') return false;

    char* endPtr = nullptr;
    double v = strtod(buf, &endPtr);
    if (endPtr == buf) return false;
    *outVal = (std::abs(v) > 1e-6);
    return true;
}

static bool WriteCvarStringValue(uintptr_t cvarAddr, const char* valStr) {
    if (!cvarAddr || !valStr) return false;
    uintptr_t vtable = 0;
    if (!SafeRead(cvarAddr, &vtable) || !vtable) return false;
    uintptr_t setFnAddr = 0;
    if (!SafeRead(vtable + 0x08, &setFnAddr) || !setFnAddr) return false;

    __try {
        FnCvarSetString fnSet = (FnCvarSetString)setFnAddr;
        fnSet((void*)cvarAddr, valStr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LocateHudCvars() {
    if (g_cvarLocatorRun) return;
    g_cvarLocatorRun = true;

    g_cvarAdviserKeepHidden = FindCvarObjectByName("g_adviser_keep_hidden");
    g_cvarHudNotificationsKeepHidden = FindCvarObjectByName("g_hud_notifications_keep_hidden");

    TruckerLog("[HUD] CVar解決: g_adviser_keep_hidden=0x%llX, g_hud_notifications_keep_hidden=0x%llX\n",
           (unsigned long long)g_cvarAdviserKeepHidden,
           (unsigned long long)g_cvarHudNotificationsKeepHidden);
}

static void HideHudWidgetsAndNotifications() {
    if (!g_cvarLocatorRun) {
        LocateHudCvars();
    }
    if (g_hudHiddenActive) return;

    bool advVal = false;
    if (ReadCvarBoolValue(g_cvarAdviserKeepHidden, &advVal)) {
        g_savedAdviserHidden = advVal;
    } else {
        g_savedAdviserHidden = false;
    }

    bool notifVal = false;
    if (ReadCvarBoolValue(g_cvarHudNotificationsKeepHidden, &notifVal)) {
        g_savedNotificationsHidden = notifVal;
    } else {
        g_savedNotificationsHidden = false;
    }

    bool ok1 = WriteCvarStringValue(g_cvarAdviserKeepHidden, "1");
    bool ok2 = WriteCvarStringValue(g_cvarHudNotificationsKeepHidden, "1");
    g_hudHiddenActive = true;

    TruckerLog("[HUD] ウィジェット・通知を非表示化しました (adviser=%s, notifications=%s)\n",
           ok1 ? "OK" : "SKIP", ok2 ? "OK" : "SKIP");
}

static void RestoreHudWidgetsAndNotifications() {
    if (!g_hudHiddenActive) return;

    if (g_cvarAdviserKeepHidden) {
        WriteCvarStringValue(g_cvarAdviserKeepHidden, g_savedAdviserHidden ? "1" : "0");
    }
    if (g_cvarHudNotificationsKeepHidden) {
        WriteCvarStringValue(g_cvarHudNotificationsKeepHidden, g_savedNotificationsHidden ? "1" : "0");
    }
    g_hudHiddenActive = false;
    TruckerLog("[HUD] ウィジェット・通知の表示設定を復元しました\n");
}

// -----------------------------------------------------------------------------
// 歩行モード開始 (F9) - 最新動的アドレス & テレメトリ連携
// -----------------------------------------------------------------------------
static void EnterWalkMode() {
    TruckerLog("[EnterWalk] >>> EnterWalkMode 開始...\n");

    // F9押下時のみ最新の動的カメラポインタを更新
    RefreshDynamicCameraOnSpawn();

    uint32_t activeSlot = GetActiveCameraSlot();
    uintptr_t sourceCam = GetCameraObjectBySlot(activeSlot);
    if (!sourceCam) {
        sourceCam = GetCameraObjectBySlot(2); // Slot 2 フォールバック
    }
    if (!sourceCam) {
        sourceCam = g_debugCamAddr;
    }
    if (!sourceCam) {
        TruckerLog("[EnterWalk] [ERROR] カメラオブジェクトが取得できないためスポーンできません\n");
        return;
    }
    TruckerLog("[EnterWalk] Step 1: sourceCam 取得成功 (Slot %u: 0x%llX)\n", activeSlot, (unsigned long long)sourceCam);

    float cx = 0, cy = 0, cz = 0;
    SafeRead(sourceCam + 0x40, &cx);
    SafeRead(sourceCam + 0x44, &cy);
    SafeRead(sourceCam + 0x48, &cz);

    int16_t secX = 0, secZ = 0;
    SafeRead(sourceCam + 0x4C, &secX);
    SafeRead(sourceCam + 0x4E, &secZ);

    float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
    SafeRead(sourceCam + 0x50, &qw);
    SafeRead(sourceCam + 0x54, &qx);
    SafeRead(sourceCam + 0x58, &qy);
    SafeRead(sourceCam + 0x5C, &qz);

    // テレメトリのトラック世界座標と照合 (セクターが未初期化の場合は補正)
    if (g_hasTruckWorld) {
        int16_t telSecX = (int16_t)std::floor(g_truckWorldX / 512.0);
        int16_t telSecZ = (int16_t)std::floor(g_truckWorldZ / 512.0);
        if (secX == 0 && secZ == 0) {
            secX = telSecX;
            secZ = telSecZ;
        }
    }

    // トラック右向きベクトル
    float tr_x = 1.0f - 2.0f * (qy * qy + qz * qz);
    float tr_z = 2.0f * (qx * qz + qw * qy);
    float len_r = std::sqrt(tr_x * tr_x + tr_z * tr_z);
    if (len_r > 1e-4f) {
        tr_x /= len_r;
        tr_z /= len_r;
    } else {
        tr_x = 1.0f; tr_z = 0.0f;
    }

    float warp_x = cx - tr_x * SPAWN_OFFSET_SIDE;
    float warp_z = cz - tr_z * SPAWN_OFFSET_SIDE;
    float ground_est = cy - 1.85f;
    float warp_y = ground_est + STANDING_EYE_HEIGHT;

    g_spawnSectorX = secX;
    g_spawnSectorZ = secZ;
    g_currentPos = Vec3(warp_x, warp_y, warp_z);
    g_lastSafePos = g_currentPos;

    g_cachedGroundY = ground_est;
    g_groundValid = true;
    TruckerLog("[EnterWalk] Step 2: スポーン座標計算完了: (%.1f, %.1f, %.1f) Sec:[%d,%d]\n", warp_x, warp_y, warp_z, secX, secZ);

    // F9押下時に原点オフセットを最新解決
    if (g_pVerifiedScene && g_hasTruckWorld) {
        SolveOriginOffsetViaTelemetry(g_pVerifiedScene, g_truckWorldX, g_truckWorldY, g_truckWorldZ);
    }

    // Slot 0 (フリーカメラ) にSEH保護付きで書き込み
    TruckerLog("[EnterWalk] Step 3: Slot 0 (g_debugCamAddr=0x%llX) へ座標書き込み開始...\n", (unsigned long long)g_debugCamAddr);
    if (g_debugCamAddr) {
        SafeWrite(g_debugCamAddr + 0x40, warp_x);
        SafeWrite(g_debugCamAddr + 0x44, warp_y);
        SafeWrite(g_debugCamAddr + 0x48, warp_z);
        SafeWrite(g_debugCamAddr + 0x4C, secX);
        SafeWrite(g_debugCamAddr + 0x4E, secZ);
        SafeWrite(g_debugCamAddr + 0x50, qw);
        SafeWrite(g_debugCamAddr + 0x54, qx);
        SafeWrite(g_debugCamAddr + 0x58, qy);
        SafeWrite(g_debugCamAddr + 0x5C, qz);
    }
    TruckerLog("[EnterWalk] Step 4: Slot 0 への書き込み完了\n");

    // focus2 初期値設定 & フック有効化
    g_hookWalkerFocus.isValid = 1;
    g_hookWalkerFocus.x = warp_x;
    g_hookWalkerFocus.y = warp_y;
    g_hookWalkerFocus.z = warp_z;
    g_hookWalkerFocus.secX = secX;
    g_hookWalkerFocus.secZ = secZ;

    TruckerLog("[EnterWalk] Step 5: EnableSceneFocusHook 呼び出し開始...\n");
    bool focusOk = EnableSceneFocusHook();
    TruckerLog("[EnterWalk] Step 5 完了: EnableSceneFocusHook 戻り値 = %s\n", focusOk ? "成功" : "失敗/スキップ");

    // カメラ視点をフリーカメラ (Slot 0) へ切り替え
    TruckerLog("[EnterWalk] Step 6: SwitchCameraSlot(0) 呼び出し開始...\n");
    SwitchCameraSlot(0);
    TruckerLog("[EnterWalk] Step 6 完了: SwitchCameraSlot(0) 完了\n");

    // 初回コライダーロード (歩行者周囲 15m)
    g_lastRegionPos = g_currentPos;
    bool regionOk = ExecuteRegionAroundWalker(15.0f);
    TruckerLog("[EnterWalk] Step 7: 初回周囲コライダーロード(15m) = %s\n", regionOk ? "成功" : "失敗/スキップ");

    // 車両操作系 (Space/WASD等) の入力保持メモリをブランク化 & ウィジェット・通知を非表示化
    LockVehicleInput();
    HideHudWidgetsAndNotifications();

    g_isWalking = true;
    g_walkStartTime = (float)GetTickCount64() / 1000.0f;
    TruckerLog("[EnterWalk] Step 8: 歩行モードONフラグ設定完了! (isWalking=true)\n");

    printf("\n>>> 【歩行モード開始】スポーン成功: (%.1f, %.1f, %.1f) Sec:[%d,%d]\n", warp_x, warp_y, warp_z, secX, secZ);
    printf(">>> 【地面接地】推定地上高: %.2fm (目線高: %.2fm)\n", g_cachedGroundY, warp_y);
}

// -----------------------------------------------------------------------------
// 歩行モード終了 (F9 / Anti-Void)
// -----------------------------------------------------------------------------
static void ExitWalkMode(const char* reason = "ユーザー操作") {
    g_isWalking = false;
    UnlockVehicleInput();
    RestoreHudWidgetsAndNotifications();
    DisableSceneFocusHook();
    SwitchCameraSlot(2);
    printf("\n>>> 【歩行モード終了】理由: %s -> 運転席視点に復帰\n", reason);
}

static scs_telemetry_unregister_from_event_t g_unregisterEventFn = nullptr;

// -----------------------------------------------------------------------------
// SCS Telemetry frame_end コールバック (メインスレッド同期実行: TM Real Walk準拠)
// -----------------------------------------------------------------------------
static void TelemetryFrameStartCallback(const scs_event_t event, const void* const event_info, const scs_context_t context) {
    if (!g_running) return;

    if (g_isWalking) {
        UpdateLockedVehicleInputEveryFrame();
    }
}

static void TelemetryFrameEndCallback(const scs_event_t event, const void* const event_info, const scs_context_t context) {
    if (!g_running) return;
    if (g_isWalking) {
        UpdateLockedVehicleInputEveryFrame();
    }

    static uint64_t lastTime = 0;
    static uint64_t lastPrintTime = 0;
    static uint64_t lastRegionTime = 0;

    uint64_t now = GetTickCount64();
    if (lastTime == 0) lastTime = now;
    float dt = (float)(now - lastTime) / 1000.0f;
    if (dt <= 0.0f) dt = 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    lastTime = now;

    // シーン候補からテストRaycastで真のシーンを確定 (STAGE 3)
    if (!g_pVerifiedScene && !g_sceneCandidates.empty()) {
        for (void* scene : g_sceneCandidates) {
            PxRaycastBufferNative buf;
            memset(&buf, 0, sizeof(buf));
            buf.vtable = &g_dummyVtable;
            buf.block.faceIndex = 0xFFFFFFFF;
            buf.block.distance = 1e30f;

            PxQueryFilterDataNative filterData;
            memset(&filterData, 0, sizeof(filterData));
            filterData.flags = 1;

            bool ok = TMRealWalkRaycastSafe(scene, Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 1.0f, &buf, 0x0003, &filterData);
            g_pVerifiedScene = scene;
            TruckerLog("[physx] STAGE 3: 物理シーン確定: %p (テストRaycast応答: %d)\n", scene, ok ? 1 : 0);
            break;
        }
    }

    // 原点オフセット確定 (STAGE 4: 未解決時のみ実行)
    if (g_pVerifiedScene && !g_originOffsetSolved && g_hasTruckWorld) {
        SolveOriginOffsetViaTelemetry(g_pVerifiedScene, g_truckWorldX, g_truckWorldY, g_truckWorldZ);
    }

    // F9: 歩行モード切替
    static bool s_lastF9State = false;
    bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9Down && !s_lastF9State) {
        if (!g_isWalking) {
            bool menuCursor = IsInGameMenuCursorVisible();
            // 歩行モード突入判定:
            // 1. ガレージ/メニュー/ポーズ中ではないこと (!g_gamePaused)
            // 2. メニュー用のマウスカーソルが表示されていないこと (!menuCursor)
            // 3. トラックがワールド上にロード済みであること (g_hasTruckWorld)
            // 4. パーキングブレーキが掛かっていること (g_parkingBrakeOn)
            if (!g_gamePaused && !menuCursor && g_hasTruckWorld && g_parkingBrakeOn) {
                printf("\n>>> [F9キー押下] 開始条件クリア (Paused:NO, Cursor:NO, Truck:OK, P-Brake:ON) -> 歩行モード突入\n");
                EnterWalkMode();
            } else {
                printf("\n>>> [F9拒否] 歩行開始条件を満たしていません (Paused:%s, UIカーソル:%s, Truck:%s, P-Brake:%s)\n",
                       g_gamePaused ? "YES(不可)" : "NO(OK)",
                       menuCursor ? "表示中(メニュー画面・不可)" : "非表示(OK)",
                       g_hasTruckWorld ? "OK" : "未ロード(不可)",
                       g_parkingBrakeOn ? "ON(OK)" : "OFF(解除中・不可)");
            }
        } else {
            ExitWalkMode("F9キー押下");
        }
    }
    s_lastF9State = f9Down;

    // F10: モニター表示切替
    static bool s_lastF10State = false;
    bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !s_lastF10State) {
        g_monitorEnabled = !g_monitorEnabled;
        printf(">>> リアルタイムモニター: %s\n", g_monitorEnabled ? "ON" : "OFF");
    }
    s_lastF10State = f10Down;

    // 歩行制御ループ (メインスレッドで直列実行)
    if (g_isWalking && g_debugCamAddr) {
        // BackSpace: 緊急脱出
        if ((GetAsyncKeyState(VK_BACK) & 0x8000) != 0) {
            ExitWalkMode("緊急脱出 (BackSpace)");
            return;
        }

        // Alt: Noclip 切替 (歩行中のみ受付)
        static bool s_lastAltState = false;
        bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (altDown && !s_lastAltState) {
            g_isNoclip = !g_isNoclip;
            printf(">>> モード切替: %s\n", g_isNoclip ? "【NOCLIP (壁抜けON)】" : "【通常歩行 (接地・コリジョンON)】");
        }
        s_lastAltState = altDown;

        // フリーカメラ クォータニオン取得
        float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        SafeRead(g_debugCamAddr + 0x50, &qw);
        SafeRead(g_debugCamAddr + 0x54, &qx);
        SafeRead(g_debugCamAddr + 0x58, &qy);
        SafeRead(g_debugCamAddr + 0x5C, &qz);

        float qnorm = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
        if (qnorm > 1e-6f) {
            qw /= qnorm; qx /= qnorm; qy /= qnorm; qz /= qnorm;
        }

        // 前方ベクトル
        float fx = -2.0f * (qx * qz + qw * qy);
        float fz = -(qw * qw + qz * qz - qx * qx - qy * qy);
        float fl = std::sqrt(fx * fx + fz * fz);
        float fwdX = (fl > 1e-4f) ? (fx / fl) : 0.0f;
        float fwdZ = (fl > 1e-4f) ? (fz / fl) : -1.0f;

        // 右方向ベクトル
        float rx = qw * qw + qx * qx - qy * qy - qz * qz;
        float rz = 2.0f * (qx * qz - qw * qy);
        float rl = std::sqrt(rx * rx + rz * rz);
        float rgtX = (rl > 1e-4f) ? (rx / rl) : 1.0f;
        float rgtZ = (rl > 1e-4f) ? (rz / rl) : 0.0f;

        float inputFwd = 0.0f;
        float inputRgt = 0.0f;
        if (GetAsyncKeyState('W') & 0x8000) inputFwd += 1.0f;
        if (GetAsyncKeyState('S') & 0x8000) inputFwd -= 1.0f;
        if (GetAsyncKeyState('D') & 0x8000) inputRgt += 1.0f;
        if (GetAsyncKeyState('A') & 0x8000) inputRgt -= 1.0f;

        float inLen = std::sqrt(inputFwd * inputFwd + inputRgt * inputRgt);
        if (inLen > 0.0f) {
            inputFwd /= inLen;
            inputRgt /= inLen;
        }

        float speed = WALK_SPEED;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= SPRINT_MULTIPLIER;

        float moveDx = (fwdX * inputFwd + rgtX * inputRgt) * speed * dt;
        float moveDz = (fwdZ * inputFwd + rgtZ * inputRgt) * speed * dt;

        float moveDy = 0.0f;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) moveDy += VERTICAL_SPEED * dt;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) moveDy -= VERTICAL_SPEED * dt;

        // セクター相対ローカル座標 ([0, 512) に正規化)
        int delta_sx = (int)std::floor(g_currentPos.x / 512.0f);
        int delta_sz = (int)std::floor(g_currentPos.z / 512.0f);
        float camLocalX = g_currentPos.x - (float)delta_sx * 512.0f;
        float camLocalZ = g_currentPos.z - (float)delta_sz * 512.0f;
        int16_t curSecX = g_spawnSectorX + delta_sx;
        int16_t curSecZ = g_spawnSectorZ + delta_sz;

        // focus2 メモリバッファ更新
        g_hookWalkerFocus.x = camLocalX;
        g_hookWalkerFocus.y = g_currentPos.y;
        g_hookWalkerFocus.z = camLocalZ;
        g_hookWalkerFocus.secX = curSecX;
        g_hookWalkerFocus.secZ = curSecZ;

        // 1000ms毎の能動的コライダー強制ロード (15.0f: ユーザー指定最適化)
        if (now - lastRegionTime >= 1000) {
            lastRegionTime = now;
            ExecuteRegionAroundWalker(15.0f);
        }

        // PhysX Raycast 座標計算
        double walkerWorldX = (double)curSecX * 512.0 + (double)camLocalX;
        double walkerWorldZ = (double)curSecZ * 512.0 + (double)camLocalZ;
        float physxSx = (float)(walkerWorldX - g_originOffsetX);
        float physxSz = (float)(walkerWorldZ - g_originOffsetZ);

        // =================================================================
        // 【精巧化された物理処理】障害物当たり判定・壁スライド・地面追従
        // =================================================================
        if (!g_isNoclip && g_pVerifiedScene && g_originOffsetSolved) {
            // 1. 地面判定 (5点クロスサンプリング)
            float feetY = g_currentPos.y - STANDING_EYE_HEIGHT;
            float startRayY = feetY + 0.35f;
            float hitGroundY = 0.0f;
            bool hitGround = ExecuteGroundRaycast5Points(g_pVerifiedScene, physxSx, physxSz, startRayY, &hitGroundY);

            if (hitGround) {
                g_cachedGroundY = hitGroundY;
                g_groundValid = true;
            }

            // 2. 移動予測判定 (Pre-move Sweep Block: 体幹半径 BODY_RADIUS による完全遮断)
            g_wallHit = false;
            if (inLen > 0.0f) {
                float moveDist = std::sqrt(moveDx * moveDx + moveDz * moveDz);
                float dirX = (moveDist > 1e-4f) ? (moveDx / moveDist) : fwdX;
                float dirZ = (moveDist > 1e-4f) ? (moveDz / moveDist) : fwdZ;

                const float BODY_RADIUS = 0.45f; // キャラクターの体幹半径
                const float checkDist = moveDist + BODY_RADIUS; // 目標地点までの先読み距離

                // A. 胸・腰の高さ (地面+0.85m)
                float chestRayY = feetY + 0.85f;
                float wallDistChest = 0.0f;
                Vec3 normalChest(0.0f, 0.0f, 0.0f);
                bool hitChest = ExecuteObstacleRaycast(g_pVerifiedScene, physxSx, physxSz, chestRayY, dirX, dirZ, checkDist, &wallDistChest, &normalChest);

                // B. 膝の高さ (地面+0.40m)
                float kneeRayY = feetY + 0.40f;
                float wallDistKnee = 0.0f;
                Vec3 normalKnee(0.0f, 0.0f, 0.0f);
                bool hitKnee = ExecuteObstacleRaycast(g_pVerifiedScene, physxSx, physxSz, kneeRayY, dirX, dirZ, checkDist, &wallDistKnee, &normalKnee);

                if (hitChest || hitKnee) {
                    g_wallHit = true;
                    g_wallDistance = (hitChest && hitKnee) ? std::min(wallDistChest, wallDistKnee) : (hitChest ? wallDistChest : wallDistKnee);
                    g_wallNormal = (g_wallDistance == wallDistChest && hitChest) ? normalChest : normalKnee;

                    // 法線の正規化チェック
                    float nLen = std::sqrt(g_wallNormal.x * g_wallNormal.x + g_wallNormal.z * g_wallNormal.z);
                    if (nLen > 1e-3f) {
                        g_wallNormal.x /= nLen;
                        g_wallNormal.z /= nLen;
                    }

                    // 【当たった瞬間の移動完全ブロック】
                    // 障害物までの距離が体幹半径以下の場合は、壁方向への移動を完全にゼロにして前進を停止
                    if (g_wallDistance <= BODY_RADIUS) {
                        moveDx = 0.0f;
                        moveDz = 0.0f;

                        // 密着・埋まり防止の安全オフセット (壁の外側へわずかに離す)
                        if (nLen > 1e-3f) {
                            float pushAmount = (BODY_RADIUS - g_wallDistance) + 0.01f;
                            if (pushAmount > 0.0f && pushAmount < 0.10f) {
                                moveDx += g_wallNormal.x * pushAmount;
                                moveDz += g_wallNormal.z * pushAmount;
                            }
                        }
                    } else {
                        // 障害物に近づける限界まで移動量をクリップ
                        float allowedMove = g_wallDistance - BODY_RADIUS;
                        if (allowedMove < moveDist && allowedMove > 0.0f) {
                            moveDx = dirX * allowedMove;
                            moveDz = dirZ * allowedMove;
                        }
                    }
                }
            }
        } else {
            g_wallHit = false;
        }

        // 移動の適用
        Vec3 nextPos = g_currentPos;
        nextPos.x += moveDx;
        nextPos.z += moveDz;
        nextPos.y += moveDy;

        // 地面高さのスムーズ追従 (指数平滑補間: 時定数 0.06秒)
        if (!g_isNoclip && g_groundValid) {
            float targetEyeY = g_cachedGroundY + STANDING_EYE_HEIGHT;
            float diffY = targetEyeY - nextPos.y;
            if (std::abs(diffY) <= 2.0f) {
                float smoothFactor = 1.0f - std::exp(-dt / 0.06f);
                nextPos.y += diffY * smoothFactor;
            }
        }

        g_lastSafePos = g_currentPos;
        g_currentPos = nextPos;

        // カメラ座標の再計算 & 反映 ([0, 512) に正規化)
        delta_sx = (int)std::floor(g_currentPos.x / 512.0f);
        delta_sz = (int)std::floor(g_currentPos.z / 512.0f);
        camLocalX = g_currentPos.x - (float)delta_sx * 512.0f;
        camLocalZ = g_currentPos.z - (float)delta_sz * 512.0f;
        curSecX = g_spawnSectorX + delta_sx;
        curSecZ = g_spawnSectorZ + delta_sz;

        *(float*)(g_debugCamAddr + 0x40) = camLocalX;
        *(float*)(g_debugCamAddr + 0x44) = g_currentPos.y;
        *(float*)(g_debugCamAddr + 0x48) = camLocalZ;
        *(int16_t*)(g_debugCamAddr + 0x4C) = curSecX;
        *(int16_t*)(g_debugCamAddr + 0x4E) = curSecZ;

        // リアルタイムモニター出力 (0.3秒間隔)
        if (g_monitorEnabled && (now - lastPrintTime > 300)) {
            lastPrintTime = now;
            const char* noclipStr = g_isNoclip ? "NOCLIP" : "GROUND";
            const char* wallStr = g_wallHit ? "WALL_SLIDE" : "CLEAR";
            char gStr[32];
            if (g_groundValid) sprintf_s(gStr, "%5.2fm", g_cachedGroundY);
            else sprintf_s(gStr, "  LOST");

            TruckerLog("[WalkFree] Pos:(%6.1f, %5.2f, %6.1f) | Sec:[%d,%d] | State:%-6s | Ground:%s | Obs:%-10s | Focus:%u Reg:%u\n",
                g_currentPos.x, g_currentPos.y, g_currentPos.z, curSecX, curSecZ,
                noclipStr, gStr, wallStr,
                g_redirectedFocusCalls, g_regionQueryCount);
        }
    }
}

// -----------------------------------------------------------------------------
// バックグラウンド初期化スレッド (初回特定のみ行い、メインループは telemetry 側へ委譲)
// -----------------------------------------------------------------------------
static DWORD WINAPI WalkThread(LPVOID lpParam) {
    InitConsole();

    LocatePhysxVtable();
    for (int retry = 0; retry < 50; ++retry) {
        if (LocateCameraManager()) break;
        Sleep(200);
    }
    LocateSceneFocusModule();
    LocateInputMixModule();
    LocateHudCvars();

    TruckerLog("[WalkInitThread] 初期化完了: メインループはゲームのFrameEndイベントで同期稼働します (F9キー待機中)...\n");

    // ドライブ画面を検知して物理シーンヒープスキャンを開始
    bool s_scanStarted = false;
    while (g_running && !g_pVerifiedScene) {
        uint32_t currentSlot = GetActiveCameraSlot();
        if (currentSlot < 14 && !s_scanStarted && g_physxSceneVtable) {
            s_scanStarted = true;
            TruckerLog("[physx] ドライブ画面検知 (Slot=%u). 物理シーン探索スレッド起動...\n", currentSlot);
            HANDLE hScan = CreateThread(NULL, 0, SceneScannerThread, NULL, 0, NULL);
            if (hScan) CloseHandle(hScan);
            break;
        }
        Sleep(500);
    }

    return 0;
}

// -----------------------------------------------------------------------------
// SCS テレメトリプラグイン エントリーポイント
// -----------------------------------------------------------------------------
extern "C" {

SCS_EXPORT scs_u32_t scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_v100_t* const params) {
    if (version < 0x00010000 || !params) return 1;

    g_pVehHandler = AddVectoredExceptionHandler(1, CrashShieldVectoredHandler);
    g_gameBase = (uintptr_t)GetModuleHandle(NULL);

    if (params->register_for_channel) {
        scs_u32_t res1 = params->register_for_channel("truck.world.placement", 0xFFFFFFFF, SCS_VALUE_TYPE_dplacement, 0, TelemetryPlacementCallback, NULL);
        scs_u32_t res2 = params->register_for_channel("truck.speed", 0xFFFFFFFF, SCS_VALUE_TYPE_float, 0, TelemetrySpeedCallback, NULL);
        scs_u32_t res3 = params->register_for_channel("truck.brake.parking", 0xFFFFFFFF, SCS_VALUE_TYPE_bool, 0, TelemetryParkingBrakeCallback, NULL);
        TruckerLog("[telemetry] register truck.world.placement: %s (code %d)\n", res1 == 0 ? "OK" : "FAILED", (int)res1);
        TruckerLog("[telemetry] register truck.speed: %s (code %d)\n", res2 == 0 ? "OK" : "FAILED", (int)res2);
        TruckerLog("[telemetry] register truck.brake.parking: %s (code %d)\n", res3 == 0 ? "OK" : "FAILED", (int)res3);
    }

    if (params->register_for_event) {
        g_unregisterEventFn = params->unregister_from_event;
        scs_u32_t evStartRes = params->register_for_event(SCS_TELEMETRY_EVENT_frame_start, TelemetryFrameStartCallback, NULL);
        scs_u32_t evEndRes   = params->register_for_event(SCS_TELEMETRY_EVENT_frame_end, TelemetryFrameEndCallback, NULL);
        scs_u32_t evPauseRes = params->register_for_event(SCS_TELEMETRY_EVENT_paused, TelemetryPauseCallback, NULL);
        scs_u32_t evRunRes   = params->register_for_event(SCS_TELEMETRY_EVENT_started, TelemetryPauseCallback, NULL);
        TruckerLog("[telemetry] register SCS_TELEMETRY_EVENT_frame_start: %s (code %d)\n", evStartRes == 0 ? "OK" : "FAILED", (int)evStartRes);
        TruckerLog("[telemetry] register SCS_TELEMETRY_EVENT_frame_end: %s (code %d)\n", evEndRes == 0 ? "OK" : "FAILED", (int)evEndRes);
        TruckerLog("[telemetry] register SCS_TELEMETRY_EVENT_paused: %s (code %d)\n", evPauseRes == 0 ? "OK" : "FAILED", (int)evPauseRes);
        TruckerLog("[telemetry] register SCS_TELEMETRY_EVENT_started: %s (code %d)\n", evRunRes == 0 ? "OK" : "FAILED", (int)evRunRes);
    }

    g_running = true;
    g_hThread = CreateThread(NULL, 0, WalkThread, NULL, 0, NULL);
    return 0;
}

SCS_EXPORT void scs_telemetry_shutdown(void) {
    g_running = false;
    __try {
        UnlockVehicleInput();
        RestoreHudWidgetsAndNotifications();
        DisableSceneFocusHook();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // スレッド待機でメインスレッドをブロックしない（即座にハンドルだけクローズ）
    if (g_hThread) {
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    if (g_pVehHandler) {
        RemoveVectoredExceptionHandler(g_pVehHandler);
        g_pVehHandler = NULL;
    }
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    return TRUE;
}
