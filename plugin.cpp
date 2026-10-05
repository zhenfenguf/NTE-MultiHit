// 多重打击（Multi Hit）——正式版最小实现：界面上只有"启用开关 + 倍率输入框 + 一句提示"。
//
// 机制（推导过程与实测记录见 AI接手文件 §9.26~§9.29）：
//   游戏里有一个"命中结算"原生函数（下称 burst）。复刻 Tokky-NTE 的两步做法：
//     ① 钩住 burst，在 detour 里用**同一组参数**把原函数再调 N-1 次；
//     ② 同时钩住 burst 内部调用的节流判定函数（下称 gate），让它恒返回 1（放行）。
//        burst 开头是 `if (!gate(...)) return;`，不放行的话我们的重入会被直接挡掉。
//   实测：20 倍小怪 2 下、50 倍 1 下；倍率 200 会被服务端踢下线，所以上限取 100。
//
// 地址来源（仓库规则：偏移与签名必须有 Profile、validator 与降级行为，不能硬编码在插件里）：
//   两条特征码写在 profiles/nte/nte-current.json 的 nte.multi-hit.burst / nte.multi-hit.gate，
//   插件运行时从宿主正在用的那份 Profile 读出 pattern，交给 anomaly.interop.signature 解析。
//   读不到或匹配失败 ⇒ 保持惰性，面板写明原因，绝不猜地址。

#include "anomaly/sdk/cpp.hpp"
#include "anomaly/sdk/services/interop.h"
#include "anomaly/sdk/services/ui.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

namespace {

// ——— 界面文案：面板标题、开关、输入框 ———
// 面板标题后缀的版本号与那句提示词都从 manifest.json 读（见 LoadManifestIdentity），
// 所以改版本号或改提示词只需要动 manifest.json，不用重新编译。
constexpr char kPanelTitle[] = "多重打击";
constexpr char kEnableLabel[] = "启用多重打击";
constexpr char kMultiplierLabel[] = "每次结算几次（2-100）";
constexpr char kFallbackVersion[] = "1.0.0";
constexpr char kFallbackHint[] = "建议倍率控制在50以下，超过可能造成卡顿、被服务器踢等异象";

// 倍率上限 100：实测 50 安全、200 会被服务端踢下线（AI接手文件 §9.29）。
constexpr std::uint32_t kMinMultiplier = 2U;
constexpr std::uint32_t kMaxMultiplier = 100U;
constexpr std::uint32_t kDefaultMultiplier = 50U;

constexpr char kSymbolBurst[] = "nte.multi-hit.burst";
constexpr char kSymbolGate[] = "nte.multi-hit.gate";
constexpr char kModuleName[] = "HTGame.exe";
constexpr char kSectionName[] = ".text";

// burst 与 gate 都是 5 参数原生函数：x64 下前 4 个走 rcx/rdx/r8/r9，第 5 个在 [rsp+0x28]
// （反汇编逐条核对过：burst 读 [rsp+0x90]、gate 读 [rbp+0x7f]，换算后都是入口 +0x28）。
using NativeFn = void*(__fastcall*)(void*, void*, void*, void*, void*);

struct Context final {
    const AnomalyHostApiV1* host{};
    const AnomalyUiServiceV1* ui{};
    const AnomalyHookServiceV1* hook{};
    const AnomalySignatureServiceV1* signature{};

    NativeFn burst_original{};
    NativeFn gate_original{};
    AnomalyGenerationHandleV1 burst_hook{};
    AnomalyGenerationHandleV1 gate_hook{};
    std::atomic<std::uint64_t> bursts{};
    std::atomic<std::uint64_t> replays{};

    std::atomic<int> enabled{0};
    std::atomic<std::uint32_t> multiplier{kDefaultMultiplier};

