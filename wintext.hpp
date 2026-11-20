#pragma once
#include "editor.hpp"
#include "runner.hpp"
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <commctrl.h>
#include <fstream>

namespace ui {
using edit::Range;
using edit::Text;
inline Text lf(Text s) {
    Text out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n')
                ++i;
            out += L'\n';
        } else
            out += s[i];
    }
    return out;
}
inline Text native(Text s) {
    std::replace(s.begin(), s.end(), L'\n', L'\r');
    return s;
}
inline Text get(HWND w) {
    GETTEXTLENGTHEX len{GTL_PRECISE | GTL_NUMCHARS, 1200};
    size_t n = SendMessageW(w, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&len), 0);
    Text s(n + 1, 0);
    GETTEXTEX req{static_cast<DWORD>(s.size() * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr};
    s.resize(
        SendMessageW(w, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&req), reinterpret_cast<LPARAM>(s.data())));
    return lf(std::move(s));
}
inline void set(HWND w, const Text &text) { SetWindowTextW(w, native(text).c_str()); }
inline Range selection(HWND w) {
    CHARRANGE r{};
    SendMessageW(w, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&r));
    return {r.cpMin, r.cpMax};
}
inline void select(HWND w, Range r) {
    CHARRANGE c{r.a, r.b};
    SendMessageW(w, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&c));
}
inline void replace(HWND w, Range r, const Text &s) {
    select(w, r);
    Text val = native(s);
    SendMessageW(w, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(val.c_str()));
}
inline void apply(HWND w, const edit::Change &c) {
    replace(w, c.old, c.text);
    select(w, c.selection);
    SendMessageW(w, EM_SCROLLCARET, 0, 0);
}
inline ITextDocument *document(HWND w) {
    IRichEditOle *ole = nullptr;
    ITextDocument *doc = nullptr;
    SendMessageW(w, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&ole));
    if (ole) {
        ole->QueryInterface(__uuidof(ITextDocument), reinterpret_cast<void **>(&doc));
        ole->Release();
    }
    return doc;
}
inline COLORREF color(edit::Ink ink) {
    switch (ink) {
    case edit::Ink::keyword:
        return RGB(137, 60, 150);
    case edit::Ink::builtin:
        return RGB(25, 87, 159);
    case edit::Ink::number:
        return RGB(164, 77, 21);
    case edit::Ink::string:
        return RGB(20, 115, 79);
    case edit::Ink::comment:
        return RGB(102, 124, 117);
    case edit::Ink::definition:
        return RGB(117, 77, 15);
    default:
        return RGB(35, 44, 50);
    }
}
inline void paintFont(ITextDocument *doc, int a, int b, COLORREF ink, bool background = false) {
    if (a >= b)
        return;
    ITextRange *range = nullptr;
    ITextFont *font = nullptr;
    if (SUCCEEDED(doc->Range(a, b, &range))) {
        if (SUCCEEDED(range->GetFont(&font))) {
            if (background)
                font->SetBackColor(ink);
            else
                font->SetForeColor(ink);
            font->Release();
        }
        range->Release();
    }
}
inline void highlight(HWND w, ITextDocument *doc, const std::vector<Range> &marks) {
    if (!doc)
        return;
    auto s = get(w);
    if (s.empty())
        return;
    RECT client{};
    GetClientRect(w, &client);
    POINTL top{0, 0}, bottom{client.right, client.bottom};
    int lo = edit::start(
        s, std::clamp(static_cast<int>(SendMessageW(w, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&top))), 0,
                      edit::length(s)));
    int hi = edit::end(
        s, std::clamp(static_cast<int>(SendMessageW(w, EM_CHARFROMPOS, 0, reinterpret_cast<LPARAM>(&bottom))),
                      lo, edit::length(s)));
    // TOM ranges preserve the caret, scroll position and native undo stack.
    long frozen = 0;
    doc->Freeze(&frozen);
    doc->Undo(tomSuspend, nullptr);
    paintFont(doc, lo, hi, color(edit::Ink::normal));
    paintFont(doc, lo, hi, RGB(255, 255, 255), true);
    for (auto t : edit::scan(s)) {
        if (t.a >= hi)
            break;
        if (t.b > lo)
            paintFont(doc, std::max(lo, t.a), std::min(hi, t.b), color(t.ink));
    }
    for (auto r : marks)
        paintFont(doc, r.a, std::min(edit::length(s), std::max(r.a + 1, r.b)), RGB(182, 231, 211), true);
    doc->Undo(tomResume, nullptr);
    doc->Unfreeze(&frozen);
}
inline Text read(const run::Path &path, std::streamoff limit = 8 * 1024 * 1024) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open file: " + run::utf8(path.wstring()));
    auto n = file.tellg();
    if (n < 0 || n > limit)
        throw std::runtime_error(run::utf8(L"ファイルが読み込みサイズの上限を超えています。"));
    std::string bytes(static_cast<size_t>(n), 0);
    file.seekg(0);
    if (!file.read(bytes.data(), n))
        throw std::runtime_error("Cannot read file");
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0)
        bytes.erase(0, 3);
    if (bytes.find('\0') != bytes.npos)
        throw std::runtime_error(run::utf8(L"UTF-8 のテキストファイルを指定してください。"));
    return run::wide(bytes, true);
}
inline void save(const run::Path &path, const Text &s) {
    wchar_t tmp[MAX_PATH]{};
    run::check(GetTempFileNameW(path.parent_path().c_str(), L"pys", 0, tmp) != 0, "Create temporary file");
    struct Cleanup {
        wchar_t *p;
        ~Cleanup() { DeleteFileW(p); }
    } cleanup{tmp};
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    auto data = run::utf8(s);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    if (!file)
        throw std::runtime_error("Cannot save file");
    run::check(MoveFileExW(tmp, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
               "Save file");
}
} // namespace ui
