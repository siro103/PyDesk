#include "editor.hpp"
#include "runner.hpp"
#include <iostream>
#include <fstream>

static int count = 0;
static void expect(bool ok, const char *name) {
    if (!ok)
        throw std::runtime_error(name);
    std::cout << "ok  " << name << std::endl;
    ++count;
}
static std::wstring changed(std::wstring s, const edit::Change &c) {
    return s.replace(c.old.a, c.old.b - c.old.a, c.text);
}
int main() {
    try {
        std::wstring s = L"if x:\n    first\n    last";
        expect(edit::newline(s, {5, 5}).text == L"\n    ", "colon indentation");
        expect(edit::newline(s, {15, 15}).text == L"\n    ", "preserve indentation");
        auto enterAtEnd = [](const std::wstring &source) {
            int p = edit::length(source);
            return edit::newline(source, {p, p}).text;
        };
        expect(enterAtEnd(L"if n > 0:  # positive") == L"\n    ", "if header with trailing comment");
        expect(enterAtEnd(L"    for x in xs:  # nested") == L"\n        ",
               "nested for body adds four spaces");
        expect(enterAtEnd(L"if x == '#:('  # typing") == L"\n    ",
               "unfinished if header and quoted punctuation");
        expect(enterAtEnd(L"for x in xs") == L"\n    ", "unfinished for header");
        expect(enterAtEnd(L"    if (\n        value):  # done") == L"\n        ",
               "multiline header uses statement indentation");
        expect(enterAtEnd(L"    if x: print('#')  # more:") == L"\n    ",
               "inline suite keeps its indentation");
        expect(enterAtEnd(L"    # for x in xs:") == L"\n    ", "comment does not start a block");
        expect(enterAtEnd(L"s = '''text\nif x:") == L"\n",
               "text inside triple quotes does not start a block");
        expect(enterAtEnd(L"values = [x\n    for x in xs") == L"\n    ",
               "comprehension keeps continuation indentation");
        expect(enterAtEnd(L"format_value = 2") == L"\n", "identifier prefix does not start a block");
        expect(enterAtEnd(L"values = [  # list") == L"\n    ", "opening bracket before a comment");
        auto c = edit::block(s, {6, 16}, edit::Block::comment);
        auto after = changed(s, c);
        expect(after == L"if x:\n    # first\n    last", "whole-line selection excludes next line");
        expect(changed(after, edit::block(after, c.selection, edit::Block::comment)) == s, "toggle comments");
        expect(changed(s, edit::moveLines(s, {10, 10}, true, false)) == L"if x:\n    last\n    first",
               "move last two lines");
        expect(changed(L"abc", edit::moveLines(L"abc", {1, 1}, false, true)) == L"abc\nabc",
               "duplicate line");
        auto colors = edit::scan(L"x = '''#a\nb'''  # comment\nprint(10)");
        expect(colors.size() == 4 && colors[0].ink == edit::Ink::string &&
                   colors[1].ink == edit::Ink::comment,
               "multiline tokenizer");
        std::atomic<bool> stopped{false};
        std::string out;
        auto invoke = [&](const std::wstring &src, const std::wstring &in = L"", int t = 10000) {
            out.clear();
            return run::execute(src, in, std::filesystem::current_path() / L"例 題.py", t, stopped,
                                [&](const char *p, size_t n) { out.append(p, n); });
        };
        auto r =
            invoke(L"n=int(input())\nprint(sum(map(int,input().split())))\nprint('日本語 🐍')", L"3\n1 2 3");
        expect(r.state == run::State::success && out.find("6") != out.npos &&
                   out.find(u8"日本語 🐍") != out.npos,
               "UTF-8 stdin and stdout with EOF");
        r = invoke(L"print('abc')\nraise ValueError('boom')");
        if (out.find("<string>") != out.npos || out.find("ValueError: boom") == out.npos ||
            out.find("line 2") == out.npos)
            std::cerr << out << std::endl;
        expect(r.code != 0 && out.find("ValueError: boom") != out.npos && out.find("line 2") != out.npos &&
                   out.find("<string>") == out.npos,
               "traceback points into user buffer");
        r = invoke(L"if True print(1)");
        expect(r.state == run::State::error && out.find("SyntaxError") != out.npos, "syntax error");
        r = invoke(L"import sys\nprint(len(sys.stdin.read()))", std::wstring(600000, L'x'));
        expect(out.find("600000") != out.npos, "large stdin");
        r = invoke(L"import sys\nsys.stdout.write('a'*1500000)");
        expect(r.code == 0 && out.size() > run::outputLimit && out.size() < run::outputLimit + 200,
               "bounded output");
        r = invoke(L"while True: pass", L"", 180);
        expect(r.state == run::State::timeout && r.ms < 4000, "timeout");
        stopped = true;
        r = invoke(L"while True: pass");
        expect(r.state == run::State::stopped && r.ms < 4000, "cancel");
        stopped = false;
        out.clear();
        r = run::execute(L"import subprocess, sys, time\np=subprocess.Popen([sys.executable,'-c','import "
                         L"time; time.sleep(40)'])\nprint(p.pid, flush=True)\ntime.sleep(40)",
                         L"", std::filesystem::current_path() / L"parent.py", 5000, stopped,
                         [&](const char *p, size_t n) {
                             out.append(p, n);
                             if (out.find('\n') != out.npos)
                                 stopped = true;
                         });
        expect(r.state == run::State::stopped, "stop after streamed output from live program");
        run::Handle descendant(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(out))));
        expect(!descendant.h || WaitForSingleObject(descendant.h, 1500) == WAIT_OBJECT_0,
               "stop cleans up spawned Python child");
        stopped = false;
        r = invoke(L"print(__name__, __file__)\nimport pathlib\nprint(pathlib.Path.cwd() == "
                   L"pathlib.Path(__file__).parent)");
        expect(r.code == 0 && out.find("__main__") != out.npos && out.find(u8"例 題.py") != out.npos &&
                   out.find("True") != out.npos,
               "script name and working directory");
        std::cout << count << " checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
