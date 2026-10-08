// ==WindhawkMod==
// @id              taskbar-split-align
// @name            Taskbar Split Alignment
// @description     Puts the Start button (and optionally Search) at the far left while the other taskbar icons stay centered
// @version         0.4.0
// @author          you
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Split Alignment

Moves the Start button (and optionally Search) to the far-left edge of the
screen while the pinned/running app icons stay centered.

How it works: the Windows 11 taskbar layout is produced by the
`TaskbarFrameRepeater` (an ItemsRepeater) inside `Taskbar.TaskbarFrame`. This
mod attaches a XAML visual tree watcher to explorer (the same technique used by
the Windows 11 Taskbar Styler mod), collects the taskbar items as they are
created, and shifts the far-left group horizontally.

Use **Debug logging** first: it logs every taskbar item with its class name,
x position and width, so the grouping can be verified from the Windhawk log.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- leftGroup: "start"
  $name: Left group
  $description: Which buttons are moved to the far left.
  $options:
  - "start": Start only
  - "start_search": Start and Search
- leftPadding: 0
  $name: Extra left padding
  $description: Shifts the left group further left (negative) or right (positive).
- debugLogging: 1
  $name: Debug logging
  $description: Dump the detected taskbar items to the Windhawk log.
*/
// ==/WindhawkModSettings==

#include <windows.h>

// winbase.h defines GetCurrentTime as a function-like macro, which collides
// with a WinRT method name in the XAML projection headers.
#undef GetCurrentTime

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <Unknwn.h>
#include <ocidl.h>
#include <weakreference.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <xamlom.h>
#include <winrt/Windows.UI.Xaml.hosting.h>

namespace wf = winrt::Windows::Foundation;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxm = winrt::Windows::UI::Xaml::Media;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct {
    bool includeSearch;
    int leftPadding;
    bool debugLogging;
} g_settings;

void LoadSettings() {
    PCWSTR leftGroup = Wh_GetStringSetting(L"leftGroup");
    g_settings.includeSearch = (wcscmp(leftGroup, L"start_search") == 0);
    Wh_FreeStringSetting(leftGroup);

    g_settings.leftPadding = Wh_GetIntSetting(L"leftPadding", 0);
    g_settings.debugLogging = Wh_GetIntSetting(L"debugLogging", 0) != 0;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

HMODULE GetCurrentModuleHandle() {
    HMODULE module;
    if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           L"", &module)) {
        return nullptr;
    }
    return module;
}

std::wstring ClassNameOf(wf::IInspectable const& obj) {
    try {
        return std::wstring(winrt::get_class_name(obj));
    } catch (...) {
        return L"<unknown>";
    }
}

// The handle a mutation callback would report for an element.
InstanceHandle HandleFromInspectable(wf::IInspectable const& instance) {
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(reinterpret_cast<::IUnknown*>(winrt::get_abi(instance))
                             ->QueryInterface(winrt::guid_of<wf::IInspectable>(),
                                              inspectable.put_void()));
    return reinterpret_cast<InstanceHandle>(inspectable.get());
}

// XamlDiagnostics implements this interface too, and xamlom.h does not declare
// it.
static constexpr GUID IID_IXamlDiagnosticsTestHooks =
    {0x735941a2, 0x3ee3, 0x495a, {0x8d, 0xa9, 0x97, 0x26, 0x27, 0x00, 0x30, 0x75}};

struct IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle handle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(
        InstanceHandle handle, void** dispatcherQueue) = 0;
};

// ---------------------------------------------------------------------------
// Adaptive pin cadence
// ---------------------------------------------------------------------------
// 任务栏增删图标时，系统会对整组图标做一段位移动画（布局 x 会在 781.6 和
// 803.2 之间平滑过渡）。我们的做法是"采样 -> 补偿 Translation"，所以：
//
//   采样间隔 200ms 时，动画的头 200ms 里开始按钮会跟着整组滑出去
//   （实测最多 21.6 逻辑像素 = 半个按钮），下一个 tick 才被拉回来。
//   动画大约 250ms，于是肉眼看到的就是"抖一下再弹回来"。
//
// 修法：加一个突发模式。只要发现布局在动（动画中）或任务栏元素有增删，
// 就把采样间隔从 200ms 降到 ~12ms，跟着动画逐帧修正，残差降到 1~2 像素
// （肉眼不可见）。动画停稳后再回到 200ms 省 CPU。

