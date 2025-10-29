// desktop_drop_plugin.cpp
// - Async channel dispatch via root subclass messages (no MsgHost window)
// - Runtime logging toggle (Debug default ON, Release default OFF)
// - No CBT hook, no scattered macros

#include "include/desktop_drop/desktop_drop_plugin.h"

#include <windows.h>
#include <shellapi.h>
#include <ole2.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// -------------------- Runtime logging --------------------
namespace logging {
  inline bool default_enabled() {
  #ifdef NDEBUG
    return false; // Release
  #else
    return true;  // Debug
  #endif
  }

  inline std::atomic<bool>& flag() {
    static std::atomic<bool> f(default_enabled());
    return f;
  }
  inline bool enabled() { return flag().load(std::memory_order_relaxed); }
  inline void set_enabled(bool on) { flag().store(on, std::memory_order_relaxed); }

  inline void logf(const char* fmt, ...) {
    if (!enabled()) return;
    char buf[2048];
    va_list ap; va_start(ap, fmt);
  #if defined(_MSC_VER)
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  #else
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  #endif
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';

    std::string s = std::string("[desktop_drop] ") + buf + "\n";
    OutputDebugStringA(s.c_str());
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    std::cout << s;
  }
} // namespace logging

// -------------------- Small utils --------------------

template <typename T>
inline void UNUSED(const T&) {}

