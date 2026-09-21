// One JSON line per poll: what a person sitting beside the user would see and hear.
//   {"t": s since start, "idle_s": s since the last key or mouse input, "focus": {"app", "title", "text"},
//    "media": [{"app", "title", "artist"}], "mic": [app], "headphones": device name}
// Past `idle_s`, a field is there only when it has something to say: nothing playing, no `media`; sound on speakers, no `headphones`.
// usage: kernel [poll_seconds=10]
#include <windows.h>
#include <initguid.h> // these three in this order: initguid has the next two define their property keys here, not just declare them,
#include <mmdeviceapi.h> // and this one brings the macro
#include <functiondiscoverykeys_devpkey.h> // that this one is written in
#include <audiopolicy.h>
#include <uiautomation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <set>
#include <string>

const size_t TEXT_CAP = 2000; // UTF-16 units of window text per line. ponytail: spent in document order; start from the focused element if what is on screen still overflows it

winrt::com_ptr<IUIAutomation> uia;
winrt::com_ptr<IUIAutomationCacheRequest> cache; // brings the window's whole subtree, with the properties we read, back in one cross-process call
winrt::com_ptr<IMMDeviceEnumerator> audio;

std::string quote(std::wstring_view s) { // a JSON string
    std::string o = "\"";
    for (unsigned char c : winrt::to_string(s)) {
        if (c == '"' || c == '\\') (o += '\\') += c;
        else if (c == '\n') o += "\\n"; // text is mostly newlines; the six-character unicode escape costs a model several tokens each
        else if (c < 0x20) o += std::format("\\u{:04x}", c);
        else o += c;
    }
    return o + '"';
}

// What carries content; the rest of a UIA tree is buttons, panes and tabs (the list Aisling settled on).
bool content(CONTROLTYPEID type) {
    switch (type) {
    case UIA_TextControlTypeId: case UIA_DocumentControlTypeId: case UIA_HyperlinkControlTypeId: case UIA_EditControlTypeId:
    case UIA_ListItemControlTypeId: case UIA_DataItemControlTypeId: case UIA_TreeItemControlTypeId: return true;
    default: return false;
    }
}

// Appends the names of the content elements that are on screen, one per line, in document order.
// `said` is what the nearest ancestor already put down: a list item's name repeats its paragraphs, a link's its text.
void read(IUIAutomationElement* e, const std::wstring& said, std::wstring& o) {
    if (o.size() >= TEXT_CAP) return;
    CONTROLTYPEID type = 0;
    BOOL offscreen = FALSE;
    BSTR name = nullptr; // a COM string: the callee allocates, we free
    e->get_CachedControlType(&type);
    e->get_CachedIsOffscreen(&offscreen);
    e->get_CachedName(&name);
    std::wstring line = name ? name : L"";
    SysFreeString(name);
    bool say = content(type) && !offscreen && !line.empty() && said.find(line) == std::wstring::npos && !o.ends_with(line + L'\n'); // nor twice in a row
    if (say) (o += line.substr(0, TEXT_CAP - o.size())) += L'\n';

    winrt::com_ptr<IUIAutomationElementArray> children;
    int n = 0;
    if (FAILED(e->GetCachedChildren(children.put())) || !children) return;
    children->get_Length(&n);
    for (int i = 0; i < n; i++) {
        winrt::com_ptr<IUIAutomationElement> child;
        children->GetElement(i, child.put());
        read(child.get(), say ? line : said, o);
    }
}

// The window's text as a person glancing at it would read it. Chromium builds its tree only once someone has asked,
// so the first poll of such an app comes back nearly empty and the next one has the page.
// ponytail: blocks for as long as the app takes to answer (the desktop: 4 s); walk on a second thread if a poll must not stall
std::wstring text(HWND w) {
    winrt::com_ptr<IUIAutomationElement> root;
    std::wstring o;
    if (SUCCEEDED(uia->ElementFromHandleBuildCache(w, cache.get(), root.put())) && root) read(root.get(), L"", o);
    return o;
}

void field(std::string& o, const char* key, std::wstring_view value) { // appends "key":"value" unless there is no value
    if (!value.empty()) o += std::format("{}\"{}\":{}", o.empty() ? "" : ",", key, quote(value));
}

std::wstring app_name(DWORD pid) { // C:\...\chrome.exe -> chrome
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        QueryFullProcessImageNameW(p, 0, path, &n);
        CloseHandle(p);
    }
    return std::filesystem::path(path).stem().wstring();
}