static constexpr DWORD kPinIdleIntervalMs = 200;   // 稳态
static constexpr DWORD kPinBurstIntervalMs = 12;   // 突发（约 55~80Hz）
static constexpr ULONGLONG kPinBurstWindowMs = 1000;

std::atomic<ULONGLONG> g_pinBurstUntilMs{0};

// 用事件而不是纯 Sleep：稳态可能正睡在 200ms 里，这时来了布局变化，
// 必须马上把它叫醒，否则第一次修正最多会晚 200ms（按钮就这么滑出去的）。
HANDLE g_pinWakeEvent = nullptr;

void RequestPinBurst() {
    ULONGLONG want = GetTickCount64() + kPinBurstWindowMs;
    ULONGLONG cur = g_pinBurstUntilMs.load(std::memory_order_relaxed);
    // 窗口可以叠加延长，但不缩回，避免多个触发源互相踩。
    while (want > cur && !g_pinBurstUntilMs.compare_exchange_weak(
                             cur, want, std::memory_order_relaxed)) {
    }
    // 自动重置事件：正好唤醒一次等待中的 pin 线程。
    if (g_pinWakeEvent) {
        SetEvent(g_pinWakeEvent);
    }
}

bool PinBurstActive() {
    return GetTickCount64() < g_pinBurstUntilMs.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The visual tree watcher
// ---------------------------------------------------------------------------

// Taskbar items we keep track of.
struct TaskbarItem {
    wux::UIElement element{nullptr};
    wux::FrameworkElement fe{nullptr};
    std::wstring className;
    std::wstring elementName;
    std::wstring parentClassName;
    // Position measured BEFORE any translation, i.e. the layout position.
    double measuredLeft = 0;
    double width = 0;
    bool isLeftGroup = false;
    bool translationCaptured = false;
    winrt::Windows::Foundation::Numerics::float3 originalTranslation{};
};

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2,
                               winrt::non_agile> {
public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site);

    VisualTreeWatcher(const VisualTreeWatcher&) = delete;
    VisualTreeWatcher& operator=(const VisualTreeWatcher&) = delete;
    VisualTreeWatcher(VisualTreeWatcher&&) = delete;
    VisualTreeWatcher& operator=(VisualTreeWatcher&&) = delete;

    ~VisualTreeWatcher();

    void UnadviseVisualTreeChange();

    // Undoes every margin change this mod made.
    void RestoreAll();

    // Re-measures the target and re-applies the translation. Safe to call at
    // any time from the taskbar UI thread; used by the periodic pin timer so
    // that the compensation can never stay stale after a layout change.
    void Pin();

private:
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation, VisualElement element,
        VisualMutationType mutationType) override;
    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle element, VisualElementState elementState,
        LPCWSTR context) noexcept override;

    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable obj;
        winrt::check_hresult(m_XamlDiagnostics->GetIInspectableFromHandle(
            handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));
        return obj;
    }

    bool RememberElement(VisualElement element);
    void ApplySplit();

    // Records what this pin measured. Caller must hold m_itemsMutex.
    void NoteMeasurement(bool measured, double layoutLeft,
                         double prevTranslation);

    // Raw point.X of an element, as reported by TransformToVisual.
    static bool MeasurePointX(wux::UIElement const& element, double* outLeft);

    // Pins an element so that its layout left edge lands on desiredLeft.
    // The translation is cleared first, then measured, then applied, which is
    // correct whether or not TransformToVisual accounts for Translation.
    // Returns the measured layout position, or -1 when it couldn't measure.
    static double PinElement(wux::UIElement const& element, double desiredLeft,
                             double* outPointX, double* outTranslationX);

    winrt::com_ptr<IXamlDiagnostics> m_XamlDiagnostics = nullptr;
    winrt::com_ptr<IXamlDiagnosticsTestHooks> m_XamlDiagnosticsTestHooks =
        nullptr;

    std::mutex m_itemsMutex;
    std::vector<TaskbarItem> m_items;

    // The element we decided to move. Locking on to one element keeps later
    // taskbar changes from hijacking the target.
    wux::UIElement m_target{nullptr};
    double m_targetLayoutLeft = -1;

    // Set by the last measurement: was the layout still moving (animation in
    // progress), and how far off the anchor was the button before we corrected
    // it. Used to drive the burst cadence and to log the residual error.
    bool m_layoutMoving = false;
    double m_lastDeviation = 0;
};