inline uint64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string ws2s(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s; s.resize(n);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

static DWORD ChooseAllowedEffect(DWORD allowed) {
  if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
  if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
  if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
  return DROPEFFECT_NONE;
}
static DWORD EffectFromKeyState(DWORD key, DWORD allowed) {
  DWORD want = DROPEFFECT_NONE;
  if ((key & MK_CONTROL) && (key & MK_SHIFT)) want = DROPEFFECT_LINK;
  else if (key & MK_CONTROL) want = DROPEFFECT_COPY;
  else if (key & MK_SHIFT)   want = DROPEFFECT_MOVE;
  else                       want = ChooseAllowedEffect(allowed);
  return (want & allowed) ? want : ChooseAllowedEffect(allowed);
}

static const wchar_t* kRootPropName = L"DesktopDrop.RootThisPtr";

// -------------------- Async UI event payload --------------------

enum class UiEventType { Entered, Updated, Exited, Performed };

struct UiEventPayload {
  UiEventType type;
  double x = 0;
  double y = 0;
  flutter::EncodableList* files = nullptr; // owned pointer when type == Performed
};

// Forward declare target so registrar can hold a pointer.
class DesktopDropTarget;

// -------------------- DropRegistrar (declarations) --------------------

class DropRegistrar {
public:
  static DropRegistrar& Instance();

  void Start(HWND root, DesktopDropTarget* target);
  void Stop();

  void EnsureRegisteredUnderPoint(POINTL pt_screen);
  void RequestReRegisterAll();
  void OnDragSessionEnd();

  bool IsDragging() const;
  void OnWmDropFiles(HDROP hdrop);

  void PostUiEvent(UiEventPayload* ev); // ownership transfers

  HWND Root() const { return root_; }

private:
  DropRegistrar() = default;
  ~DropRegistrar() = default;

  // Messages delivered via root window subclass WndProc
  static constexpr UINT kMsgDoReRegister = WM_APP + 0x501;
  static constexpr UINT kMsgFireUiEvent  = WM_APP + 0x502;

  // Static callbacks
  static LRESULT CALLBACK RootSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  static BOOL     CALLBACK EnumChildProc(HWND w, LPARAM lp);

  // Helpers
  void SubclassRootForDropfiles();
  void UnsubclassRootForDropfiles();
  void RegisterOnWindow(HWND w);
  void RevokeOnWindow(HWND w);
  void ForceReRegisterAll();
  void DoReRegister(HWND w);
  void FlushPendingChildOps();

private:
  HWND root_ = nullptr;
  DesktopDropTarget* target_ = nullptr;

  std::unordered_set<HWND> registered_;
  std::vector<HWND> pending_add_;
  std::vector<HWND> pending_del_;
  bool pending_re_reg_ = false;

  WNDPROC old_root_proc_ = nullptr;
};

// -------------------- DesktopDropTarget (definition) --------------------

class DesktopDropTarget : public IDropTarget {
public:
  DesktopDropTarget(std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel,
                    HWND window_handle)
      : channel_(std::move(channel)), window_handle_(window_handle),
        ref_count_(1), need_revoke_ole_initialize_(false) {

    logging::logf("DesktopDropTarget() this=%p hwnd=%p ref=%ld",
                  this, window_handle_, (long)ref_count_);

    HRESULT ret = RegisterDragDrop(window_handle_, this);
    logging::logf("RegisterDragDrop first ret=0x%08lX", (unsigned long)ret);
    if (ret == CO_E_NOTINITIALIZED || ret == E_OUTOFMEMORY || FAILED(ret)) {
      logging::logf("calling OleInitialize(nullptr)...");
      OleInitialize(nullptr);
      ret = RegisterDragDrop(window_handle_, this);
      logging::logf("RegisterDragDrop retry ret=0x%08lX", (unsigned long)ret);
      if (SUCCEEDED(ret)) {
        need_revoke_ole_initialize_ = true;
        logging::logf("need_revoke_ole_initialize_=true");
      }
    }

    DragAcceptFiles(window_handle_, TRUE);
    DropRegistrar::Instance().Start(window_handle_, this);
  }

  // IDropTarget
  HRESULT DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
    in_ole_drop_.store(true, std::memory_order_relaxed);

    if (pdwEffect) {
      DWORD allowed = *pdwEffect ? *pdwEffect : (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
      *pdwEffect = EffectFromKeyState(grfKeyState, allowed);
      if (*pdwEffect == DROPEFFECT_NONE) *pdwEffect = ChooseAllowedEffect(allowed);
    }

    DropRegistrar::Instance().EnsureRegisteredUnderPoint(pt);

    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    HRESULT q = pDataObj ? pDataObj->QueryGetData(&fmt) : E_POINTER;
    POINT sp{ pt.x, pt.y }; HWND under = WindowFromPoint(sp);
    logging::logf(
      "DragEnter this=%p inEff=0x%08lX outEff=0x%08lX keys=0x%08lX CF_HDROP?=%s pt=(%ld,%ld) hwnd_under=%p",
      this,
      (unsigned long)(pdwEffect ? *pdwEffect : 0),
      (unsigned long)(pdwEffect ? *pdwEffect : 0),
      (unsigned long)grfKeyState,
      (q == S_OK ? "yes" : "no"),
      (long)pt.x, (long)pt.y,
      under
    );

    POINT client{ pt.x, pt.y }; ScreenToClient(window_handle_, &client);
    auto ev = new UiEventPayload{ UiEventType::Entered, double(client.x), double(client.y), nullptr };
    DropRegistrar::Instance().PostUiEvent(ev);
    return S_OK;
  }

  HRESULT DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
    if (pdwEffect) {
      DWORD allowed = *pdwEffect ? *pdwEffect : (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
      *pdwEffect = EffectFromKeyState(grfKeyState, allowed);
      if (*pdwEffect == DROPEFFECT_NONE) *pdwEffect = ChooseAllowedEffect(allowed);
    }

    POINT client{ pt.x, pt.y }; ScreenToClient(window_handle_, &client);
    auto ev = new UiEventPayload{ UiEventType::Updated, double(client.x), double(client.y), nullptr };
    DropRegistrar::Instance().PostUiEvent(ev);
    return S_OK;
  }

  HRESULT DragLeave() override {
    logging::logf("DragLeave this=%p", this);
    in_ole_drop_.store(false, std::memory_order_relaxed);

    auto ev = new UiEventPayload{ UiEventType::Exited, 0, 0, nullptr };
    DropRegistrar::Instance().PostUiEvent(ev);

    DropRegistrar::Instance().OnDragSessionEnd();
    last_drop_ms_.store(NowMs(), std::memory_order_relaxed);
    return S_OK;
  }

  HRESULT Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
    if (pdwEffect) {
      DWORD allowed = *pdwEffect ? *pdwEffect : (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
      *pdwEffect = EffectFromKeyState(grfKeyState, allowed);
      if (*pdwEffect == DROPEFFECT_NONE) *pdwEffect = ChooseAllowedEffect(allowed);
    }

    logging::logf("Drop this=%p inEff=0x%08lX outEff=0x%08lX keys=0x%08lX pt=(%ld,%ld)",
                  this,
                  (unsigned long)(pdwEffect ? *pdwEffect : 0),
                  (unsigned long)(pdwEffect ? *pdwEffect : 0),
                  (unsigned long)grfKeyState,
                  (long)pt.x, (long)pt.y);

    auto files_list = std::make_unique<flutter::EncodableList>();

    FORMATETC fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stg{};
    if (pDataObj && pDataObj->QueryGetData(&fmt) == S_OK) {
      HRESULT gd = pDataObj->GetData(&fmt, &stg);
      logging::logf("GetData ret=0x%08lX", (unsigned long)gd);
      if (gd == S_OK) {
        PVOID data = GlobalLock(stg.hGlobal);
        if (data) {
          UINT count = DragQueryFile(reinterpret_cast<HDROP>(stg.hGlobal), 0xFFFFFFFF, nullptr, 0);
          logging::logf("HDROP files=%lu", (unsigned long)count);
          for (UINT i = 0; i < count; ++i) {
            wchar_t fn[MAX_PATH]{}; DragQueryFile(reinterpret_cast<HDROP>(stg.hGlobal), i, fn, MAX_PATH);
            std::string u8 = ws2s(fn);
            files_list->emplace_back(u8);
            logging::logf("done: %s", u8.c_str());
          }
          GlobalUnlock(stg.hGlobal);
        } else {
          logging::logf("GlobalLock failed");
        }
        ReleaseStgMedium(&stg);
      }
    } else {
      logging::logf("QueryGetData CF_HDROP not available");
    }

    auto evPerformed = new UiEventPayload{ UiEventType::Performed, 0, 0, files_list.release() };
    DropRegistrar::Instance().PostUiEvent(evPerformed);

    auto evExited = new UiEventPayload{ UiEventType::Exited, 0, 0, nullptr };
    DropRegistrar::Instance().PostUiEvent(evExited);

    in_ole_drop_.store(false, std::memory_order_relaxed);
    DropRegistrar::Instance().OnDragSessionEnd();
    last_drop_ms_.store(NowMs(), std::memory_order_relaxed);
    return S_OK;
  }

  // IUnknown
  HRESULT QueryInterface(const IID& iid, void** ppv) override {
    if (iid == IID_IDropTarget || iid == IID_IUnknown) { AddRef(); *ppv = this; return S_OK; }
    *ppv = nullptr; return E_NOINTERFACE;
  }
  ULONG AddRef() override {
    ULONG r = InterlockedIncrement(&ref_count_);
    logging::logf("AddRef this=%p ref=%lu", this, (unsigned long)r);
    return r;
  }
  ULONG Release() override {
    LONG c = InterlockedDecrement(&ref_count_);
    logging::logf("Release this=%p ref=%ld", this, (long)c);
    if (c == 0) { logging::logf("Release deleting this=%p", this); delete this; return 0; }
    return (ULONG)c;
  }

  virtual ~DesktopDropTarget() {
    DropRegistrar::Instance().Stop();
    HRESULT rv = RevokeDragDrop(window_handle_);
    logging::logf("~DesktopDropTarget this=%p RevokeDragDrop ret=0x%08lX need_revoke_ole_initialize_=%s",
                  this, (unsigned long)rv, need_revoke_ole_initialize_ ? "true" : "false");
    if (need_revoke_ole_initialize_) {
      logging::logf("OleUninitialize()");
      OleUninitialize();
    }
  }

  bool IsDragging() const { return in_ole_drop_.load(std::memory_order_relaxed); }

  // Async dispatch on UI thread via root window messages
  void DeliverUiEvent(UiEventPayload* ev) {
    switch (ev->type) {
      case UiEventType::Entered:
        channel_->InvokeMethod("entered",
          std::make_unique<flutter::EncodableValue>(flutter::EncodableList{
            flutter::EncodableValue(ev->x), flutter::EncodableValue(ev->y) }));
        break;
      case UiEventType::Updated:
        channel_->InvokeMethod("updated",
          std::make_unique<flutter::EncodableValue>(flutter::EncodableList{
            flutter::EncodableValue(ev->x), flutter::EncodableValue(ev->y) }));
        break;
      case UiEventType::Exited:
        channel_->InvokeMethod("exited", std::make_unique<flutter::EncodableValue>());
        break;
      case UiEventType::Performed: {
        std::unique_ptr<flutter::EncodableList> files(ev->files);
        channel_->InvokeMethod("performOperation",
          std::make_unique<flutter::EncodableValue>(*files));
        break;
      }
    }
  }

  void HandleDropFiles(HDROP hdrop) {
    if (IsDragging()) return;

    uint64_t now = NowMs();
    if (now - last_drop_ms_.load(std::memory_order_relaxed) < 200) return;

    POINT pt{}; BOOL client = DragQueryPoint(hdrop, &pt);
    if (!client) {
      POINT scr{}; DragQueryPoint(hdrop, &scr);
      ScreenToClient(window_handle_, &scr); pt = scr;
    }

    auto evEnter = new UiEventPayload{ UiEventType::Entered, double(pt.x), double(pt.y), nullptr };
    DropRegistrar::Instance().PostUiEvent(evEnter);

    UINT num = DragQueryFile(hdrop, 0xFFFFFFFF, nullptr, 0);
    logging::logf("WM_DROPFILES files=%lu", (unsigned long)num);
    auto list = std::make_unique<flutter::EncodableList>();
    for (UINT i = 0; i < num; ++i) {
      wchar_t path[MAX_PATH]{}; DragQueryFile(hdrop, i, path, MAX_PATH);
      std::string s = ws2s(path);
      list->emplace_back(s);
      logging::logf("done: %s", s.c_str());
    }
    auto evPerformed = new UiEventPayload{ UiEventType::Performed, 0, 0, list.release() };
    DropRegistrar::Instance().PostUiEvent(evPerformed);

    auto evExit = new UiEventPayload{ UiEventType::Exited, 0, 0, nullptr };
    DropRegistrar::Instance().PostUiEvent(evExit);

    logging::logf("WM_DROPFILES seen; request deferred OLE re-register");
    DropRegistrar::Instance().RequestReRegisterAll();

    last_drop_ms_.store(NowMs(), std::memory_order_relaxed);
  }

private:
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  HWND window_handle_;
  LONG ref_count_;
  bool need_revoke_ole_initialize_;
  std::atomic<bool> in_ole_drop_{ false };
  std::atomic<uint64_t> last_drop_ms_{ 0 };
};

// -------------------- DropRegistrar (definitions) --------------------

DropRegistrar& DropRegistrar::Instance() {
  static DropRegistrar inst; return inst;
}

void DropRegistrar::Start(HWND root, DesktopDropTarget* target) {
  root_ = root;
  target_ = target;

  DragAcceptFiles(root_, TRUE);
  SubclassRootForDropfiles();

  RegisterOnWindow(root_);
  EnumChildWindows(root_, EnumChildProc, reinterpret_cast<LPARAM>(this));
}

void DropRegistrar::Stop() {
  if (root_) {
    DragAcceptFiles(root_, FALSE);
    UnsubclassRootForDropfiles();
  }

  for (HWND w : registered_) {
    if (w && IsWindow(w)) {
      HRESULT rv = RevokeDragDrop(w);
      logging::logf("Stop revoke w=%p ret=0x%08lX", w, (unsigned long)rv);
      UNUSED(rv);
    }
  }
  registered_.clear();

  root_ = nullptr;
  target_ = nullptr;

  pending_re_reg_ = false;
  pending_add_.clear();
  pending_del_.clear();
}

void DropRegistrar::EnsureRegisteredUnderPoint(POINTL pt_screen) {
  POINT sp{ pt_screen.x, pt_screen.y };
  HWND under = WindowFromPoint(sp);
  if (!under) return;
  if (registered_.find(under) != registered_.end()) return;
  RegisterOnWindow(under);
}

void DropRegistrar::RequestReRegisterAll() {
  if (!root_ || !IsWindow(root_)) return;
  pending_re_reg_ = true;
  PostMessage(root_, kMsgDoReRegister, 0, 0);
}

void DropRegistrar::OnDragSessionEnd() {
  FlushPendingChildOps();
  if (pending_re_reg_) {
    RequestReRegisterAll();
  }
}

bool DropRegistrar::IsDragging() const {
  return target_ ? target_->IsDragging() : false;
}

void DropRegistrar::OnWmDropFiles(HDROP hdrop) {
  if (target_) target_->HandleDropFiles(hdrop);
}

void DropRegistrar::PostUiEvent(UiEventPayload* ev) {
  if (!root_ || !IsWindow(root_)) { delete ev; return; }
  PostMessage(root_, kMsgFireUiEvent, reinterpret_cast<WPARAM>(ev), 0);
}

LRESULT CALLBACK DropRegistrar::RootSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto& self = Instance();

  // Custom async messages
  if (msg == kMsgDoReRegister) {
    if (!self.IsDragging() && self.pending_re_reg_) {
      self.pending_re_reg_ = false;
      self.ForceReRegisterAll();
    } else if (self.pending_re_reg_) {
      PostMessage(hwnd, kMsgDoReRegister, 0, 0);
    }
    return 0;
  }
  if (msg == kMsgFireUiEvent) {
    UiEventPayload* ev = reinterpret_cast<UiEventPayload*>(wp);
    if (self.target_) self.target_->DeliverUiEvent(ev);
    delete ev;
    return 0;
  }

  // WM_DROPFILES fallback
  if (msg == WM_DROPFILES) {
    self.OnWmDropFiles(reinterpret_cast<HDROP>(wp));
    DragFinish(reinterpret_cast<HDROP>(wp));
    return 0;
  }

  // Pre-register child windows on creation/destruction
  if (msg == WM_PARENTNOTIFY) {
    const WORD code = LOWORD(wp);
    if (code == WM_CREATE) {
      HWND child = reinterpret_cast<HWND>(lp);
      self.RegisterOnWindow(child);
      DragAcceptFiles(child, TRUE);
      PostMessage(hwnd, DropRegistrar::kMsgDoReRegister, 0, 0);
      return 0;
    }
    if (code == WM_DESTROY) {
      HWND child = reinterpret_cast<HWND>(lp);
      self.RevokeOnWindow(child);
      DragAcceptFiles(child, FALSE);
      return 0;
    }
  }

  return CallWindowProc(self.old_root_proc_, hwnd, msg, wp, lp);
}