// ponytail: GetForegroundWindow says ApplicationFrameHost for UWP apps; take the pid from the UIA focused element instead
std::string focus() {
    HWND w = GetForegroundWindow();
    wchar_t title[512]{};
    GetWindowTextW(w, title, 512);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::string o;
    field(o, "app", app_name(pid));
    field(o, "title", title);
    field(o, "text", text(w));
    return o.empty() ? "" : ",\"focus\":{" + o + "}";
}

// What is playing, from the system media sessions (the ones the volume flyout shows). Paused is the same as not there.
std::string media() {
    using namespace winrt::Windows::Media::Control;
    static auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    std::string all;
    for (auto session : manager.GetSessions()) {
        try { // a session can close between being listed and being asked: leave it out of this poll
            if (session.GetPlaybackInfo().PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) continue;
            auto properties = session.TryGetMediaPropertiesAsync().get();
            std::wstring app{session.SourceAppUserModelId()}; // ponytail: a packaged app's id is a long package string, not a name
            if (app.ends_with(L".exe")) app.resize(app.size() - 4); // an unpackaged app's is its file name: make it read like focus.app
            std::string o;
            field(o, "app", app);
            field(o, "title", properties.Title());
            field(o, "artist", properties.Artist());
            all += (all.empty() ? "" : ",") + ("{" + o + "}");
        } catch (winrt::hresult_error const&) {}
    }
    return all.empty() ? "" : ",\"media\":[" + all + "]";
}

// The apps with a live capture stream: someone is in a call, or recording. Asked of Core Audio because the registry's
// ConsentStore, where the tray icon's list lives, keeps "in use" entries of apps that died mid-call (seen: a Discord long gone).
std::string mic() {
    winrt::com_ptr<IMMDeviceCollection> mics;
    std::set<std::wstring> apps; // one app may hold several devices
    UINT n = 0;
    audio->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, mics.put());
    mics->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        winrt::com_ptr<IMMDevice> device;
        winrt::com_ptr<IAudioSessionManager2> manager;
        winrt::com_ptr<IAudioSessionEnumerator> sessions;
        int count = 0;
        mics->Item(i, device.put());
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void()))) continue; // unplugged just now
        manager->GetSessionEnumerator(sessions.put());
        sessions->GetCount(&count);
        for (int j = 0; j < count; j++) {
            winrt::com_ptr<IAudioSessionControl> session;
            AudioSessionState state;
            DWORD pid = 0;
            sessions->GetSession(j, session.put());
            session->GetState(&state);
            session.as<IAudioSessionControl2>()->GetProcessId(&pid);
            if (state == AudioSessionStateActive && !app_name(pid).empty()) apps.insert(app_name(pid));
        }
    }
    std::string o;
    for (auto& app : apps) o += (o.empty() ? "" : ",") + quote(app);
    return o.empty() ? "" : ",\"mic\":[" + o + "]";
}

// The default output device's name, when it sits on the user's head: nobody beside them hears what is playing.
// ponytail: the driver decides what it calls itself; an analog jack that shares the speakers' endpoint never shows up here
std::string headphones() {
    winrt::com_ptr<IMMDevice> out;
    winrt::com_ptr<IPropertyStore> properties;
    PROPVARIANT form, name;
    if (FAILED(audio->GetDefaultAudioEndpoint(eRender, eMultimedia, out.put()))) return ""; // no output device at all
    out->OpenPropertyStore(STGM_READ, properties.put());
    properties->GetValue(PKEY_AudioEndpoint_FormFactor, &form);
    if (form.uintVal != Headphones && form.uintVal != Headset) return "";
    properties->GetValue(PKEY_Device_FriendlyName, &name);
    std::string o = ",\"headphones\":" + quote(name.pwszVal);
    PropVariantClear(&name);
    return o;
}

int idle_s() {
    LASTINPUTINFO last{sizeof last};
    GetLastInputInfo(&last);
    return (GetTickCount() - last.dwTime) / 1000;
}

int main(int argc, char** argv) {
    int poll = argc > 1 ? atoi(argv[1]) : 10;
    winrt::init_apartment();
    uia = winrt::create_instance<IUIAutomation>(CLSID_CUIAutomation);
    uia->CreateCacheRequest(cache.put());
    cache->AddProperty(UIA_NamePropertyId);
    cache->AddProperty(UIA_ControlTypePropertyId);
    cache->AddProperty(UIA_IsOffscreenPropertyId);
    cache->put_TreeScope(TreeScope_Subtree);
    audio = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::println(R"({{"t":{:.1f},"idle_s":{}{}{}{}{}}})", t, idle_s(), focus(), media(), mic(), headphones());
        fflush(stdout); // println throws once the reader is gone, which ends us
        Sleep(poll * 1000);
    }
}