    int window_open{1};
    char status[192]{"正在等待游戏模块…"};
    std::uint64_t next_try_ms{};
};

Context* g_context{};             // detour 没有 user 参数，只能从这里取状态
thread_local bool t_replaying{};  // 重入守卫：防止我们自己的重放再被放大

// manifest.json 里拿到的身份信息：版本号挂到面板标题后面，description 就是面板上的提示词。
struct ManifestIdentity final {
    char version[32];
    char title[96];
    char hint[512];
};

ManifestIdentity g_identity{};

// 重放期间置位，任何退出路径都会复位（含原函数里抛出的异常/远跳）。
struct ReplayGuard final {
    ReplayGuard() noexcept { t_replaying = true; }
    ~ReplayGuard() noexcept { t_replaying = false; }
    ReplayGuard(const ReplayGuard&) = delete;
    ReplayGuard& operator=(const ReplayGuard&) = delete;
};

bool Amplifying() noexcept {
    return t_replaying ||
           (g_context != nullptr && g_context->enabled.load(std::memory_order_relaxed) != 0);
}

// ——— detour ———

void* ANOMALY_CALL BurstDetour(void* a, void* b, void* c, void* d, void* e) {
    Context* const context = g_context;
    if (context == nullptr || context->burst_original == nullptr) return nullptr;

    void* const result = context->burst_original(a, b, c, d, e);  // 先按游戏原本的语义走一次
    if (!Amplifying()) return result;

    // 重放期间持回调租约：热重载/卸载时宿主会等我们退出，不会把代码页从脚下拆掉。
    AnomalyGenerationHandleV1 lease{};
    if (context->hook == nullptr || context->hook->begin_callback == nullptr ||
        context->hook->begin_callback(context->hook->user, context->burst_hook, &lease).code !=
            ANOMALY_STATUS_V1_OK) {
        return result;
    }

    std::uint32_t times = context->multiplier.load(std::memory_order_relaxed);
    if (times < kMinMultiplier) times = kMinMultiplier;
    if (times > kMaxMultiplier) times = kMaxMultiplier;

    const ReplayGuard guard;
    for (std::uint32_t extra = 1U; extra < times; ++extra) {
        context->burst_original(a, b, c, d, e);
    }

    context->bursts.fetch_add(1U, std::memory_order_relaxed);
    context->replays.fetch_add(times - 1U, std::memory_order_relaxed);
    if (context->hook->end_callback != nullptr) {
        context->hook->end_callback(context->hook->user, lease);
    }
    return result;
}

// gate 是 burst 的节流判定：返回 0 会让 burst 开头的 `je` 直接退出。
// 只有在我们放大时（含自己的重放）才恒返回 1 —— 也就是 Tokky 那句"门控返回 1"；
// 关掉开关时原样转发，保证"关闭 == 游戏原样"。
void* ANOMALY_CALL GateDetour(void* a, void* b, void* c, void* d, void* e) {
    if (Amplifying()) return reinterpret_cast<void*>(static_cast<std::intptr_t>(1));
    Context* const context = g_context;
    if (context != nullptr && context->gate_original != nullptr) {
        return context->gate_original(a, b, c, d, e);
    }
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(1));
}

// ——— 读文件 / 取 JSON 里的字符串字段（Profile 与 manifest.json 共用）———

bool ReadFileText(const wchar_t* path, std::string& out) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char chunk[8192];
    DWORD read = 0;
    while (out.size() < 512U * 1024U &&
           ReadFile(file, chunk, static_cast<DWORD>(sizeof(chunk)), &read, nullptr) != 0 &&
           read > 0) {
        out.append(chunk, read);
    }
    CloseHandle(file);
    return !out.empty();
}

// 取出 "<key>": "value" 里的 value。只认第一个匹配，够用 —— 这两个文件都是我们自己维护的。
bool JsonStringValue(const std::string& json, const char* key, char* out,
                     const std::size_t cap) noexcept {
    const std::size_t key_at = json.find(std::string("\"") + key + "\"");
    if (key_at == std::string::npos) return false;
    const std::size_t colon = json.find(':', key_at);
    if (colon == std::string::npos) return false;
    const std::size_t begin = json.find('"', colon + 1U);
    if (begin == std::string::npos) return false;
    const std::size_t end = json.find('"', begin + 1U);
    if (end == std::string::npos || end <= begin + 1U) return false;
    const std::size_t length = end - begin - 1U;
    if (length >= cap) return false;
    std::memcpy(out, json.data() + begin + 1U, length);
    out[length] = '\0';
    return true;
}