void DropRegistrar::SubclassRootForDropfiles() {
  if (!root_ || !IsWindow(root_)) return;
  if (old_root_proc_) return;
  SetProp(root_, kRootPropName, reinterpret_cast<HANDLE>(this));
  old_root_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtr(root_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RootSubclassProc)));
  logging::logf("Root subclass installed");
}

void DropRegistrar::UnsubclassRootForDropfiles() {
  if (!root_ || !IsWindow(root_)) return;
  if (!old_root_proc_) return;
  SetWindowLongPtr(root_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(old_root_proc_));
  RemoveProp(root_, kRootPropName);
  old_root_proc_ = nullptr;
  logging::logf("Root subclass removed");
}

void DropRegistrar::RegisterOnWindow(HWND w) {
  if (!w) return;
  if (registered_.find(w) != registered_.end()) return;

  if (IsDragging()) { pending_add_.push_back(w); return; }

  HRESULT hr = RegisterDragDrop(w, reinterpret_cast<IDropTarget*>(target_));
  logging::logf("RegisterOnWindow w=%p ret=0x%08lX", w, (unsigned long)hr);
  if (SUCCEEDED(hr) || hr == DRAGDROP_E_ALREADYREGISTERED) {
    registered_.insert(w);
  }
}

void DropRegistrar::RevokeOnWindow(HWND w) {
  if (!w) return;
  if (registered_.find(w) == registered_.end()) return;

  if (IsDragging()) { pending_del_.push_back(w); return; }

  HRESULT rv = RevokeDragDrop(w);
  logging::logf("RevokeOnWindow w=%p ret=0x%08lX", w, (unsigned long)rv);
  registered_.erase(w);
}