// Caller must hold m_itemsMutex.
void VisualTreeWatcher::NoteMeasurement(bool measured, double layoutLeft,
                                        double prevTranslation) {
    if (!measured) {
        m_layoutMoving = false;
        m_lastDeviation = 0;
        return;
    }

    // 修正前那一刻的可见位置 = 布局位置 + 上一次设的 Translation。
    m_lastDeviation = (layoutLeft + prevTranslation) - g_settings.leftPadding;

    // 布局位置还在变 = 系统动画还没走完，需要继续高频跟踪。
    m_layoutMoving = m_targetLayoutLeft >= 0 &&
                     (layoutLeft - m_targetLayoutLeft > 0.5 ||
                      m_targetLayoutLeft - layoutLeft > 0.5);
}

bool VisualTreeWatcher::RememberElement(VisualElement element) {
    wf::IInspectable inspectable;
    try {
        inspectable = FromHandle(element.Handle);
    } catch (...) {
        return false;
    }

    auto uiElement = inspectable.try_as<wux::UIElement>();
    if (!uiElement) {
        return false;
    }

    std::wstring className = ClassNameOf(inspectable);

    // Only keep elements that live inside the taskbar.
    if (className.find(L"Taskbar") == std::wstring::npos &&
        className.find(L"Search") == std::wstring::npos) {
        return false;
    }

    auto fe = uiElement.try_as<wux::FrameworkElement>();
    if (!fe) {
        return true;  // 任务栏相关，只是不是 FrameworkElement
    }

    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);

        // Don't remember the same element twice.
        for (auto const& existing : m_items) {
            if (existing.element == uiElement) {
                return true;
            }
        }

        TaskbarItem item;
        item.element = uiElement;
        item.fe = fe;
        item.className = className;

        try {
            item.elementName = std::wstring(fe.Name());
        } catch (...) {
            item.elementName = L"";
        }

        // Which container holds this element - essential to understand the
        // taskbar layout.
        try {
            auto parent = wuxm::VisualTreeHelper::GetParent(fe);
            item.parentClassName = parent ? ClassNameOf(parent) : L"<root>";
        } catch (...) {
            item.parentClassName = L"<unknown>";
        }

        item.width = fe.ActualWidth();

        try {
            auto transform = uiElement.TransformToVisual(nullptr);
            auto point = transform.TransformPoint({0.0f, 0.0f});
            item.measuredLeft = point.X;
        } catch (...) {
            item.measuredLeft = -999;
        }

        // The Start button is Taskbar.ExperienceToggleButton named
        // LaunchListButton. Search is a separate repeater item.
        bool isStartButton =
            item.elementName.find(L"LaunchListButton") != std::wstring::npos;
        bool isToggleButton =
            className.find(L"ExperienceToggleButton") != std::wstring::npos;
        bool isSearch = className.find(L"Search") != std::wstring::npos;

        if (isStartButton) {
            item.isLeftGroup = true;
        } else if (isToggleButton && item.elementName.empty()) {
            // Unnamed toggle button: most likely the Start button.
            item.isLeftGroup = true;
        } else if (g_settings.includeSearch && isSearch) {
            item.isLeftGroup = true;
        }

        item.originalTranslation = uiElement.Translation();
        item.translationCaptured = true;

        m_items.push_back(item);
    }

    if (g_settings.debugLogging) {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        auto const& last = m_items.back();
        Wh_Log(L"item: class=%s name=[%s] parent=%s x=%.1f w=%.1f leftGroup=%d",
               last.className.c_str(), last.elementName.c_str(),
               last.parentClassName.c_str(), last.measuredLeft, last.width,
               last.isLeftGroup ? 1 : 0);
    }

    return true;
}

bool VisualTreeWatcher::MeasurePointX(wux::UIElement const& element,
                                      double* outLeft) {
    try {
        auto transform = element.TransformToVisual(nullptr);
        auto point = transform.TransformPoint({0.0f, 0.0f});
        *outLeft = point.X;
        return true;
    } catch (...) {
        return false;
    }
}

