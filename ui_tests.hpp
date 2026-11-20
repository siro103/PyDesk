#include <iostream>
static int assertions = 0;
static void ensure(bool ok, const char *name) {
    if (!ok)
        throw std::runtime_error(name);
    std::cout << "ok  " << name << std::endl;
    ++assertions;
}
static void pump(App &a) {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT)
            break;
        if (!a.key(m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}
static void pressEnter(App &a, HWND field) {
    SetFocus(field);
    LPARAM keyData = 1 | (MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC) << 16);
    PostMessageW(field, WM_KEYDOWN, VK_RETURN, keyData);
    PostMessageW(field, WM_KEYUP, VK_RETURN, keyData | (3u << 30));
    pump(a);
}
static void checkEnter(App &a, HWND field, const Text &before, Range selection, const Text &after, int caret,
                       const char *name) {
    if (field == a.code)
        a.resetDoc(before);
    else
        ui::set(field, before);
    ui::select(field, selection);
    pressEnter(a, field);
    auto actual = ui::get(field);
    auto pos = ui::selection(field);
    if (actual != after || pos.a != caret || pos.b != caret)
        std::cerr << "Expected " << std::count(after.begin(), after.end(), L'\n') << " breaks and caret "
                  << caret << ", got " << std::count(actual.begin(), actual.end(), L'\n') << " and " << pos.a
                  << ".." << pos.b << '\n';
    ensure(actual == after && pos.a == caret && pos.b == caret, name);
}
static void untilDone(App &a) {
    auto start = GetTickCount64();
    while (a.job && GetTickCount64() - start < 12000) {
        pump(a);
        Sleep(10);
    }
    ensure(!a.job, "run completed through window timer");
}
int main() {
    try {
        platform();
        App a;
        a.create();
        ShowWindow(a.win, SW_SHOWNOACTIVATE);
        UpdateWindow(a.win);
        ensure(a.doc != nullptr, "native text document");
        ensure(ui::get(a.code) == sample, "editor initial document");
        SendMessageW(a.runBtn, BM_CLICK, 0, 0);
        untilDone(a);
        ensure(ui::get(a.output) == L"14\n", "run button executes sample");
        ensure(IsWindowEnabled(a.runBtn) && !IsWindowEnabled(a.stopBtn), "idle button state");
        checkEnter(a, a.input, L"1 2", {3, 3}, L"1 2\n", 4, "Enter in stdin inserts a single line break");
        checkEnter(a, a.code, L"x = 1", {5, 5}, L"x = 1\n", 6, "Enter at end of code inserts one break");
        checkEnter(a, a.code, L"if True:\n    pass", {8, 8}, L"if True:\n    \n    pass", 13,
                   "Enter before an existing break indents exactly one line");
        const Text nestedHeader = L"for n in [2]:  # 外側";
        a.resetDoc(nestedHeader);
        ui::select(a.code, {edit::length(nestedHeader), edit::length(nestedHeader)});
        pressEnter(a, a.code);
        for (wchar_t c : Text(L"if n > 0:  # 内側"))
            SendMessageW(a.code, WM_CHAR, c, 1);
        pressEnter(a, a.code);
        const Text nestedBody = nestedHeader + L"\n    if n > 0:  # 内側\n        ";
        ensure(ui::get(a.code) == nestedBody && ui::selection(a.code).b == edit::length(nestedBody),
               "type nested for/if headers and land at column nine");
        SendMessageW(a.code, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"print(n)"));
        a.cmd(ID_RUN);
        untilDone(a);
        ensure(ui::get(a.output) == L"2\n", "automatically indented nested code runs");
        checkEnter(a, a.code, L"# 日本語\nx", {5, 5}, L"# 日本語\n\nx", 6,
                   "Enter after committed Japanese text inserts one break");
        checkEnter(a, a.code, L"ab\ncd", {1, 4}, L"a\nd", 2, "Enter replaces a multiline selection once");
        checkEnter(a, a.code, L"a\n", {2, 2}, L"a\n\n", 3, "Enter on empty final line inserts one break");
        pressEnter(a, a.code);
        ensure(ui::get(a.code) == L"a\n\n\n", "two consecutive Enter presses each add one break");
        a.resetDoc(L"# ");
        ui::select(a.code, {2, 2});
        SendMessageW(a.code, WM_IME_CHAR, L'字', 1);
        ensure(ui::get(a.code) == L"# 字", "editor accepts an IME-committed character");
        pressEnter(a, a.code);
        ensure(ui::get(a.code) == L"# 字\n", "Enter following an IME commit inserts one break");
        a.resetDoc(L"if True:");
        ui::select(a.code, {8, 8});
        pressEnter(a, a.code);
        ensure(ui::get(a.code) == L"if True:\n    ", "Enter indents through editor");
        SendMessageW(a.code, WM_CHAR, '(', 0);
        ensure(ui::get(a.code) == L"if True:\n    ()" && ui::selection(a.code).a == 14,
               "pair insertion and caret");
        SendMessageW(a.win, WM_TIMER, syntaxTimer, 0);
        a.cmd(ID_UNDO);
        ensure(ui::get(a.code) == L"if True:\n    ", "coloring preserves undo");
        a.cmd(ID_REDO);
        ensure(ui::get(a.code) == L"if True:\n    ()", "redo after coloring");
        a.resetDoc(L"val + val + val");
        ui::select(a.code, {0, 0});
        for (int i = 0; i < 3; i++)
            a.cmd(ID_ADD);
        ensure(a.marks.size() == 3, "select next occurrence three times");
        SendMessageW(a.code, WM_CHAR, 'x', 0);
        SendMessageW(a.code, WM_CHAR, 'y', 0);
        ensure(ui::get(a.code) == L"xy + xy + xy", "type at all three selections");
        a.cmd(ID_UNDO);
        ensure(ui::get(a.code) == L"x + x + x", "undo synchronized typing as one edit");
        a.resetDoc(L"x+x");
        ui::select(a.code, {0, 1});
        a.cmd(ID_ADD);
        pressEnter(a, a.code);
        ensure(ui::get(a.code) == L"\n+\n", "one Enter replaces each selected match with one break");
        a.cmd(ID_UNDO);
        ensure(ui::get(a.code) == L"x+x", "one undo reverses a multiple-selection Enter");
        const Text mixed = L"if ok:XX\n    for x in xs:XX";
        a.resetDoc(mixed);
        ui::select(a.code, {6, 8});
        a.cmd(ID_ADD);
        pressEnter(a, a.code);
        ensure(ui::get(a.code) == L"if ok:\n    \n    for x in xs:\n        ",
               "simultaneous Enter computes indentation separately for each header");
        a.cmd(ID_UNDO);
        ensure(ui::get(a.code) == mixed, "undo restores both header selections in one edit");
        a.clearMarks();
        a.resetDoc(L"a\nb");
        ui::select(a.code, {0, 3});
        a.cmd(ID_COMMENT);
        ensure(ui::get(a.code) == L"# a\n# b", "toggle block comment");
        a.cmd(ID_COMMENT);
        ensure(ui::get(a.code) == L"a\nb", "uncomment block");
        ui::select(a.code, {0, 0});
        a.cmd(ID_MOVE_DOWN);
        ensure(ui::get(a.code) == L"b\na", "move line through editor");
        a.cmd(ID_COPY_UP);
        ensure(ui::get(a.code) == L"b\na\na", "duplicate line");
        wcscpy_s(a.needle, L"a");
        wcscpy_s(a.replacement, L"ok");
        a.find.Flags = FR_DOWN | FR_REPLACEALL;
        a.findAction(&a.find);
        ensure(ui::get(a.code) == L"b\nok\nok", "replace all via find handler");
        a.find.Flags = FR_DOWN;
        ui::select(a.code, {0, 0});
        wcscpy_s(a.needle, L"ok");
        a.cmd(ID_NEXT);
        ensure(ui::selection(a.code).a == 2 && ui::selection(a.code).b == 4, "find next selects result");
        a.resetDoc(L"while True: pass");
        a.cmd(ID_RUN);
        ensure(a.job && IsWindowEnabled(a.stopBtn), "stop enabled during run");
        a.cmd(ID_STOP);
        untilDone(a);
        ensure(a.resultLabel == L"停止しました", "stop action");
        a.resetDoc(L"print('日本語 🐍')\nraise ValueError('bad')");
        a.cmd(ID_RUN);
        untilDone(a);
        auto out = ui::get(a.output);
        ensure(out.find(L"日本語 🐍") != Text::npos && out.find(L"ValueError: bad") != Text::npos &&
                   a.resultLabel == L"実行エラー",
               "Japanese output and traceback in result pane");
        // The isolated test directory never shares state with an actual PyDesk session.
        a.sessionDir = std::filesystem::absolute(Path("build") / L"ui-test-state");
        std::filesystem::create_directories(a.sessionDir);
        auto file = a.sessionDir / L"日本語.py";
        ui::save(file, L"\ufeffprint(0)\r\n");
        a.loadFile(file);
        ensure(a.bom && a.crlf, "load BOM and CRLF file");
        ui::replace(a.code, {6, 7}, L"9");
        a.saveFile(false);
        ensure(ui::read(file) == L"print(9)\r\n" && !a.dirty, "save source preserving CRLF");
        a.loadFile(file);
        ensure(a.bom && ui::get(a.code) == L"print(9)\n", "save preserves BOM");
        a.resetDoc(L"print('下書き')\n");
        ui::set(a.input, L"5\n日本語\n");
        a.cmd(ID_T60);
        a.remember();
        a.resetDoc(L"");
        ui::set(a.input, L"");
        a.restore();
        ensure(ui::get(a.code) == L"print('下書き')\n" && ui::get(a.input) == L"5\n日本語\n" &&
                   a.timeout == 60000,
               "restore scratch code, stdin and timeout");
        a.resetDoc(sample);
        ui::set(a.input, L"3 1 4 1 5\n");
        a.cmd(ID_RUN);
        untilDone(a);
        UpdateWindow(a.win);
        pump(a);
        // Capture only this app, regardless of desktop layout or other open windows.
        RECT area{};
        GetClientRect(a.win, &area);
        int w = area.right, h = area.bottom;
        HDC screen = GetDC(a.win), dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void *pixels = nullptr;
        auto bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        SelectObject(dc, bmp);
        PrintWindow(a.win, dc, PW_CLIENTONLY | 2);
        GdiFlush();
        std::ofstream image("build/ui-preview.bmp", std::ios::binary);
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4d42;
        fh.bfOffBits = sizeof(fh) + sizeof(info.bmiHeader);
        fh.bfSize = fh.bfOffBits + w * h * 4;
        image.write(reinterpret_cast<char *>(&fh), sizeof(fh));
        image.write(reinterpret_cast<char *>(&info.bmiHeader), sizeof(info.bmiHeader));
        image.write(static_cast<char *>(pixels), static_cast<std::streamsize>(w) * h * 4);
        image.close();
        DeleteDC(dc);
        DeleteObject(bmp);
        ReleaseDC(a.win, screen);
        DestroyWindow(a.win);
        OleUninitialize();
        std::cout << assertions << " UI checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