void DropRegistrar::ForceReRegisterAll() {
  if (!root_ || !IsWindow(root_)) return;

  logging::logf("ForceReRegisterAll begin");

  auto snapshot = std::vector<HWND>(registered_.begin(), registered_.end());
  DoReRegister(root_);
  for (HWND w : snapshot) {
    if (w && IsWindow(w) && w != root_) {
      DoReRegister(w);
    }
  }
  EnumChildWindows(root_, EnumChildProc, reinterpret_cast<LPARAM>(this));

  DragAcceptFiles(root_, TRUE);
  unsigned long long total = static_cast<unsigned long long>(registered_.size());
  logging::logf("ForceReRegisterAll done. total=%llu", total);
}

void DropRegistrar::DoReRegister(HWND w) {
  HRESULT rv = RevokeDragDrop(w);
  logging::logf("ReReg revoke w=%p ret=0x%08lX", w, (unsigned long)rv);
  registered_.erase(w);
  HRESULT rr = RegisterDragDrop(w, reinterpret_cast<IDropTarget*>(target_));
  logging::logf("ReReg register w=%p ret=0x%08lX", w, (unsigned long)rr);
  if (SUCCEEDED(rr) || rr == DRAGDROP_E_ALREADYREGISTERED) {
    registered_.insert(w);
  }
}