double VisualTreeWatcher::PinElement(wux::UIElement const& element,
                                     double desiredLeft, double* outPointX,
                                     double* outTranslationX) {
    winrt::Windows::Foundation::Numerics::float3 saved{};
    try {
        saved = element.Translation();
    } catch (...) {
        return -1;
    }

    // Clear our own offset first, so the measurement below is the position the
    // layout gave the element and nothing else.
    try {
        element.Translation({0.0f, 0.0f, 0.0f});
    } catch (...) {
        return -1;
    }

    double pointX = 0;
    if (!MeasurePointX(element, &pointX)) {
        try {
            element.Translation(saved);
        } catch (...) {
        }
        return -1;
    }

    if (outPointX) {
        *outPointX = pointX;
    }
    if (outTranslationX) {
        *outTranslationX = saved.x;
    }

    double left = pointX;
    if (left <= 0 || left > 6000) {
        // Not laid out yet, or the element is somewhere absurd: leave it alone.
        try {
            element.Translation({0.0f, 0.0f, 0.0f});
        } catch (...) {
        }
        return -1;
    }

    float dx = (float)(desiredLeft - left);
    if (dx < -4000.0f) {
        dx = -4000.0f;
    }
    if (dx > 4000.0f) {
        dx = 4000.0f;
    }

    try {
        element.Translation({dx, 0.0f, 0.0f});
    } catch (...) {
        return -1;
    }

    return left;
}

void VisualTreeWatcher::ApplySplit() {
    std::lock_guard<std::mutex> lock(m_itemsMutex);

    if (m_items.empty()) {
        return;
    }

    double desiredLeft = (double)g_settings.leftPadding;

    // 1. Re-use the element we locked on to before.
    if (m_target) {
        double pointX = 0, translationX = 0;
        double left = PinElement(m_target, desiredLeft, &pointX, &translationX);
        if (left >= 0) {
            NoteMeasurement(true, left, translationX);
            m_targetLayoutLeft = left;
            if (g_settings.debugLogging) {
                Wh_Log(L"split/locked: pointX=%.1f transX=%.1f layoutLeft=%.1f "
                       L"dev=%.2f -> %.1f",
                       pointX, translationX, left, m_lastDeviation, desiredLeft);
            }
            return;
        }
        m_target = nullptr;
        m_targetLayoutLeft = -1;
        NoteMeasurement(false, 0, 0);
    }

    // 2. Pick a target: prefer the named Start button.
    wux::UIElement namedTarget{nullptr};
    double namedPointX = 0;
    for (auto const& item : m_items) {
        if (!item.element) {
            continue;
        }
        if (item.elementName.find(L"LaunchListButton") == std::wstring::npos) {
            continue;
        }
        double pointX = 0;
        if (MeasurePointX(item.element, &pointX) && pointX > 0 &&
            pointX < 6000) {
            namedTarget = item.element;
            namedPointX = pointX;
            break;
        }
    }

    if (namedTarget) {
        double pointX = 0, translationX = 0;
        double left =
            PinElement(namedTarget, desiredLeft, &pointX, &translationX);
        if (left >= 0) {
            m_target = namedTarget;
            NoteMeasurement(true, left, translationX);
            m_targetLayoutLeft = left;
            if (g_settings.debugLogging) {
                Wh_Log(L"split/new(named): pointX=%.1f transX=%.1f "
                       L"layoutLeft=%.1f dev=%.2f -> %.1f",
                       pointX, translationX, left, m_lastDeviation, desiredLeft);
            }
        }
        return;
    }

    // 3. No named Start button: fall back to the leftmost toggle button whose
    //    position is plausible.
    wux::UIElement fallback{nullptr};
    double fallbackLeft = 1e9;
    for (auto const& item : m_items) {
        if (!item.isLeftGroup || !item.element) {
            continue;
        }
        if (item.className.find(L"Search") != std::wstring::npos) {
            continue;
        }
        double pointX = 0;
        if (!MeasurePointX(item.element, &pointX)) {
            continue;
        }
        if (pointX <= 0 || pointX > 6000) {
            continue;
        }
        if (pointX < fallbackLeft) {
            fallbackLeft = pointX;
            fallback = item.element;
        }
    }

    if (!fallback) {
        if (g_settings.debugLogging) {
            Wh_Log(L"split: no usable target found");
        }
        return;
    }

    double pointX = 0, translationX = 0;
    double left = PinElement(fallback, desiredLeft, &pointX, &translationX);
    if (left >= 0) {
        m_target = fallback;
        NoteMeasurement(true, left, translationX);
        m_targetLayoutLeft = left;
        if (g_settings.debugLogging) {
            Wh_Log(L"split/new(fallback): pointX=%.1f transX=%.1f "
                   L"layoutLeft=%.1f dev=%.2f -> %.1f",
                   pointX, translationX, left, m_lastDeviation, desiredLeft);
        }
    }
}

void VisualTreeWatcher::Pin() {
    ApplySplit();

    // 布局还在动 = 系统动画没结束，继续保持高频采样，别让它滑出去。
    bool moving;
    {
        std::lock_guard<std::mutex> lock(m_itemsMutex);
        moving = m_layoutMoving;
    }
    if (moving) {
        RequestPinBurst();
    }
}

