#include "wintext.hpp"
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mutex>
#include <thread>
#include <iomanip>
#include <sstream>
#include <memory>
#include <optional>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
using edit::Range;
using edit::Text;
using run::Path;
constexpr COLORREF ink = RGB(37, 48, 53), muted = RGB(92, 108, 111), rule = RGB(219, 226, 224);
constexpr COLORREF green = RGB(20, 110, 85), paper = RGB(255, 255, 255), surface = RGB(245, 248, 247);
constexpr COLORREF red = RGB(164, 51, 46);
enum Id {
    ID_NEW = 101,
    ID_OPEN,
    ID_SAVE,
    ID_SAVE_AS,
    ID_QUIT,
    ID_UNDO,
    ID_REDO,
    ID_FIND,
    ID_REPLACE,
    ID_NEXT,
    ID_PREV,
    ID_COMMENT,
    ID_ADD,
    ID_COMPLETE,
    ID_MOVE_UP,
    ID_MOVE_DOWN,
    ID_COPY_UP,
    ID_COPY_DOWN,
    ID_RUN,
    ID_STOP,
    ID_COPY_OUTPUT,
    ID_T2,
    ID_T10,
    ID_T60,
    ID_TNONE,
    ID_CLEAR_INPUT,
    ID_RUN_UNLIMITED
};
constexpr int limits[] = {2000, 10000, 60000, 0};
static Text timeoutLabel(int milliseconds) {
    return milliseconds ? std::to_wstring(milliseconds / 1000) + L"秒" : L"制限なし";
}
constexpr int syntaxTimer = 1, runTimer = 2;
constexpr auto sample = L"def solve():\n    numbers = list(map(int, input().split()))\n    "
                        L"print(sum(numbers))\n\n\nif __name__ == \"__main__\":\n    solve()\n";