// 读本插件包里的 manifest.json（与 plugin.dll 同目录）：
//   version     → 面板标题后缀 + 报给宿主的插件版本
//   description → 面板上的提示词
// 读不到就退回编译期默认值，绝不因此影响功能。
void LoadManifestIdentity() noexcept {
    std::snprintf(g_identity.version, sizeof(g_identity.version), "%s", kFallbackVersion);
    std::snprintf(g_identity.hint, sizeof(g_identity.hint), "%s", kFallbackHint);

    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(reinterpret_cast<const void*>(
                               &LoadManifestIdentity)),
                           &self) != 0) {
        wchar_t directory[MAX_PATH]{};
        if (GetModuleFileNameW(self, directory, MAX_PATH) != 0) {
            wchar_t* const slash = std::wcsrchr(directory, L'\\');
            if (slash != nullptr) {
                *slash = L'\0';
                wchar_t manifest[MAX_PATH]{};
                if (_snwprintf_s(manifest, MAX_PATH, _TRUNCATE, L"%ls\\manifest.json", directory) >
                    0) {
                    std::string json;
                    if (ReadFileText(manifest, json)) {
                        JsonStringValue(json, "version", g_identity.version,
                                        sizeof(g_identity.version));
                        JsonStringValue(json, "description", g_identity.hint,
                                        sizeof(g_identity.hint));
                    }
                }
            }
        }
    }
    _snprintf_s(g_identity.title, sizeof(g_identity.title), _TRUNCATE, "%s v%s", kPanelTitle,
                g_identity.version);
}

// ——— 从 Profile 取 pattern，再解析地址 ———

// 读 <游戏目录>\Anomaly\profiles\nte\nte-current.json 里某个符号的 pattern 文本。
// 任何一步失败都返回 false —— 降级优于猜测。
bool ProfilePattern(const char* symbol, char* out, const std::size_t cap) noexcept {
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return false;
    wchar_t* const slash = std::wcsrchr(exe, L'\\');
    if (slash == nullptr) return false;
    *slash = L'\0';

    wchar_t path[MAX_PATH]{};
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\Anomaly\\profiles\\nte\\nte-current.json",
                     exe) < 0) {
        return false;
    }

    std::string json;
    if (!ReadFileText(path, json)) return false;

    const std::size_t symbol_at = json.find(std::string("\"") + symbol + "\"");
    if (symbol_at == std::string::npos) return false;
    const std::size_t pattern_at = json.find("\"pattern\"", symbol_at);
    if (pattern_at == std::string::npos) return false;
    return JsonStringValue(json.substr(pattern_at), "pattern", out, cap);
}

std::uintptr_t ResolveSymbol(const Context& context, const char* symbol) noexcept {
    if (context.signature == nullptr || context.signature->resolve == nullptr) return 0;
    char pattern[512]{};
    if (!ProfilePattern(symbol, pattern, sizeof(pattern))) return 0;
    std::uintptr_t address = 0;
    if (context.signature
            ->resolve(context.signature->user, anomaly::sdk::StringView(kModuleName),
                      anomaly::sdk::StringView(kSectionName), anomaly::sdk::StringView(pattern),
                      &address)
            .code != ANOMALY_STATUS_V1_OK ||
        address == 0) {
        return 0;
    }
    return address;
}

bool ArmOne(Context& context, const std::uintptr_t target, void* detour, const char* label,
            NativeFn* out_original, AnomalyGenerationHandleV1* out_handle) noexcept {
    AnomalyHookRequestV1 request{sizeof(request)};
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = target;
    request.detour = detour;
    request.label = anomaly::sdk::StringView(label);
    std::uintptr_t original = 0;
    if (context.hook->create(context.hook->user, &request, &original, out_handle).code !=
            ANOMALY_STATUS_V1_OK ||
        original == 0 || out_handle->id == 0) {
        return false;
    }
    *out_original = reinterpret_cast<NativeFn>(original);
    return true;
}

void Release(Context& context) noexcept {
    if (context.hook == nullptr) return;
    if (context.hook->release != nullptr) {
        if (context.burst_hook.id != 0) context.hook->release(context.hook->user, context.burst_hook);
        if (context.gate_hook.id != 0) context.hook->release(context.hook->user, context.gate_hook);
    }
    context.burst_hook = {};
    context.gate_hook = {};
    context.burst_original = nullptr;
    context.gate_original = nullptr;
}