// Restores every translation this mod applied.
void VisualTreeWatcher::RestoreAll() {    std::lock_guard<std::mutex> lock(m_itemsMutex);

    int restored = 0;

    // Also undo the currently locked target, which may not be in m_items.
    if (m_target) {
        try {
            m_target.Translation({0.0f, 0.0f, 0.0f});
            restored++;
        } catch (...) {
        }
        m_target = nullptr;
        m_targetLayoutLeft = -1;
    }

    for (auto& item : m_items) {
        if (!item.translationCaptured || !item.element) {
            continue;
        }
        try {
            item.element.Translation(item.originalTranslation);
            restored++;
        } catch (...) {
        }
    }

    Wh_Log(L"restore: %d element(s) restored", restored);
}

VisualTreeWatcher::VisualTreeWatcher(winrt::com_ptr<IUnknown> site) :
    m_XamlDiagnostics(site.as<IXamlDiagnostics>()) {
    Wh_Log(L"Constructing VisualTreeWatcher");

    // 刚注入时任务栏往往还在建/还在动画，直接进突发模式，让它尽快钉住。
    RequestPinBurst();

    HRESULT hr = m_XamlDiagnostics->QueryInterface(
        IID_IXamlDiagnosticsTestHooks, m_XamlDiagnosticsTestHooks.put_void());
    if (FAILED(hr)) {
        Wh_Log(L"IXamlDiagnosticsTestHooks unavailable: %08X", hr);
    }

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID lpParam) -> DWORD {
            auto watcher = reinterpret_cast<VisualTreeWatcher*>(lpParam);
            auto service = watcher->m_XamlDiagnostics.as<IVisualTreeService3>();
            HRESULT hr = service->AdviseVisualTreeChange(watcher);
            watcher->Release();
            if (FAILED(hr)) {
                Wh_Log(L"AdviseVisualTreeChange failed: %08X", hr);
            }
            return 0;
        },
        this, 0, nullptr);
    if (thread) {
        AddRef();
        CloseHandle(thread);
    }
}

VisualTreeWatcher::~VisualTreeWatcher() {
    Wh_Log(L"Destructing VisualTreeWatcher");
}

void VisualTreeWatcher::UnadviseVisualTreeChange() {
    HRESULT hr =
        m_XamlDiagnostics.as<IVisualTreeService3>()->UnadviseVisualTreeChange(
            this);
    if (FAILED(hr)) {
        Wh_Log(L"UnadviseVisualTreeChange failed: %08X", hr);
    }
}

HRESULT VisualTreeWatcher::OnVisualTreeChange(ParentChildRelation,
                                              VisualElement element,
                                              VisualMutationType mutationType) try {
    if (mutationType != Add && mutationType != Remove) {
        return S_OK;
    }

    try {
        bool taskbarRelated = false;

        if (mutationType == Add) {
            taskbarRelated = RememberElement(element);
        } else {
            // 删除时元素已经不在树里，这里只做一次廉价判断：能读出类名就过滤，
            // 读不出来就走 200ms 兜底轮询（布局变化检测）再进入突发模式。
            try {
                auto inspectable = FromHandle(element.Handle);
                std::wstring className = ClassNameOf(inspectable);
                taskbarRelated =
                    className.find(L"Taskbar") != std::wstring::npos ||
                    className.find(L"Search") != std::wstring::npos;
            } catch (...) {
            }
        }

        // 任务栏增删图标 -> 重排动画马上开始，立刻切到高频采样，
        // 否则要等下一次 200ms 轮询才发现（那段时间开始按钮会跟着滑）。
        if (taskbarRelated) {
            RequestPinBurst();
        }

        // The translation is NOT applied here: this callback fires before the
        // taskbar's layout has settled, so measuring now would give a stale
        // position. The pin timer (burst or idle) does the actual work.
    } catch (...) {
        Wh_Log(L"Error %08X", winrt::to_hresult());
    }

    return S_OK;
} catch (...) {
    return S_OK;
}

HRESULT VisualTreeWatcher::OnElementStateChanged(InstanceHandle,
                                                 VisualElementState,
                                                 LPCWSTR) noexcept {
    return S_OK;
}

// ---------------------------------------------------------------------------
// TAP (the diagnostics consumer that gets injected into explorer)
// ---------------------------------------------------------------------------

winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;