BOOL CALLBACK DropRegistrar::EnumChildProc(HWND w, LPARAM lp) {
  auto self = reinterpret_cast<DropRegistrar*>(lp);
  self->RegisterOnWindow(w);
  DragAcceptFiles(w, TRUE);
  return TRUE;
}

void DropRegistrar::FlushPendingChildOps() {
  for (HWND w : pending_del_) {
    if (w && IsWindow(w)) {
      HRESULT rv = RevokeDragDrop(w);
      logging::logf("Flush revoke w=%p ret=0x%08lX", w, (unsigned long)rv);
      registered_.erase(w);
    }
  }
  pending_del_.clear();

  for (HWND w : pending_add_) {
    if (w && IsWindow(w)) {
      HRESULT rr = RegisterDragDrop(w, reinterpret_cast<IDropTarget*>(target_));
      logging::logf("Flush register w=%p ret=0x%08lX", w, (unsigned long)rr);
      if (SUCCEEDED(rr) || rr == DRAGDROP_E_ALREADYREGISTERED) {
        registered_.insert(w);
      }
    }
  }
  pending_add_.clear();
}

// -------------------- Plugin wrapper --------------------

class DesktopDropPlugin : public flutter::Plugin {
public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
    auto channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
        registrar->messenger(), "desktop_drop", &flutter::StandardMethodCodec::GetInstance());

    HWND hwnd = nullptr;
    if (registrar->GetView()) hwnd = registrar->GetView()->GetNativeWindow();
    if (!hwnd) { logging::logf("RegisterWithRegistrar: no window, no drop."); return; }

    channel->SetMethodCallHandler([](const auto& call, auto result) {
      const auto& name = call.method_name();
      if (name == "setLoggingEnabled") {
        bool on = false;
        if (const auto* arg = std::get_if<bool>(call.arguments())) on = *arg;
        logging::set_enabled(on);
        result->Success(flutter::EncodableValue(true));
        return;
      } else if (name == "getLoggingEnabled") {
        result->Success(flutter::EncodableValue(logging::enabled()));
        return;
      }
      logging::logf("MethodCall: %s", name.c_str());
      result->NotImplemented();
    });

    auto drop_target = new DesktopDropTarget(std::move(channel), hwnd);

    auto plugin = std::make_unique<DesktopDropPlugin>(drop_target);
    registrar->AddPlugin(std::move(plugin));
  }

  explicit DesktopDropPlugin(DesktopDropTarget* target) : target_(target) {
    target_->AddRef();
    logging::logf("DesktopDropPlugin() AddRef target=%p", target_);
  }
  ~DesktopDropPlugin() override {
    logging::logf("~DesktopDropPlugin() Release target=%p", target_);
    target_->Release();
  }

private:
  DesktopDropTarget* target_;
};

} // namespace

// Optional C API to toggle logging at runtime from native code
extern "C" __declspec(dllexport) void DesktopDropSetLoggingEnabled(int on) {
  logging::set_enabled(on != 0);
}

// C API
void DesktopDropPluginRegisterWithRegistrar(FlutterDesktopPluginRegistrarRef registrar) {
  DesktopDropPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