void SetStatus(Context& context, const char* text) noexcept {
    std::snprintf(context.status, sizeof(context.status), "%s", text);
}

// 解析 + 挂钩。失败时保持惰性，把原因留在面板上。
void Arm(Context& context) noexcept {
    if (context.hook == nullptr || context.hook->create == nullptr || context.signature == nullptr) {
        SetStatus(context, "等待宿主服务就绪…");
        return;
    }

    const std::uintptr_t burst = ResolveSymbol(context, kSymbolBurst);
    const std::uintptr_t gate = ResolveSymbol(context, kSymbolGate);
    if (burst == 0 || gate == 0) {
        SetStatus(context, "未找到游戏函数：Profile 缺少 nte.multi-hit.* 符号，或游戏已更新导致特征码失效");
        return;
    }

    NativeFn burst_original = nullptr;
    NativeFn gate_original = nullptr;
    AnomalyGenerationHandleV1 burst_hook{};
    AnomalyGenerationHandleV1 gate_hook{};
    const bool burst_ok =
        ArmOne(context, burst, reinterpret_cast<void*>(&BurstDetour), "multi-hit-burst",
               &burst_original, &burst_hook);
    const bool gate_ok = ArmOne(context, gate, reinterpret_cast<void*>(&GateDetour),
                                "multi-hit-gate", &gate_original, &gate_hook);
    if (!burst_ok || !gate_ok) {
        if (burst_ok && context.hook->release != nullptr) {
            context.hook->release(context.hook->user, burst_hook);
        }
        if (gate_ok && context.hook->release != nullptr) {
            context.hook->release(context.hook->user, gate_hook);
        }
        std::snprintf(context.status, sizeof(context.status),
                      "挂钩失败（burst=0x%llX gate=0x%llX）；游戏更新后本插件需新的特征码",
                      static_cast<unsigned long long>(burst),
                      static_cast<unsigned long long>(gate));
        return;
    }

    context.burst_original = burst_original;
    context.gate_original = gate_original;
    context.burst_hook = burst_hook;
    context.gate_hook = gate_hook;
    std::snprintf(context.status, sizeof(context.status), "已就绪（burst=0x%llX gate=0x%llX）",
                  static_cast<unsigned long long>(burst),
                  static_cast<unsigned long long>(gate));
}

// ——— 配置持久化：只有两个键 ———

bool ConfigPath(wchar_t* out, const std::size_t cap) noexcept {
    wchar_t* local = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&local, &size, L"LOCALAPPDATA") != 0 || local == nullptr) return false;
    const bool ok =
        _snwprintf_s(out, cap, _TRUNCATE, L"%ls\\Anomaly\\MultiHitMini.cfg", local) >= 0;
    std::free(local);
    return ok;
}

void LoadConfig(Context& context) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!ConfigPath(path, MAX_PATH)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") != 0 || file == nullptr) return;

    char line[128];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::uint32_t mult = 0;
        int enabled = 0;
        if (sscanf_s(line, "mult=%u", &mult) == 1) {
            if (mult < kMinMultiplier) mult = kMinMultiplier;
            if (mult > kMaxMultiplier) mult = kMaxMultiplier;
            context.multiplier.store(mult, std::memory_order_relaxed);
        } else if (sscanf_s(line, "enabled=%d", &enabled) == 1) {
            context.enabled.store(enabled != 0 ? 1 : 0, std::memory_order_relaxed);
        }
    }
    std::fclose(file);
}

void SaveConfig(const Context& context) noexcept {
    wchar_t path[MAX_PATH]{};
    if (!ConfigPath(path, MAX_PATH)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || file == nullptr) return;
    std::fprintf(file, "enabled=%d\nmult=%u\n", context.enabled.load(std::memory_order_relaxed),
                 static_cast<unsigned>(context.multiplier.load(std::memory_order_relaxed)));
    std::fclose(file);
}