// The taskbar window, needed to run restore code on the taskbar UI thread.
HWND g_taskbarUiWnd = nullptr;

// {C85D8CC7-5463-40E8-A432-F5916B6427E5}
static constexpr CLSID CLSID_WindhawkTAP = {
    0xc85d8cc7, 0x5463, 0x40e8, {0xa4, 0x32, 0xf5, 0x91, 0x6b, 0x64, 0x27, 0xe5}};

class WindhawkTAP
    : public winrt::implements<WindhawkTAP, IObjectWithSite, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override;

private:
    winrt::com_ptr<IUnknown> site;
};

HRESULT WindhawkTAP::SetSite(IUnknown* pUnkSite) try {
    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->UnadviseVisualTreeChange();
        g_visualTreeWatcher = nullptr;
    }

    site.copy_from(pUnkSite);

    if (site) {
        // Decrease refcount increased by InitializeXamlDiagnosticsEx.
        FreeLibrary(GetCurrentModuleHandle());
        g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(site);
    }

    return S_OK;
} catch (...) {
    HRESULT hr = winrt::to_hresult();
    Wh_Log(L"Error %08X", hr);
    return hr;
}

HRESULT WindhawkTAP::GetSite(REFIID riid, void** ppvSite) noexcept {
    return site.as(riid, ppvSite);
}

template <class T>
struct SimpleFactory
    : winrt::implements<SimpleFactory<T>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid,
                                             void** ppvObject) override try {
        if (!pUnkOuter) {
            *ppvObject = nullptr;
            return winrt::make<T>().as(riid, ppvObject);
        }
        return CLASS_E_NOAGGREGATION;
    } catch (...) {
        HRESULT hr = winrt::to_hresult();
        Wh_Log(L"Error %08X", hr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"

__declspec(dllexport) _Use_decl_annotations_ STDAPI
    DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) try {
    if (rclsid == CLSID_WindhawkTAP) {
        *ppv = nullptr;
        return winrt::make<SimpleFactory<WindhawkTAP>>().as(riid, ppv);
    }
    return CLASS_E_CLASSNOTAVAILABLE;
} catch (...) {
    HRESULT hr = winrt::to_hresult();
    Wh_Log(L"Error %08X", hr);
    return hr;
}

__declspec(dllexport) _Use_decl_annotations_ STDAPI DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

#pragma clang diagnostic pop

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX =
    decltype(&InitializeXamlDiagnosticsEx);

bool g_inInjectWindhawkTAP = false;

HRESULT InjectWindhawkTAP() noexcept {
    HMODULE module = GetCurrentModuleHandle();
    if (!module) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR location[MAX_PATH];
    switch (GetModuleFileName(module, location, ARRAYSIZE(location))) {
        case 0:
        case ARRAYSIZE(location):
            return HRESULT_FROM_WIN32(GetLastError());
    }

    const HMODULE wux = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!wux) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const auto ixde = reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(
        GetProcAddress(wux, "InitializeXamlDiagnosticsEx"));
    if (!ixde) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_inInjectWindhawkTAP = true;

    HRESULT hr = E_FAIL;
    for (int i = 0; i < 10000; i++) {
        WCHAR connectionName[256];
        wsprintf(connectionName, L"VisualDiagConnection%d", i + 1);

        hr = ixde(connectionName, GetCurrentProcessId(), L"", location,
                  CLSID_WindhawkTAP, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    g_inInjectWindhawkTAP = false;

    return hr;
}

// ---------------------------------------------------------------------------
// Window/thread plumbing
// ---------------------------------------------------------------------------

using RunFromWindowThreadProc_t = void(WINAPI*)(PVOID parameter);

bool RunFromWindowThread(HWND hWnd, RunFromWindowThreadProc_t proc,
                         PVOID procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        PVOID procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message ==
                    RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID)) {
                    auto param = (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);

    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param{proc, procParam};
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

BOOL CALLBACK EnumWindowsCallback(HWND hWnd, LPARAM lParam) {
    auto* found = reinterpret_cast<HWND*>(lParam);

    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hWnd, &dwProcessId);
    if (dwProcessId != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR className[256];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) {
        return TRUE;
    }

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0) {
        *found = hWnd;
        return FALSE;
    }

    return TRUE;
}

struct ChildSearchContext {
    HWND found;
    PCWSTR wantedClassName;
};