struct Session {
    std::mutex lock;
    std::string bytes;
    bool done = false;
    run::Result result{};
    std::atomic<bool> stop{false};
    std::thread thread;
    ~Session() {
        stop = true;
        if (thread.joinable())
            thread.join();
    }
};
struct App {
    HINSTANCE instance{};
    HWND win{}, code{}, input{}, output{}, runBtn{}, unlimitedBtn{}, stopBtn{}, copyBtn{}, clearBtn{},
        findWindow{};
    HFONT normal{}, bold{}, smallFont{}, mono{}, brand{};
    HBRUSH white{};
    ITextDocument *doc = nullptr;
    UINT dpi = 96, findMsg = 0;
    HMENU menu{};
    FINDREPLACEW find{};
    wchar_t needle[512]{}, replacement[512]{};
    Path path, sessionDir;
    Text baseline, summary = L"準備完了", resultLabel = L"未実行",
                   placeholder = L"ここに実行結果が表示されます";
    bool dirty = false, changing = false, crlf = false, bom = false, closing = false;
    bool composingCode = false;
    double split = .605, row = .40;
    int xSplit = 0, ySplit = 0, drag = 0, timeout = 10000;
    RECT gutter{}, statusRect{};
    COLORREF stateColor = muted;
    std::vector<Range> marks;
    std::unique_ptr<Session> job;
    std::string utfTail;
    bool pendingCR = false;
    int scale(int v) const { return MulDiv(v, int(dpi), 96); }
    void refresh() { InvalidateRect(win, nullptr, FALSE); }
    void scheduleColor() { SetTimer(win, syntaxTimer, 75, nullptr); }
    void fail(const std::exception &e) {
        MessageBoxW(win, run::wide(e.what()).c_str(), L"PyDesk", MB_OK | MB_ICONERROR);
    }
    void showTitle() {
        Text file = path.empty() ? L"main.py" : path.filename().wstring();
        SetWindowTextW(win, ((dirty ? L"●  " : L"") + file + L"  —  PyDesk").c_str());
        refresh();
    }
    void clearMarks() {
        if (!marks.empty()) {
            marks.clear();
            scheduleColor();
            refresh();
        }
    }
    void resetDoc(const Text &s, Path p = {}) {
        if (s.size() > 2 * 1024 * 1024)
            throw std::length_error(run::utf8(L"コードは 2 Mi 文字まで開けます。"));
        changing = true;
        clearMarks();
        path = std::move(p);
        ui::set(code, s);
        SendMessageW(code, EM_EMPTYUNDOBUFFER, 0, 0);
        ui::select(code, {0, 0});
        baseline = s;
        dirty = false;
        changing = false;
        scheduleColor();
        showTitle();
    }
    void loadFile(const Path &p) {
        auto s = ui::read(p);
        std::ifstream raw(p, std::ios::binary);
        char start[3]{};
        raw.read(start, 3);
        bom = raw.gcount() == 3 && std::string(start, 3) == "\xef\xbb\xbf";
        crlf = s.find(L"\r\n") != s.npos;
        resetDoc(ui::lf(std::move(s)), p);
    }
    bool saveFile(bool ask) {
        Path target = path;
        if (ask || target.empty()) {
            wchar_t buf[32768]{};
            wcsncpy_s(buf, path.empty() ? L"main.py" : path.c_str(), _TRUNCATE);
            OPENFILENAMEW d{sizeof(d)};
            d.hwndOwner = win;
            d.lpstrFile = buf;
            d.nMaxFile = _countof(buf);
            d.lpstrFilter = L"Python (*.py)\0*.py\0すべてのファイル\0*.*\0";
            d.lpstrDefExt = L"py";
            d.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
            if (!GetSaveFileNameW(&d))
                return false;
            target = buf;
        }
        auto s = ui::get(code);
        Text file = bom ? L"\ufeff" : L"";
        if (crlf) {
            for (wchar_t c : s) {
                if (c == '\n')
                    file += L'\r';
                file += c;
            }
        } else
            file += s;
        ui::save(target, file);
        path = target;
        baseline = s;
        dirty = false;
        showTitle();
        return true;
    }
    bool allowChange() {
        if (!dirty)
            return true;
        int ans = MessageBoxW(win, L"現在のコードへの変更を保存しますか？", L"PyDesk",
                              MB_YESNOCANCEL | MB_ICONQUESTION);
        return ans == IDNO || (ans == IDYES && saveFile(false));
    }
    void open() {
        wchar_t buf[32768]{};
        OPENFILENAMEW d{sizeof(d)};
        d.hwndOwner = win;
        d.lpstrFile = buf;
        d.nMaxFile = _countof(buf);
        d.lpstrFilter = L"Python (*.py)\0*.py\0すべてのファイル\0*.*\0";
        d.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetOpenFileNameW(&d) && allowChange())
            loadFile(buf);
    }
    void remember() {
        if (sessionDir.empty())
            return;
        std::filesystem::create_directories(sessionDir);
        // One atomic snapshot keeps code, stdin, and file identity in sync.
        auto name = path.wstring(), src = ui::get(code), in = ui::get(input);
        Text header = L"PYDESK1\n" + std::to_wstring(name.size()) + L" " + std::to_wstring(src.size()) +
                      L" " + std::to_wstring(timeout) + L"\n";
        ui::save(sessionDir / L"session.txt", header + name + src + in);
    }
    void restore() {
        resetDoc(sample);
        ui::set(input, L"3 1 4 1 5\n");
        if (sessionDir.empty() || !std::filesystem::exists(sessionDir / L"session.txt"))
            return;
        auto data = ui::read(sessionDir / L"session.txt", 20 * 1024 * 1024);
        auto e = data.find(L'\n', 8);
        if (data.substr(0, 8) != L"PYDESK1\n" || e == Text::npos)
            throw std::runtime_error("Invalid PyDesk session header");
        size_t pn = 0, sn = 0;
        int t = 0;
        std::wistringstream header(data.substr(8, e - 8));
        if (!(header >> pn >> sn >> t) || pn > data.size() - e - 1 || sn > data.size() - e - 1 - pn)
            throw std::runtime_error("Invalid PyDesk session length");
        Path p = data.substr(e + 1, pn);
        if (std::find(std::begin(limits), std::end(limits), t) != std::end(limits))
            timeout = t;
        if (!p.empty() && std::filesystem::is_regular_file(p))
            loadFile(p);
        else
            resetDoc(data.substr(e + 1 + pn, sn));
        ui::set(input, data.substr(e + 1 + pn + sn));
    }
    void fonts() {
        for (HFONT f : {normal, bold, smallFont, mono, brand})
            if (f)
                DeleteObject(f);
        auto make = [&](int sz, int weight, const wchar_t *face) {
            return CreateFontW(-scale(sz), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               face);
        };
        normal = make(14, FW_NORMAL, L"Yu Gothic UI");
        bold = make(14, FW_SEMIBOLD, L"Yu Gothic UI");
        smallFont = make(12, FW_NORMAL, L"Yu Gothic UI");
        mono = make(16, FW_NORMAL, L"Consolas");
        brand = make(22, FW_SEMIBOLD, L"Segoe UI");
        for (HWND w : {code, input, output})
            if (w)
                SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
    }
    HMENU menus() {
        auto root = CreateMenu(), f = CreatePopupMenu(), e = CreatePopupMenu(), r = CreatePopupMenu(),
             tm = CreatePopupMenu();
        auto item = [](HMENU m, UINT id, const wchar_t *t) { AppendMenuW(m, MF_STRING, id, t); };
        item(f, ID_NEW, L"新規作成\tCtrl+N");
        item(f, ID_OPEN, L"開く…\tCtrl+O");
        item(f, ID_SAVE, L"保存\tCtrl+S");
        item(f, ID_SAVE_AS, L"名前を付けて保存…\tCtrl+Shift+S");
        AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
        item(f, ID_QUIT, L"終了");
        item(e, ID_UNDO, L"元に戻す\tCtrl+Z");
        item(e, ID_REDO, L"やり直す\tCtrl+Shift+Z");
        AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
        item(e, ID_FIND, L"検索…\tCtrl+F");
        item(e, ID_REPLACE, L"置換…\tCtrl+H");
        item(e, ID_NEXT, L"次を検索\tF3");
        item(e, ID_PREV, L"前を検索\tShift+F3");
        AppendMenuW(e, MF_SEPARATOR, 0, nullptr);
        item(e, ID_COMMENT, L"行コメント切替\tCtrl+/");
        item(e, ID_ADD, L"次の一致を選択\tCtrl+D");
        item(e, ID_COMPLETE, L"入力候補\tCtrl+Space");
        item(e, ID_MOVE_UP, L"行を上へ移動\tAlt+↑");
        item(e, ID_MOVE_DOWN, L"行を下へ移動\tAlt+↓");
        item(e, ID_COPY_UP, L"上に行を複製\tAlt+Shift+↑");
        item(e, ID_COPY_DOWN, L"下に行を複製\tAlt+Shift+↓");
        item(r, ID_RUN, L"Python を実行\tCtrl+Enter / F5");
        item(r, ID_RUN_UNLIMITED, L"制限なしで実行");
        item(r, ID_STOP, L"停止\tShift+F5");
        item(tm, ID_T2, L"2 秒");
        item(tm, ID_T10, L"10 秒");
        item(tm, ID_T60, L"60 秒");
        item(tm, ID_TNONE, L"制限なし");
        AppendMenuW(r, MF_POPUP, reinterpret_cast<UINT_PTR>(tm), L"時間制限");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(f), L"ファイル(&F)");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(e), L"編集(&E)");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(r), L"実行(&R)");
        return root;
    }
    HWND button(int id, const wchar_t *text) {
        return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1,
                             win, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    }
    HWND textBox(bool readonly, const wchar_t *name) {
        DWORD flags = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                      ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL;
        HWND h = CreateWindowExW(0, MSFTEDIT_CLASS, L"", flags | (readonly ? ES_READONLY : 0), 0, 0, 1, 1,
                                 win, nullptr, instance, nullptr);
        if (!h)
            run::check(FALSE, "Create RichEdit");
        SetPropW(h, L"PyDeskRole", reinterpret_cast<HANDLE>(const_cast<wchar_t *>(name)));
        SendMessageW(h, EM_EXLIMITTEXT, 0, 2 * 1024 * 1024);
        SendMessageW(h, EM_SETTARGETDEVICE, 0, 1);
        SendMessageW(h, EM_SETBKGNDCOLOR, 0, paper);
        SendMessageW(h, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL);
        SendMessageW(h, EM_SETUNDOLIMIT, readonly ? 0 : 1000, 0);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(mono), FALSE);
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = ink;
        SendMessageW(h, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&cf));
        SetWindowSubclass(h, subclass, 1, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }
    void init(HWND hwnd) {
        win = hwnd;
        dpi = GetDpiForWindow(win);
        white = CreateSolidBrush(paper);
        fonts();
        findMsg = RegisterWindowMessageW(FINDMSGSTRINGW);
        menu = menus();
        SetMenu(win, menu);
        code = textBox(false, L"code");
        input = textBox(false, L"stdin");
        output = textBox(true, L"output");
        doc = ui::document(code);
        runBtn = button(ID_RUN, L"実行");
        unlimitedBtn = button(ID_RUN_UNLIMITED, L"制限なしで実行");
        stopBtn = button(ID_STOP, L"停止");
        copyBtn = button(ID_COPY_OUTPUT, L"コピー");
        clearBtn = button(ID_CLEAR_INPUT, L"クリア");
        EnableWindow(stopBtn, FALSE);
        EnableMenuItem(menu, ID_STOP, MF_BYCOMMAND | MF_GRAYED);
        try {
            restore();
        } catch (const std::exception &error) {
            fail(error);
        }
        CheckMenuRadioItem(
            menu, ID_T2, ID_TNONE,
            ID_T2 + int(std::find(std::begin(limits), std::end(limits), timeout) - std::begin(limits)),
            MF_BYCOMMAND);
        layout();
        SetFocus(code);
    }
    void rect(HWND child, int x, int y, int w, int h) {
        MoveWindow(child, x, y, std::max(1, w), std::max(1, h), TRUE);
    }
    void layout() {
        if (!code)
            return;
        RECT c{};
        GetClientRect(win, &c);
        int w = c.right, h = c.bottom;
        int top = scale(64), foot = h - scale(30), line = scale(1), head = scale(42);
        xSplit = std::clamp(int(w * split), scale(270), std::max(scale(270), w - scale(210)));
        ySplit = std::clamp(top + int((foot - top) * row), top + scale(92),
                            std::max(top + scale(92), foot - scale(94)));
        gutter = {0, top + head, scale(53), foot};
        statusRect = {0, foot, w, h};
        rect(code, gutter.right, top + head + scale(10), xSplit - gutter.right - scale(5),
             foot - top - head - scale(12));
        int rx = xSplit + scale(15), rw = w - rx - scale(8);
        rect(input, rx, top + head + scale(10), rw, ySplit - top - head - scale(15));
        rect(output, rx, ySplit + head + scale(10), rw, foot - ySplit - head - scale(15));
        rect(runBtn, w - scale(208), scale(14), scale(188), scale(36));
        rect(unlimitedBtn, w - scale(336), scale(14), scale(120), scale(36));
        rect(stopBtn, w - scale(414), scale(14), scale(70), scale(36));
        rect(clearBtn, w - scale(76), top + scale(7), scale(62), head - scale(14));
        rect(copyBtn, w - scale(76), ySplit + scale(7) + line, scale(62), head - scale(14));
        scheduleColor();
        refresh();
    }
    void drawText(HDC dc, Text s, RECT r, HFONT font, COLORREF col, UINT align = DT_LEFT) {
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, col);
        DrawTextW(dc, s.c_str(), int(s.size()), &r,
                  align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    void fill(HDC dc, RECT r, COLORREF c) {
        SetDCBrushColor(dc, c);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    void round(HDC dc, RECT r, COLORREF bg, COLORREF border, int radius) {
        auto oldB = SelectObject(dc, GetStockObject(DC_BRUSH)),
             oldP = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, bg);
        SetDCPenColor(dc, border);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, scale(radius), scale(radius));
        SelectObject(dc, oldB);
        SelectObject(dc, oldP);
    }
    void drawButton(DRAWITEMSTRUCT *d) {
        bool primary = d->CtlID == ID_RUN, stop = d->CtlID == ID_STOP,
             disabled = (d->itemState & ODS_DISABLED) != 0, pressed = (d->itemState & ODS_SELECTED) != 0;
        RECT b = d->rcItem;
        auto dc = d->hDC;
        COLORREF bg = primary ? (disabled  ? RGB(224, 231, 228)
                                 : pressed ? RGB(15, 82, 64)
                                           : green)
                              : (pressed ? RGB(228, 235, 231) : paper);
        fill(dc, b, paper);
        round(dc, b, bg, primary ? bg : rule, 9);
        COLORREF fg = disabled ? RGB(153, 164, 160) : primary ? paper : ink;
        if (primary || stop) {
            int cx = scale(primary ? 19 : 17), cy = (b.bottom + b.top) / 2;
            if (primary) {
                POINT p[] = {
                    {cx - scale(3), cy - scale(6)}, {cx - scale(3), cy + scale(6)}, {cx + scale(6), cy}};
                SelectObject(dc, GetStockObject(DC_PEN));
                SelectObject(dc, GetStockObject(DC_BRUSH));
                SetDCPenColor(dc, fg);
                SetDCBrushColor(dc, fg);
                Polygon(dc, p, 3);
            } else
                fill(dc, {cx - scale(4), cy - scale(4), cx + scale(4), cy + scale(4)}, fg);
            auto t = b;
            t.left = scale(primary ? 35 : 30);
            drawText(dc, primary ? L"実行" : L"停止", t, bold, fg);
            if (primary) {
                COLORREF detail = disabled ? fg : RGB(209, 238, 225);
                t.left = scale(72);
                drawText(dc, timeoutLabel(timeout), t, smallFont, detail);
                t.left = scale(125);
                drawText(dc, L"Ctrl+Enter", t, smallFont, detail);
            }
        } else {
            wchar_t label[32]{};
            GetWindowTextW(d->hwndItem, label, _countof(label));
            drawText(dc, label, b, d->CtlID == ID_RUN_UNLIMITED ? bold : smallFont, fg, DT_CENTER);
        }
        if (d->itemState & ODS_FOCUS) {
            InflateRect(&b, -3, -3);
            DrawFocusRect(dc, &b);
        }
    }
    void paint(HDC dc) {
        RECT c{};
        GetClientRect(win, &c);
        int w = c.right, h = c.bottom;
        int top = scale(64), head = scale(42), foot = h - scale(30), p = scale(18);
        FillRect(dc, &c, white);
        RECT logo{p, scale(16), p + scale(32), scale(48)};
        round(dc, logo, green, green, 9);
        HPEN pen = CreatePen(PS_SOLID, scale(2), paper);
        auto old = SelectObject(dc, pen);
        MoveToEx(dc, p + scale(7), scale(27), nullptr);
        LineTo(dc, p + scale(12), scale(32));
        LineTo(dc, p + scale(7), scale(37));
        MoveToEx(dc, p + scale(15), scale(37), nullptr);
        LineTo(dc, p + scale(24), scale(37));
        SelectObject(dc, old);
        DeleteObject(pen);
        drawText(dc, L"PyDesk", {p + scale(44), 0, scale(170), top}, brand, ink);
        drawText(dc, L"Python 3", {scale(144), 1, scale(236), top}, normal, muted);
        fill(dc, {0, top, w, top + head}, surface);
        fill(dc, {xSplit, ySplit, w, ySplit + head}, surface);
        fill(dc, {0, top, w, top + 1}, rule);
        fill(dc, {0, top + head - 1, w, top + head}, rule);
        fill(dc, {xSplit, ySplit, w, ySplit + 1}, rule);
        fill(dc, {xSplit, ySplit + head - 1, w, ySplit + head}, rule);
        fill(dc, {xSplit - scale(2), top, xSplit + scale(2), foot}, rule);
        drawText(dc, (path.empty() ? L"main.py" : path.filename().wstring()) + (dirty ? Text(L"  ●") : L""),
                 {p, top, xSplit - p, top + head}, bold, ink);
        drawText(dc, L"標準入力", {xSplit + p, top, w - scale(80), top + head}, bold, ink);
        drawText(dc, L"実行結果", {xSplit + p, ySplit, w - scale(80), ySplit + head}, bold, ink);
        drawText(dc, resultLabel, {xSplit + scale(91), ySplit, w - scale(83), ySplit + head}, smallFont,
                 stateColor);
        int first = int(SendMessageW(code, EM_GETFIRSTVISIBLELINE, 0, 0));
        int total = int(SendMessageW(code, EM_GETLINECOUNT, 0, 0));
        RECT editor{};
        GetWindowRect(code, &editor);
        MapWindowPoints(nullptr, win, reinterpret_cast<POINT *>(&editor), 2);
        int saved = SaveDC(dc);
        IntersectClipRect(dc, 0, editor.top, gutter.right, foot);
        for (int ln = first; ln < total; ln++) {
            LONG pos = LONG(SendMessageW(code, EM_LINEINDEX, ln, 0));
            POINTL pt{};
            SendMessageW(code, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&pt), pos);
            int y = editor.top + pt.y;
            if (y >= foot || (ln > first && y < editor.top - scale(100)))
                break;
            RECT r{0, y, gutter.right - scale(12), y + scale(22)};
            SelectObject(dc, mono);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(136, 148, 144));
            Text n = std::to_wstring(ln + 1);
            DrawTextW(dc, n.c_str(), int(n.size()), &r, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        }
        RestoreDC(dc, saved);
        fill(dc, statusRect, surface);
        fill(dc, {0, foot, w, foot + 1}, rule);
        auto badge = job ? green : stateColor;
        round(dc, {p, foot + scale(12), p + scale(7), foot + scale(19)}, badge, badge, 7);
        drawText(dc, summary, {p + scale(15), foot, w - scale(302), h}, smallFont, ink);
        auto sel = ui::selection(code);
        int line = int(SendMessageW(code, EM_EXLINEFROMCHAR, 0, sel.b)),
            base = int(SendMessageW(code, EM_LINEINDEX, line, 0));
        Text loc = marks.size() > 1
                       ? std::to_wstring(marks.size()) + L" 箇所を選択"
                       : L"行 " + std::to_wstring(line + 1) + L"　列 " + std::to_wstring(sel.b - base + 1);
        drawText(dc, loc + L"       スペース: 4       UTF-8", {w - scale(300), foot, w - p, h}, smallFont,
                 muted, DT_RIGHT);
    }
    void controls(bool active) {
        EnableWindow(runBtn, !active);
        EnableWindow(unlimitedBtn, !active);
        EnableWindow(stopBtn, active);
        EnableMenuItem(menu, ID_RUN, MF_BYCOMMAND | (active ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(menu, ID_RUN_UNLIMITED, MF_BYCOMMAND | (active ? MF_GRAYED : MF_ENABLED));
        EnableMenuItem(menu, ID_STOP, MF_BYCOMMAND | (active ? MF_ENABLED : MF_GRAYED));
        InvalidateRect(output, nullptr, TRUE);
        refresh();
    }
    void execute(int timeoutMs) {
        if (job)
            return;
        auto src = ui::get(code), in = ui::get(input);
        Path file = path.empty() ? std::filesystem::current_path() / L"main.py" : path;
        job = std::make_unique<Session>();
        SetWindowTextW(output, L"");
        utfTail.clear();
        pendingCR = false;
        placeholder.clear();
        summary = L"Python を実行中（" + timeoutLabel(timeoutMs) + L"）";
        resultLabel = L"実行中";
        stateColor = green;
        auto *session = job.get();
        try {
            session->thread =
                std::thread([session, src = std::move(src), in = std::move(in), file, timeoutMs]() {
                    auto emit = [&](const char *data, size_t n) {
                        std::lock_guard<std::mutex> guard(session->lock);
                        session->bytes.append(data, n);
                    };
                    run::Result result{};
                    try {
                        result = run::execute(src, in, file, timeoutMs, session->stop, emit);
                    } catch (const std::exception &e) {
                        std::string why = e.what();
                        emit(why.data(), why.size());
                        result = {run::State::error, 1, 0};
                    }
                    std::lock_guard<std::mutex> guard(session->lock);
                    session->result = result;
                    session->done = true;
                });
        } catch (...) {
            job.reset();
            throw;
        }
        controls(true);
        SetTimer(win, runTimer, 50, nullptr);
    }
    void appendBytes(std::string bytes, bool final) {
        utfTail += bytes;
        size_t end = utfTail.size();
        if (!final && end) {
            size_t p = end - 1;
            while (p > 0 && (static_cast<unsigned char>(utfTail[p]) & 0xc0) == 0x80)
                --p;
            unsigned char lead = static_cast<unsigned char>(utfTail[p]);
            size_t width = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
            if (end - p < width)
                end = p;
        }
        Text text = run::wide(utfTail.substr(0, end));
        utfTail.erase(0, end);
        if (pendingCR) {
            text.insert(0, 1, L'\r');
            pendingCR = false;
        }
        if (!final && !text.empty() && text.back() == L'\r') {
            pendingCR = true;
            text.pop_back();
        }
        if (text.empty())
            return;
        std::replace(text.begin(), text.end(), L'\0', L'␀');
        auto r = ui::selection(output);
        GETTEXTLENGTHEX len{GTL_NUMCHARS | GTL_PRECISE, 1200};
        int size = int(SendMessageW(output, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&len), 0));
        POINT scroll{};
        SendMessageW(output, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        ui::replace(output, {size, size}, ui::lf(std::move(text)));
        if (r.a != size || r.b != size) {
            ui::select(output, r);
            SendMessageW(output, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        } else
            SendMessageW(output, EM_SCROLLCARET, 0, 0);
    }
    void poll() {
        if (!job)
            return;
        std::string bytes;
        bool done;
        run::Result r{};
        {
            std::lock_guard<std::mutex> guard(job->lock);
            bytes.swap(job->bytes);
            done = job->done;
            r = job->result;
        }
        appendBytes(std::move(bytes), done);
        if (!done)
            return;
        KillTimer(win, runTimer);
        job.reset();
        switch (r.state) {
        case run::State::success:
            summary = L"正常終了";
            stateColor = green;
            break;
        case run::State::error:
            summary = L"実行エラー";
            stateColor = red;
            break;
        case run::State::stopped:
            summary = L"停止しました";
            stateColor = muted;
            break;
        case run::State::timeout:
            summary = L"時間制限を超えました";
            stateColor = red;
            break;
        }
        resultLabel = summary;
        summary += L"    " + std::to_wstring(r.ms) + L" ms";
        if (r.state == run::State::success || r.state == run::State::error)
            summary += L"    終了コード " + std::to_wstring(r.code);
        if (!GetWindowTextLengthW(output))
            placeholder = L"出力はありません";
        controls(false);
    }
    void clipboard(const Text &s) {
        if (s.empty())
            return;
        Text cr;
        for (auto c : s) {
            if (c == '\n')
                cr += L'\r';
            cr += c;
        }
        size_t n = (cr.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
        if (!mem)
            throw std::bad_alloc();
        void *p = GlobalLock(mem);
        if (!p) {
            GlobalFree(mem);
            run::check(FALSE, "GlobalLock");
        }
        memcpy(p, cr.c_str(), n);
        GlobalUnlock(mem);
        if (!OpenClipboard(win)) {
            GlobalFree(mem);
            run::check(FALSE, "OpenClipboard");
        }
        BOOL ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        CloseClipboard();
        if (!ok) {
            GlobalFree(mem);
            run::check(FALSE, "SetClipboardData");
        }
    }
    Text paste() {
        if (!OpenClipboard(win))
            return {};
        Text s;
        HANDLE mem = GetClipboardData(CF_UNICODETEXT);
        if (mem) {
            auto p = static_cast<const wchar_t *>(GlobalLock(mem));
            if (p) {
                s = p;
                GlobalUnlock(mem);
            }
        }
        CloseClipboard();
        return ui::lf(std::move(s));
    }
    std::optional<Range> match(const Text &s, int from, bool forward) {
        int n = edit::length(s), m = int(wcslen(needle));
        if (!m || m > n)
            return {};
        auto matches = [&](int i) {
            if (i < 0 || i > n - m)
                return false;
            if ((find.Flags & FR_WHOLEWORD) &&
                ((i > 0 && edit::word(s[i - 1])) || (i + m < n && edit::word(s[i + m]))))
                return false;
            return CompareStringOrdinal(s.data() + i, m, needle, m,
                                        (find.Flags & FR_MATCHCASE) ? FALSE : TRUE) == CSTR_EQUAL;
        };
        from = std::clamp(from, 0, n);
        if (forward) {
            for (int i = from; i <= n - m; ++i)
                if (matches(i))
                    return Range{i, i + m};
            for (int i = 0; i < from; ++i)
                if (matches(i))
                    return Range{i, i + m};
        } else {
            for (int i = from - m; i >= 0; --i)
                if (matches(i))
                    return Range{i, i + m};
            for (int i = n - m; i > from - m; --i)
                if (matches(i))
                    return Range{i, i + m};
        }
        return {};
    }
    bool findNext(bool forward) {
        if (!*needle) {
            findDialog(false);
            return false;
        }
        auto sel = ui::selection(code);
        auto hit = match(ui::get(code), forward ? sel.b : sel.a, forward);
        if (!hit) {
            summary = L"一致する文字列がありません";
            refresh();
            return false;
        }
        clearMarks();
        ui::select(code, *hit);
        SendMessageW(code, EM_SCROLLCARET, 0, 0);
        return true;
    }
    void findDialog(bool replace) {
        if (findWindow) {
            DestroyWindow(findWindow);
            findWindow = nullptr;
        }
        auto r = ui::selection(code);
        if (r.b > r.a && r.b - r.a < 512) {
            auto text = ui::get(code).substr(r.a, r.b - r.a);
            if (text.find(L'\n') == Text::npos)
                wcscpy_s(needle, text.c_str());
        }
        find = {};
        find.lStructSize = sizeof(find);
        find.hwndOwner = win;
        find.lpstrFindWhat = needle;
        find.wFindWhatLen = _countof(needle);
        find.lpstrReplaceWith = replacement;
        find.wReplaceWithLen = _countof(replacement);
        find.Flags = FR_DOWN;
        findWindow = replace ? ReplaceTextW(&find) : FindTextW(&find);
    }
    void findAction(FINDREPLACEW *f) {
        if (f->Flags & FR_DIALOGTERM) {
            findWindow = nullptr;
            return;
        }
        bool forward = (f->Flags & FR_DOWN) != 0;
        if (f->Flags & FR_FINDNEXT) {
            findNext(forward);
            return;
        }
        auto s = ui::get(code);
        int n = int(wcslen(needle));
        if (!n)
            return;
        if (f->Flags & FR_REPLACEALL) {
            Text out;
            int pos = 0, count = 0;
            while (pos <= edit::length(s) - n) {
                auto found = match(s, pos, true);
                if (!found || found->a < pos)
                    break;
                out += s.substr(pos, found->a - pos);
                out += replacement;
                pos = found->b;
                ++count;
            }
            if (count) {
                out += s.substr(pos);
                ui::replace(code, {0, edit::length(s)}, out);
                ui::select(code, {0, 0});
            }
            summary = std::to_wstring(count) + L" 件を置換しました";
            refresh();
        } else if (f->Flags & FR_REPLACE) {
            auto r = ui::selection(code);
            auto found = match(s, r.a, true);
            if (found && r.a == found->a && r.b == found->b)
                ui::replace(code, r, replacement);
            findNext(forward);
        }
    }
    void addMatch() {
        Text s = ui::get(code);
        Range r = ui::selection(code);
        if (marks.empty() && r.a == r.b) {
            r = edit::wordAt(s, r.a);
            if (r.a == r.b)
                return;
            ui::select(code, r);
            return;
        }
        if (r.a == r.b)
            return;
        Text term = s.substr(r.a, r.b - r.a);
        if (marks.empty())
            marks.push_back(r);
        size_t pos = r.b;
        for (int pass = 0; pass < 2; pass++) {
            while ((pos = s.find(term, pos)) != Text::npos) {
                Range next{int(pos), int(pos + term.size())};
                if (std::none_of(marks.begin(), marks.end(),
                                 [&](Range m) { return m.a < next.b && next.a < m.b; })) {
                    marks.push_back(next);
                    ui::select(code, next);
                    SendMessageW(code, EM_SCROLLCARET, 0, 0);
                    scheduleColor();
                    refresh();
                    return;
                }
                pos += term.size();
            }
            pos = 0;
        }
    }
    void multi(const Text &value, int erase = 0, bool indentOnEnter = false) {
        auto ranges = marks;
        std::sort(ranges.begin(), ranges.end(), [](Range a, Range b) { return a.a < b.a; });
        const auto s = ui::get(code);
        int size = edit::length(s);
        for (auto &r : ranges)
            if (r.a == r.b) {
                if (erase < 0 && r.a > 0) {
                    --r.a;
                    if (r.a > 0 && IS_LOW_SURROGATE(s[r.a]) && IS_HIGH_SURROGATE(s[r.a - 1]))
                        --r.a;
                }
                if (erase > 0 && r.b < size) {
                    ++r.b;
                    if (r.b < size && IS_HIGH_SURROGATE(s[r.b - 1]) && IS_LOW_SURROGATE(s[r.b]))
                        ++r.b;
                }
            }
        std::vector<Range> merged;
        for (auto r : ranges) {
            if (!merged.empty() &&
                (r.a < merged.back().b || (r.a == merged.back().a && r.b == merged.back().b)))
                merged.back().b = std::max(merged.back().b, r.b);
            else
                merged.push_back(r);
        }
        ranges.swap(merged);
        const auto breaks = indentOnEnter ? edit::lineBreaks(s, ranges) : std::vector<Text>{};
        auto textAtCursor = [&](size_t i) -> const Text & { return indentOnEnter ? breaks[i] : value; };
        if (doc)
            doc->BeginEditCollection();
        for (size_t i = ranges.size(); i > 0; --i)
            ui::replace(code, ranges[i - 1], textAtCursor(i - 1));
        if (doc)
            doc->EndEditCollection();
        int delta = 0;
        for (size_t i = 0; i < ranges.size(); ++i) {
            auto &r = ranges[i];
            int inserted = edit::length(textAtCursor(i));
            int p = r.a + delta + inserted;
            delta += inserted - (r.b - r.a);
            r = {p, p};
        }
        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](Range a, Range b) { return a.a == b.a; }),
                     ranges.end());
        marks = ranges;
        ui::select(code, marks.back());
        if (marks.size() == 1)
            marks.clear();
        scheduleColor();
        refresh();
    }
    void complete() {
        auto s = ui::get(code);
        auto r = ui::selection(code);
        auto span = edit::wordAt(s, r.a);
        Text prefix = s.substr(span.a, r.a - span.a);
        std::set<Text> names = edit::keywords;
        names.insert(edit::builtins.begin(), edit::builtins.end());
        for (int i = 0, n = edit::length(s); i < n;) {
            if (edit::word(s[i]) && !iswdigit(s[i])) {
                int a = i++;
                while (i < n && edit::word(s[i]))
                    ++i;
                names.insert(s.substr(a, i - a));
            } else
                ++i;
        }
        std::vector<Text> items;
        HMENU pop = CreatePopupMenu();
        for (const auto &name : names)
            if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                items.push_back(name);
                AppendMenuW(pop, MF_STRING, items.size(), name.c_str());
                if (items.size() >= 24)
                    break;
            }
        if (items.empty()) {
            DestroyMenu(pop);
            return;
        }
        POINTL cp{};
        SendMessageW(code, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&cp), r.a);
        POINT p{cp.x, cp.y + scale(21)};
        ClientToScreen(code, &p);
        int id = TrackPopupMenu(pop, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, p.x, p.y, 0, win, nullptr);
        DestroyMenu(pop);
        if (id > 0)
            ui::replace(code, span, items[id - 1]);
    }
    void cmd(int id) {
        if (id >= ID_T2 && id <= ID_TNONE) {
            timeout = limits[id - ID_T2];
            CheckMenuRadioItem(menu, ID_T2, ID_TNONE, id, MF_BYCOMMAND);
            InvalidateRect(runBtn, nullptr, TRUE);
            return;
        }
        switch (id) {
        case ID_NEW:
            if (allowChange()) {
                crlf = bom = false;
                resetDoc(L"");
                SetFocus(code);
            }
            break;
        case ID_OPEN:
            open();
            break;
        case ID_SAVE:
            saveFile(false);
            break;
        case ID_SAVE_AS:
            saveFile(true);
            break;
        case ID_QUIT:
            PostMessageW(win, WM_CLOSE, 0, 0);
            break;
        case ID_RUN:
            execute(timeout);
            break;
        case ID_RUN_UNLIMITED:
            execute(0);
            break;
        case ID_STOP:
            if (job)
                job->stop = true;
            break;
        case ID_COPY_OUTPUT:
            clipboard(ui::get(output));
            break;
        case ID_CLEAR_INPUT:
            ui::replace(input, {0, edit::length(ui::get(input))}, L"");
            SetFocus(input);
            break;
        case ID_FIND:
            findDialog(false);
            break;
        case ID_REPLACE:
            findDialog(true);
            break;
        case ID_NEXT:
            findNext(true);
            break;
        case ID_PREV:
            findNext(false);
            break;
        case ID_UNDO:
        case ID_REDO: {
            clearMarks();
            HWND target = GetFocus() == input ? input : code;
            SendMessageW(target, id == ID_UNDO ? EM_UNDO : EM_REDO, 0, 0);
            break;
        }
        case ID_COMMENT:
            clearMarks();
            ui::apply(code, edit::block(ui::get(code), ui::selection(code), edit::Block::comment));
            break;
        case ID_ADD:
            addMatch();
            break;
        case ID_COMPLETE:
            complete();
            break;
        case ID_MOVE_UP:
        case ID_MOVE_DOWN:
        case ID_COPY_UP:
        case ID_COPY_DOWN:
            clearMarks();
            ui::apply(code, edit::moveLines(ui::get(code), ui::selection(code),
                                            id == ID_MOVE_DOWN || id == ID_COPY_DOWN,
                                            id == ID_COPY_DOWN || id == ID_COPY_UP));
            break;
        }
    }
    bool character(wchar_t c) {
        if (marks.size() > 1 && (c >= 32 || c == 8 || c == 13 || c == 9)) {
            if (c == 8)
                multi(L"", -1);
            else if (c == 13)
                multi(L"", 0, true);
            else
                multi(c == 9 ? L"    " : Text(1, c));
            return true;
        }
        if (c != 13 && c != 9 && !wcschr(L"([{'\"}])", c))
            return false;
        auto s = ui::get(code);
        auto r = ui::selection(code);
        if (c == 13) {
            ui::apply(code, edit::newline(s, r));
            return true;
        }
        if (c == 9) {
            bool back = GetKeyState(VK_SHIFT) < 0;
            if (back || r.a != r.b)
                ui::apply(code, edit::block(s, r, back ? edit::Block::outdent : edit::Block::indent));
            else
                ui::apply(code, edit::insert(r, Text(4 - (r.a - edit::start(s, r.a)) % 4, L' ')));
            return true;
        }
        if (r.a == r.b && r.a < edit::length(s) && s[r.a] == c && wcschr(L")]}'\"", c)) {
            ui::select(code, {r.a + 1, r.a + 1});
            return true;
        }
        Text open = L"([{'\"", close = L")]}'\"";
        auto index = open.find(c);
        if (index == Text::npos)
            return false;
        if (r.a == r.b && r.b < edit::length(s) && edit::word(s[r.b]))
            return false;
        Text body = s.substr(r.a, r.b - r.a), pair = Text(1, c) + body + close[index];
        ui::apply(code, {r, pair, {r.a + 1, r.b + 1}});
        return true;
    }
    bool key(MSG &m) {
        if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)
            return false;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0,
             alt = GetKeyState(VK_MENU) < 0;
        if (ctrl && alt)
            return false;
        int k = int(m.wParam), id = 0;
        bool editor = m.hwnd == code;
        if ((ctrl && k == VK_RETURN) || k == VK_F5)
            id = shift && k == VK_F5 ? ID_STOP : ID_RUN;
        else if (ctrl && k == 'S')
            id = shift ? ID_SAVE_AS : ID_SAVE;
        else if (ctrl && k == 'O')
            id = ID_OPEN;
        else if (ctrl && k == 'N')
            id = ID_NEW;
        else if (ctrl && k == 'F')
            id = ID_FIND;
        else if (ctrl && k == 'H')
            id = ID_REPLACE;
        else if (k == VK_F3)
            id = shift ? ID_PREV : ID_NEXT;
        else if (ctrl && k == 'Z' && (editor || m.hwnd == input))
            id = shift ? ID_REDO : ID_UNDO;
        else if (ctrl && k == 'Y' && (editor || m.hwnd == input))
            id = ID_REDO;
        else if (editor && ctrl && k == VK_OEM_2)
            id = ID_COMMENT;
        else if (editor && ctrl && k == 'D')
            id = ID_ADD;
        else if (editor && ctrl && k == VK_SPACE)
            id = ID_COMPLETE;
        else if (editor && alt && (k == VK_UP || k == VK_DOWN))
            id = k == VK_UP ? (shift ? ID_COPY_UP : ID_MOVE_UP) : (shift ? ID_COPY_DOWN : ID_MOVE_DOWN);
        if (id) {
            cmd(id);
            return true;
        }
        if (k == VK_F6) {
            HWND fields[] = {code, input, output};
            auto at = std::find(std::begin(fields), std::end(fields), m.hwnd);
            SetFocus(fields[(int(at - std::begin(fields)) + (shift ? 2 : 1)) % 3]);
            return true;
        }
        if (ctrl && k == 'A' && (editor || m.hwnd == input || m.hwnd == output)) {
            clearMarks();
            ui::select(m.hwnd, {0, -1});
            return true;
        }
        if (ctrl && k == 'V' && (editor || m.hwnd == input)) {
            SendMessageW(m.hwnd, WM_PASTE, 0, 0);
            return true;
        }
        if (!editor)
            return false;
        if (ctrl && k == 'X' && marks.size() > 1) {
            auto r = ui::selection(code);
            clipboard(ui::get(code).substr(r.a, r.b - r.a));
            multi(L"");
            return true;
        }
        if (!marks.empty() &&
            (k == VK_ESCAPE || k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN || k == VK_HOME ||
             k == VK_END || k == VK_PRIOR || k == VK_NEXT)) {
            clearMarks();
            return k == VK_ESCAPE;
        }
        if (k == VK_DELETE && marks.size() > 1) {
            multi(L"", 1);
            return true;
        }
        if (k == VK_BACK && marks.empty()) {
            auto r = ui::selection(code);
            if (r.a == r.b && r.a > 0) {
                auto s = ui::get(code);
                int head = edit::start(s, r.a);
                if (r.a > head && s.find_first_not_of(L' ', head) >= static_cast<size_t>(r.a)) {
                    int n = (r.a - head - 1) % 4 + 1;
                    ui::replace(code, {r.a - n, r.a}, L"");
                    return true;
                }
                Text o = L"([{'\"", c = L")]}'\"";
                auto i = o.find(s[r.a - 1]);
                if (i != Text::npos && r.a < edit::length(s) && s[r.a] == c[i]) {
                    ui::replace(code, {r.a - 1, r.a + 1}, L"");
                    return true;
                }
            }
        }
        return false;
    }
    static LRESULT CALLBACK subclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        App &a = *reinterpret_cast<App *>(data);
        try {
            if (hwnd == a.code) {
                if (msg == WM_IME_STARTCOMPOSITION)
                    a.composingCode = true;
                if (msg == WM_IME_ENDCOMPOSITION || msg == WM_KILLFOCUS)
                    a.composingCode = false;
                // RichEdit inserts a paragraph on keydown. TranslateMessage also sends WM_CHAR,
                // which already inserts the newline and its indentation as a single edit.
                if (msg == WM_KEYDOWN && w == VK_RETURN && !a.composingCode)
                    return 0;
                if (msg == WM_CHAR && !a.composingCode && a.character(static_cast<wchar_t>(w)))
                    return 0;
                if (msg == WM_LBUTTONDOWN || msg == WM_IME_STARTCOMPOSITION)
                    a.clearMarks();
                if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_HSCROLL) {
                    auto result = DefSubclassProc(hwnd, msg, w, l);
                    InvalidateRect(a.win, &a.gutter, FALSE);
                    a.scheduleColor();
                    return result;
                }
            }
            if ((hwnd == a.code || hwnd == a.input) && msg == WM_PASTE) {
                if (hwnd == a.code && a.marks.size() > 1) {
                    auto s = a.paste();
                    if (!s.empty())
                        a.multi(s);
                } else
                    SendMessageW(hwnd, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
                return 0;
            }
            if (hwnd == a.output && msg == WM_PAINT && !a.placeholder.empty()) {
                auto res = DefSubclassProc(hwnd, msg, w, l);
                HDC dc = GetDC(hwnd);
                RECT c{};
                GetClientRect(hwnd, &c);
                c.left += a.scale(1);
                c.bottom = c.top + a.scale(23);
                a.drawText(dc, a.placeholder, c, a.normal, muted);
                ReleaseDC(hwnd, dc);
                return res;
            }
        } catch (const std::exception &e) {
            a.fail(e);
            return 0;
        }
        return DefSubclassProc(hwnd, msg, w, l);
    }
    int hit(int x, int y) const {
        if (y < scale(64) || y >= statusRect.top)
            return 0;
        if (abs(x - xSplit) <= scale(5))
            return 1;
        if (x > xSplit && abs(y - ySplit) <= scale(5))
            return 2;
        return 0;
    }
    LRESULT message(UINT msg, WPARAM w, LPARAM l) {
        if (findMsg && msg == findMsg) {
            findAction(reinterpret_cast<FINDREPLACEW *>(l));
            return 0;
        }
        switch (msg) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED)
                layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto mm = reinterpret_cast<MINMAXINFO *>(l);
            mm->ptMinTrackSize = {scale(650), scale(400)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi = HIWORD(w);
            fonts();
            auto r = reinterpret_cast<RECT *>(l);
            SetWindowPos(win, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_DRAWITEM:
            drawButton(reinterpret_cast<DRAWITEMSTRUCT *>(l));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(win, &ps);
            if (code)
                paint(dc);
            EndPaint(win, &ps);
            return 0;
        }
        case WM_NOTIFY: {
            auto h = reinterpret_cast<NMHDR *>(l);
            if (!changing && h->hwndFrom == code && h->code == EN_SELCHANGE)
                InvalidateRect(win, &statusRect, FALSE);
            return 0;
        }
        case WM_COMMAND: {
            if (l && reinterpret_cast<HWND>(l) == code && !changing) {
                if (HIWORD(w) == EN_CHANGE) {
                    dirty = (ui::get(code) != baseline);
                    showTitle();
                    scheduleColor();
                }
                if (HIWORD(w) == EN_VSCROLL) {
                    InvalidateRect(win, &gutter, FALSE);
                    scheduleColor();
                }
            } else if (!l || HIWORD(w) == BN_CLICKED)
                cmd(LOWORD(w));
            return 0;
        }
        case WM_TIMER:
            if (w == syntaxTimer) {
                KillTimer(win, syntaxTimer);
                changing = true;
                ui::highlight(code, doc, marks);
                changing = false;
                InvalidateRect(win, &gutter, FALSE);
            }
            if (w == runTimer)
                poll();
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(l) == HTCLIENT) {
                POINT p{};
                GetCursorPos(&p);
                ScreenToClient(win, &p);
                int mode = drag ? drag : hit(p.x, p.y);
                if (mode) {
                    SetCursor(LoadCursorW(nullptr, mode == 1 ? IDC_SIZEWE : IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
            drag = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (drag)
                SetCapture(win);
            return 0;
        case WM_MOUSEMOVE:
            if (drag) {
                RECT r{};
                GetClientRect(win, &r);
                if (drag == 1)
                    split = std::clamp(double(GET_X_LPARAM(l)) / std::max(1L, r.right), .25, .78);
                else
                    row = std::clamp(double(GET_Y_LPARAM(l) - scale(64)) / std::max(1L, r.bottom - scale(94)),
                                     .17, .78);
                layout();
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag) {
                drag = 0;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            drag = 0;
            return 0;
        case WM_SETFOCUS:
            if (code)
                SetFocus(code);
            return 0;
        case WM_CLOSE:
            if (!path.empty() && !allowChange())
                return 0;
            // Named documents are reopened from disk. Untitled buffers are restored verbatim.
            if (!closing) {
                remember();
                closing = true;
            }
            if (job) {
                job->stop = true;
                job.reset();
            }
            DestroyWindow(win);
            return 0;
        case WM_DESTROY:
            if (doc) {
                doc->Release();
                doc = nullptr;
            }
            for (auto f : {normal, bold, smallFont, mono, brand})
                if (f)
                    DeleteObject(f);
            if (white)
                DeleteObject(white);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(win, msg, w, l);
    }
    static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
        App *a = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            a = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            a->win = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
        }
        if (!a)
            return DefWindowProcW(window, msg, w, l);
        try {
            if (msg == WM_CREATE) {
                a->init(window);
                return 0;
            }
            return a->message(msg, w, l);
        } catch (const std::exception &e) {
            a->changing = false;
            a->fail(e);
            return msg == WM_CREATE ? -1 : 0;
        }
    }
    void create() {
        instance = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpszClassName = L"PyDeskWindow";
        wc.lpfnWndProc = proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        run::check(RegisterClassW(&wc) != 0, "RegisterClass");
        dpi = GetDpiForSystem();
        RECT area{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        int width = std::min<int>(scale(1100), area.right - area.left),
            height = std::min<int>(scale(730), area.bottom - area.top);
        HWND created = CreateWindowExW(0, wc.lpszClassName, L"PyDesk", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                       area.left + (area.right - area.left - width) / 2,
                                       area.top + (area.bottom - area.top - height) / 2, width, height,
                                       nullptr, nullptr, instance, this);
        run::check(created != nullptr, "CreateWindow");
    }
    int loop() {
        MSG msg{};
        BOOL rc;
        while ((rc = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (findWindow && IsDialogMessageW(findWindow, &msg))
                continue;
            try {
                if (key(msg))
                    continue;
            } catch (const std::exception &e) {
                fail(e);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return rc < 0 ? 1 : int(msg.wParam);
    }
};
static void platform() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr)))
        throw std::runtime_error("OLE initialization failed");
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    run::check(LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr,
               "Load RichEdit");
}
#ifndef PYDESK_UI_TEST
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
    try {
        platform();
        App a;
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            throw std::runtime_error("Cannot locate LocalAppData");
        a.sessionDir = Path(local) / L"PyDesk";
        CoTaskMemFree(local);
        a.create();
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            if (argc > 1) {
                try {
                    a.loadFile(std::filesystem::absolute(argv[1]));
                } catch (const std::exception &e) {
                    a.fail(e);
                }
            }
            LocalFree(argv);
        }
        ShowWindow(a.win, show);
        UpdateWindow(a.win);
        int status = a.loop();
        OleUninitialize();
        return status;
    } catch (const std::exception &e) {
        MessageBoxW(nullptr, run::wide(e.what()).c_str(), L"PyDesk", MB_ICONERROR);
        return 1;
    }
}
#else
#include "ui_tests.hpp"
#endif