// ——— 插件生命周期 ———

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    Context* const context = new (std::nothrow) Context();
    if (context == nullptr) return {ANOMALY_STATUS_V1_FAILED, 0, {nullptr, 0}};

    context->host = host;
    const anomaly::sdk::Host service_host(host);
    context->ui = service_host
                      .Query<AnomalyUiServiceV1>(ANOMALY_UI_SERVICE_V1_ID,
                                                 ANOMALY_UI_SERVICE_V1_VERSION)
                      .get();
    context->hook = service_host
                        .Query<AnomalyHookServiceV1>(ANOMALY_HOOK_SERVICE_V1_ID,
                                                     ANOMALY_HOOK_SERVICE_V1_VERSION)
                        .get();
    context->signature = service_host
                             .Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID,
                                                               ANOMALY_SIGNATURE_SERVICE_V1_VERSION)
                             .get();
    LoadConfig(*context);

    g_context = context;
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return anomaly::sdk::Ok();
    Arm(*context);
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t /*deadline_milliseconds*/) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context != nullptr) Release(*context);
    return anomaly::sdk::Ok();
}

void ANOMALY_CALL Unload(void* plugin_context) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    SaveConfig(*context);
    Release(*context);
    if (g_context == context) g_context = nullptr;
    delete context;
}

void ANOMALY_CALL Update(void* plugin_context, double /*delta_seconds*/) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return;
    if (context->burst_original != nullptr && context->gate_original != nullptr) {
        std::snprintf(context->status, sizeof(context->status), "已就绪   放大 %llu 次 / 追加结算 %llu 次",
                      static_cast<unsigned long long>(context->bursts.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(context->replays.load(std::memory_order_relaxed)));
        return;
    }
    const std::uint64_t now = GetTickCount64();
    if (now < context->next_try_ms) return;
    context->next_try_ms = now + 2000U;  // 游戏模块/Profile 可能还没就位，2 秒后重试
    Arm(*context);
}

void ANOMALY_CALL Draw(void* plugin_context, const AnomalyUiServiceV1* ui) {
    auto* context = static_cast<Context*>(plugin_context);
    if (context == nullptr || ui == nullptr || ui->checkbox == nullptr || ui->text == nullptr ||
        ui->input_uint32 == nullptr) {
        return;
    }
    const anomaly::sdk::UiWindow window(ui, g_identity.title, &context->window_open);
    if (!window) return;

    int enabled = context->enabled.load(std::memory_order_relaxed);
    if (ui->checkbox(ui->user, anomaly::sdk::StringView(kEnableLabel), &enabled) != 0) {
        context->enabled.store(enabled != 0 ? 1 : 0, std::memory_order_relaxed);
        SaveConfig(*context);
    }

    std::uint32_t multiplier = context->multiplier.load(std::memory_order_relaxed);
    if (ui->input_uint32(ui->user, anomaly::sdk::StringView(kMultiplierLabel), &multiplier, 1U,
                         10U) != 0) {
        if (multiplier < kMinMultiplier) multiplier = kMinMultiplier;
        if (multiplier > kMaxMultiplier) multiplier = kMaxMultiplier;
        context->multiplier.store(multiplier, std::memory_order_relaxed);
        SaveConfig(*context);
    }

    ui->text(ui->user, anomaly::sdk::StringView(g_identity.hint));
    ui->text(ui->user, anomaly::sdk::StringView(context->status));
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL
AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(AnomalyPluginDescriptorV1)) {
        return {ANOMALY_STATUS_V1_INVALID_ARGUMENT, 0, {nullptr, 0}};
    }
    LoadManifestIdentity();  // 版本号与提示词的唯一来源；读不到则退回编译期默认值
    descriptor->api_major = ANOMALY_PLUGIN_API_V1_MAJOR;
    descriptor->api_minor = ANOMALY_PLUGIN_API_V1_MINOR;
    descriptor->id = anomaly::sdk::StringView("anomaly.builtin.multi-hit-mini");
    descriptor->name = anomaly::sdk::StringView(kPanelTitle);
    descriptor->author = anomaly::sdk::StringView("Anomaly");
    descriptor->version = anomaly::sdk::StringView(g_identity.version);
    descriptor->on_load = &Load;
    descriptor->on_start = &Start;
    descriptor->on_stop = &Stop;
    descriptor->on_unload = &Unload;
    descriptor->on_update = &Update;
    descriptor->on_draw = &Draw;
    return anomaly::sdk::Ok();
}