BOOL CALLBACK EnumChildWindowsCallback(HWND hWnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<ChildSearchContext*>(lParam);

    WCHAR className[256];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) {
        return TRUE;
    }

    if (_wcsicmp(className, ctx->wantedClassName) == 0) {
        ctx->found = hWnd;
        return FALSE;
    }

    return TRUE;
}

// The XAML island lives in a child window of the taskbar:
//   Shell_TrayWnd
//     -> Windows.UI.Composition.DesktopWindowContentBridge
HWND FindChildWindow(HWND parent, PCWSTR className) {
    ChildSearchContext ctx{nullptr, className};
    EnumChildWindows(parent, EnumChildWindowsCallback, (LPARAM)&ctx);
    return ctx.found;
}

HWND GetTaskbarUiWnd() {
    HWND trayWnd = nullptr;
    EnumWindows(EnumWindowsCallback, (LPARAM)&trayWnd);
    if (!trayWnd) {
        return nullptr;
    }

    HWND bridge = FindChildWindow(
        trayWnd, L"Windows.UI.Composition.DesktopWindowContentBridge");
    if (bridge) {
        return bridge;
    }

    // Fall back to the CoreWindow, which also hosts XAML content.
    return FindChildWindow(trayWnd, L"Windows.UI.Core.CoreWindow");
}

void InitializeForCurrentThread() {
    Wh_Log(L">");

    HRESULT hr = InjectWindhawkTAP();
    if (FAILED(hr)) {
        Wh_Log(L"InjectWindhawkTAP failed: %08X", hr);
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

BOOL Wh_ModInit() {
    LoadSettings();
    Wh_Log(L"Taskbar Split Alignment init: includeSearch=%d leftPadding=%d",
           g_settings.includeSearch ? 1 : 0, g_settings.leftPadding);
    return TRUE;
}

void RetryThreadProc();
HANDLE g_retryThreadHandle = nullptr;
std::atomic<bool> g_stopRetry{false};

void PinThreadProc();
HANDLE g_pinThreadHandle = nullptr;
std::atomic<bool> g_stopPin{false};

void Wh_ModAfterInit() {
    Wh_Log(L">");

    // 唤醒事件：自动重置，用来打断 pin 线程的等待。
    if (!g_pinWakeEvent) {
        g_pinWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    // The periodic re-pin timer. This MUST be started before any early return
    // below: in the common case the taskbar window already exists, and the old
    // ordering returned from the If-branch without ever starting the timer, so
    // the translation was never applied at all.
    g_stopPin = false;
    g_pinThreadHandle = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD { PinThreadProc(); return 0; }, nullptr, 0,
        nullptr);
    if (g_pinThreadHandle) {
        Wh_Log(L"Pin thread started");
    } else {
        Wh_Log(L"Pin CreateThread failed: %u", GetLastError());
    }

    // 启动那一下也马上钉一次，不用等第一个 tick。
    RequestPinBurst();

    HWND hTaskbarUiWnd = GetTaskbarUiWnd();
    if (hTaskbarUiWnd) {
        Wh_Log(L"Found taskbar UI window %08X", (DWORD)(ULONG_PTR)hTaskbarUiWnd);
        g_taskbarUiWnd = hTaskbarUiWnd;
        RunFromWindowThread(
            hTaskbarUiWnd, [](PVOID) { InitializeForCurrentThread(); },
            nullptr);
        return;
    }

    // The taskbar window is usually created after Wh_ModAfterInit runs, so
    // keep looking for it in the background.
    Wh_Log(L"No taskbar UI window yet, will retry in the background");

    g_stopRetry = false;
    g_retryThreadHandle = CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD { RetryThreadProc(); return 0; }, nullptr, 0,
        nullptr);
    if (g_retryThreadHandle) {
        Wh_Log(L"Background retry thread started");
    } else {
        Wh_Log(L"CreateThread failed: %u", GetLastError());
    }
}

// Watches for the taskbar UI window and initializes the TAP once it exists.
void RetryThreadProc() {
    Wh_Log(L"Background: retry thread running");

    for (int attempt = 0; attempt < 120 && !g_stopRetry; attempt++) {
        Sleep(500);

        HWND hTaskbarUiWnd = GetTaskbarUiWnd();
        if (!hTaskbarUiWnd) {
            if (attempt % 10 == 9) {
                Wh_Log(L"Background: still no taskbar window (attempt %d)",
                       attempt);
            }
            continue;
        }

        Wh_Log(L"Background: found taskbar UI window %08X (attempt %d)",
               (DWORD)(ULONG_PTR)hTaskbarUiWnd, attempt);

        g_taskbarUiWnd = hTaskbarUiWnd;

        RunFromWindowThread(
            hTaskbarUiWnd, [](PVOID) { InitializeForCurrentThread(); },
            nullptr);

        // A watcher is enough; stop looking once it was created.
        if (g_visualTreeWatcher) {
            Wh_Log(L"Background: visual tree watcher installed");
            return;
        }
    }

    Wh_Log(L"Background: gave up looking for the taskbar UI window");
}

// ---------------------------------------------------------------------------
// Periodic pin timer (adaptive cadence)
// ---------------------------------------------------------------------------
// The visual tree change callback fires before the taskbar layout has settled,
// so a measurement taken there is stale. Re-measuring and re-applying the
// translation continuously makes the compensation self-correcting no matter
// when the layout actually settles.
//
// 稳态 200ms 一次；一旦有任务栏元素增删、或发现布局还在变（系统重排动画中），
// 就切到 ~12ms 一次跟着动画修正，动画停稳 1 秒后再降回来。
void PinThreadProc() {
    Wh_Log(L"Pin thread running");

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    bool wasBursting = false;

    while (!g_stopPin) {
        bool bursting = PinBurstActive();
        DWORD waitMs = bursting ? kPinBurstIntervalMs : kPinIdleIntervalMs;
        // 等事件或超时：任何一种触发（元素增删 / 布局变化）都能立刻打断这次等待。
        if (g_pinWakeEvent) {
            WaitForSingleObject(g_pinWakeEvent, waitMs);
        } else {
            Sleep(waitMs);
        }
        if (g_stopPin) {
            break;
        }

        if (bursting != wasBursting) {
            wasBursting = bursting;
            if (g_settings.debugLogging) {
                // Wh_Log 的 message 必须是字面量（会被拼到 L"[%d:%S]: " 后面）。
                if (bursting) {
                    Wh_Log(L"pin: burst ON");
                } else {
                    Wh_Log(L"pin: burst OFF");
                }
            }
        }

        HWND taskbarWnd = g_taskbarUiWnd;
        if (!taskbarWnd) {
            continue;
        }

        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);

        RunFromWindowThread(
            taskbarWnd,
            [](PVOID) {
                if (g_visualTreeWatcher) {
                    g_visualTreeWatcher->Pin();
                }
            },
            nullptr);

        QueryPerformanceCounter(&t1);
        double tickMs = freq.QuadPart
                            ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                                  (double)freq.QuadPart
                            : 0.0;
        // 只在明显偏慢时打点，免得测量本身影响测量结果。
        if (g_settings.debugLogging && tickMs > 3.0) {
            Wh_Log(L"pin: tick took %.1f ms", tickMs);
        }
    }

    Wh_Log(L"Pin thread exiting");
}

void Wh_ModUninit() {
    Wh_Log(L">");

    g_stopRetry = true;
    if (g_retryThreadHandle) {
        WaitForSingleObject(g_retryThreadHandle, 2000);
        CloseHandle(g_retryThreadHandle);
        g_retryThreadHandle = nullptr;
    }

    g_stopPin = true;
    if (g_pinWakeEvent) {
        SetEvent(g_pinWakeEvent);  // 立刻叫醒，不用等最多 200ms
    }
    if (g_pinThreadHandle) {
        WaitForSingleObject(g_pinThreadHandle, 2000);
        CloseHandle(g_pinThreadHandle);
        g_pinThreadHandle = nullptr;
    }
    if (g_pinWakeEvent) {
        CloseHandle(g_pinWakeEvent);
        g_pinWakeEvent = nullptr;
    }

    // Undo the margin changes on the taskbar UI thread before dropping the
    // watcher, so that disabling the mod restores the original taskbar.
    if (g_visualTreeWatcher) {
        HWND taskbarWnd = g_taskbarUiWnd;
        if (taskbarWnd) {
            RunFromWindowThread(
                taskbarWnd,
                [](PVOID) {
                    if (g_visualTreeWatcher) {
                        g_visualTreeWatcher->RestoreAll();
                        g_visualTreeWatcher->UnadviseVisualTreeChange();
                        g_visualTreeWatcher = nullptr;
                    }
                },
                nullptr);
        } else {
            g_visualTreeWatcher->RestoreAll();
            g_visualTreeWatcher->UnadviseVisualTreeChange();
            g_visualTreeWatcher = nullptr;
        }
    }

    g_taskbarUiWnd = nullptr;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    Wh_Log(L"Taskbar Split Alignment: settings changed");
}