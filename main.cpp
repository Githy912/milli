#include "main.hpp"

#include <windows.h>

#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

static HANDLE hInput = INVALID_HANDLE_VALUE;
static HANDLE hOutput = INVALID_HANDLE_VALUE;

static std::vector<std::wstring> lines{L""};
static std::wstring filename;

static int cursorX = 0;
static int cursorY = 0;
static int scrollX = 0;
static int scrollY = 0;

static bool running = true;
static bool modified = false;
static int helpScroll = 0;
static bool helpVisible = false;

static int screenWidth = 80;
static int screenHeight = 25;

static std::vector<CHAR_INFO> screenBuffer;

static DWORD originalInputMode = 0;

struct Position {
    int x;
    int y;
};

static bool selectionActive = false;
static bool selectingWithMouse = false;
static Position selectionAnchor{};
static Position selectionCursor{};

struct EditorState {
    std::vector<std::wstring> lines;
    int cursorX;
    int cursorY;
};

static std::vector<EditorState> undoStack;
static std::vector<EditorState> redoStack;

static constexpr size_t MAX_HISTORY = 500;

// -----------------------------------------------------------------------------
// Terminal snapshot
// -----------------------------------------------------------------------------

static std::vector<CHAR_INFO> originalScreen;
static COORD originalScreenSize{};
static SMALL_RECT originalWindow{};
static COORD originalCursorPosition{};
static bool terminalSnapshotValid = false;

static void saveTerminalSnapshot() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};

    if (!GetConsoleScreenBufferInfo(hOutput, &csbi))
        return;

    originalScreenSize = csbi.dwSize;
    originalWindow = csbi.srWindow;
    originalCursorPosition = csbi.dwCursorPosition;

    const int width = originalScreenSize.X;
    const int height = originalScreenSize.Y;

    originalScreen.resize(static_cast<size_t>(width) *
                          static_cast<size_t>(height));

    SMALL_RECT entireBuffer{
        0,
        0,
        static_cast<SHORT>(width - 1),
        static_cast<SHORT>(height - 1)
    };

    COORD bufferSize{
        static_cast<SHORT>(width),
        static_cast<SHORT>(height)
    };

    COORD bufferCoord{0, 0};

    terminalSnapshotValid =
        ReadConsoleOutputW(
            hOutput,
            originalScreen.data(),
            bufferSize,
            bufferCoord,
            &entireBuffer
        ) != FALSE;
}

static void restoreTerminalSnapshot() {
    if (!terminalSnapshotValid)
        return;

    COORD bufferSize = originalScreenSize;
    COORD bufferCoord{0, 0};

    SMALL_RECT entireBuffer{
        0,
        0,
        static_cast<SHORT>(originalScreenSize.X - 1),
        static_cast<SHORT>(originalScreenSize.Y - 1)
    };

    WriteConsoleOutputW(
        hOutput,
        originalScreen.data(),
        bufferSize,
        bufferCoord,
        &entireBuffer
    );

    SetConsoleWindowInfo(
        hOutput,
        TRUE,
        &originalWindow
    );

    SetConsoleCursorPosition(
        hOutput,
        originalCursorPosition
    );

    terminalSnapshotValid = false;
}

// -----------------------------------------------------------------------------
// UTF-8
// -----------------------------------------------------------------------------

static std::wstring utf8ToWide(const std::string& text) {
    if (text.empty())
        return L"";

    int size = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0
    );

    if (size <= 0)
        return L"";

    std::wstring result(size, L'\0');

    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size
    );

    return result;
}

static std::string wideToUtf8(const std::wstring& text) {
    if (text.empty())
        return "";

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (size <= 0)
        return "";

    std::string result(size, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );

    return result;
}

// -----------------------------------------------------------------------------
// Terminal
// -----------------------------------------------------------------------------

static void clearTerminal() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};

    if (!GetConsoleScreenBufferInfo(hOutput, &csbi))
        return;

    DWORD cells =
        static_cast<DWORD>(csbi.dwSize.X) *
        static_cast<DWORD>(csbi.dwSize.Y);

    COORD origin{0, 0};

    DWORD written = 0;

    FillConsoleOutputCharacterW(
        hOutput,
        L' ',
        cells,
        origin,
        &written
    );

    FillConsoleOutputAttribute(
        hOutput,
        csbi.wAttributes,
        cells,
        origin,
        &written
    );

    SetConsoleCursorPosition(hOutput, origin);
}

static void enableConsole() {
    hInput = GetStdHandle(STD_INPUT_HANDLE);
    hOutput = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hInput == INVALID_HANDLE_VALUE ||
        hOutput == INVALID_HANDLE_VALUE)
        return;

    GetConsoleMode(hInput, &originalInputMode);

    DWORD mode =
        ENABLE_EXTENDED_FLAGS |
        ENABLE_WINDOW_INPUT |
        ENABLE_MOUSE_INPUT;

    SetConsoleMode(hInput, mode);

    CONSOLE_CURSOR_INFO cursorInfo{};
    cursorInfo.dwSize = 1;
    cursorInfo.bVisible = TRUE;

    SetConsoleCursorInfo(hOutput, &cursorInfo);
}

static void disableConsole() {
    if (hInput == INVALID_HANDLE_VALUE)
        return;

    SetConsoleMode(hInput, originalInputMode);
}

// -----------------------------------------------------------------------------
// Screen
// -----------------------------------------------------------------------------

static void updateScreenSize() {
    CONSOLE_SCREEN_BUFFER_INFO csbi{};

    if (!GetConsoleScreenBufferInfo(hOutput, &csbi))
        return;

    screenWidth =
        csbi.srWindow.Right -
        csbi.srWindow.Left + 1;

    screenHeight =
        csbi.srWindow.Bottom -
        csbi.srWindow.Top + 1;

    if (screenWidth < 20)
        screenWidth = 20;

    if (screenHeight < 5)
        screenHeight = 5;

    screenBuffer.resize(
        static_cast<size_t>(screenWidth) *
        static_cast<size_t>(screenHeight)
    );
}

static void fillBuffer(wchar_t ch, WORD attr) {
    for (auto& cell : screenBuffer) {
        cell.Char.UnicodeChar = ch;
        cell.Attributes = attr;
    }
}

static void putChar(
    int x,
    int y,
    wchar_t ch,
    WORD attr
) {
    if (x < 0 || y < 0 ||
        x >= screenWidth ||
        y >= screenHeight)
        return;

    size_t index =
        static_cast<size_t>(y) *
        static_cast<size_t>(screenWidth) +
        static_cast<size_t>(x);

    screenBuffer[index].Char.UnicodeChar = ch;
    screenBuffer[index].Attributes = attr;
}

static void putText(
    int x,
    int y,
    const std::wstring& text,
    WORD attr
) {
    for (size_t i = 0; i < text.size(); ++i) {
        putChar(
            x + static_cast<int>(i),
            y,
            text[i],
            attr
        );
    }
}

static void present() {
    SMALL_RECT target{
        0,
        0,
        static_cast<SHORT>(screenWidth - 1),
        static_cast<SHORT>(screenHeight - 1)
    };

    COORD size{
        static_cast<SHORT>(screenWidth),
        static_cast<SHORT>(screenHeight)
    };

    COORD origin{0, 0};

    WriteConsoleOutputW(
        hOutput,
        screenBuffer.data(),
        size,
        origin,
        &target
    );
}

// -----------------------------------------------------------------------------
// Selection
// -----------------------------------------------------------------------------

static Position normalizePosition(Position p) {
    if (lines.empty())
        return {0, 0};

    p.y = std::clamp(
        p.y,
        0,
        static_cast<int>(lines.size()) - 1
    );

    p.x = std::clamp(
        p.x,
        0,
        static_cast<int>(lines[p.y].size())
    );

    return p;
}

static bool positionLess(Position a, Position b) {
    if (a.y != b.y)
        return a.y < b.y;

    return a.x < b.x;
}

static bool positionsEqual(Position a, Position b) {
    return a.x == b.x && a.y == b.y;
}

static void getOrderedSelection(
    Position& start,
    Position& end
) {
    start = normalizePosition(selectionAnchor);
    end = normalizePosition(selectionCursor);

    if (positionLess(end, start))
        std::swap(start, end);
}

static bool positionInSelection(Position p) {
    if (!selectionActive)
        return false;

    Position start;
    Position end;

    getOrderedSelection(start, end);

    if (positionsEqual(start, end))
        return false;

    if (p.y < start.y || p.y > end.y)
        return false;

    if (start.y == end.y)
        return p.y == start.y &&
               p.x >= start.x &&
               p.x < end.x;

    if (p.y == start.y)
        return p.x >= start.x;

    if (p.y == end.y)
        return p.x < end.x;

    return true;
}

static void clearSelection() {
    selectionActive = false;
    selectingWithMouse = false;
}

static void selectAll() {
    if (lines.empty()) {
        lines.push_back(L"");
    }

    selectionAnchor = {0, 0};

    selectionCursor = {
        static_cast<int>(lines.back().size()),
        static_cast<int>(lines.size()) - 1
    };

    selectionActive = true;

    cursorX = selectionCursor.x;
    cursorY = selectionCursor.y;
}

// -----------------------------------------------------------------------------
// Undo / redo
// -----------------------------------------------------------------------------

static EditorState currentState() {
    return {
        lines,
        cursorX,
        cursorY
    };
}

static void restoreState(const EditorState& state) {
    lines = state.lines;
    cursorX = state.cursorX;
    cursorY = state.cursorY;

    if (lines.empty())
        lines.push_back(L"");

    cursorY = std::clamp(
        cursorY,
        0,
        static_cast<int>(lines.size()) - 1
    );

    cursorX = std::clamp(
        cursorX,
        0,
        static_cast<int>(lines[cursorY].size())
    );

    clearSelection();
}

static void saveUndoState() {
    undoStack.push_back(currentState());

    if (undoStack.size() > MAX_HISTORY)
        undoStack.erase(undoStack.begin());

    redoStack.clear();
}

static void undo() {
    if (undoStack.empty())
        return;

    redoStack.push_back(currentState());

    EditorState state = undoStack.back();
    undoStack.pop_back();

    restoreState(state);
    modified = true;
}

static void redo() {
    if (redoStack.empty())
        return;

    undoStack.push_back(currentState());

    EditorState state = redoStack.back();
    redoStack.pop_back();

    restoreState(state);
    modified = true;
}

// -----------------------------------------------------------------------------
// File
// -----------------------------------------------------------------------------

static bool loadFile(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize{};

    if (!GetFileSizeEx(file, &fileSize)) {
        CloseHandle(file);
        return false;
    }

    if (fileSize.QuadPart > 100 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    std::string data(
        static_cast<size_t>(fileSize.QuadPart),
        '\0'
    );

    DWORD read = 0;

    if (!data.empty()) {
        if (!ReadFile(
                file,
                data.data(),
                static_cast<DWORD>(data.size()),
                &read,
                nullptr
            )) {
            CloseHandle(file);
            return false;
        }
    }

    CloseHandle(file);

    data.resize(read);

    std::wstring text = utf8ToWide(data);

    lines.clear();

    size_t start = 0;

    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);

        if (end == std::wstring::npos) {
            std::wstring line = text.substr(start);

            if (!line.empty() && line.back() == L'\r')
                line.pop_back();

            lines.push_back(line);
            break;
        }

        std::wstring line =
            text.substr(start, end - start);

        if (!line.empty() && line.back() == L'\r')
            line.pop_back();

        lines.push_back(line);

        start = end + 1;
    }

    if (lines.empty())
        lines.push_back(L"");

    filename = path;
    cursorX = 0;
    cursorY = 0;
    scrollX = 0;
    scrollY = 0;

    modified = false;

    undoStack.clear();
    redoStack.clear();

    clearSelection();

    return true;
}

static bool saveFile(const std::wstring& path) {
    std::wstring text;

    for (size_t i = 0; i < lines.size(); ++i) {
        text += lines[i];

        if (i + 1 < lines.size())
            text += L"\r\n";
    }

    std::string data = wideToUtf8(text);

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;

    bool success = true;

    if (!data.empty()) {
        success =
            WriteFile(
                file,
                data.data(),
                static_cast<DWORD>(data.size()),
                &written,
                nullptr
            ) != FALSE;
    }

    CloseHandle(file);

    if (!success || written != data.size())
        return false;

    filename = path;
    modified = false;

    return true;
}

static bool saveAs() {
    DWORD oldMode = 0;
    GetConsoleMode(hInput, &oldMode);

    SetConsoleMode(
        hInput,
        ENABLE_EXTENDED_FLAGS
    );

    SetConsoleCursorPosition(
        hOutput,
        {0, static_cast<SHORT>(screenHeight - 1)}
    );

    std::wstring path;

    WriteConsoleW(
        hOutput,
        L"Save as: ",
        9,
        nullptr,
        nullptr
    );

    DWORD mode = 0;
    GetConsoleMode(hOutput, &mode);

    std::getline(std::wcin, path);

    SetConsoleMode(hInput, oldMode);

    if (path.empty())
        return false;

    return saveFile(path);
}

static bool saveCurrent() {
    if (filename.empty())
        return saveAs();

    return saveFile(filename);
}

// -----------------------------------------------------------------------------
// Editing helpers
// -----------------------------------------------------------------------------

static bool isWordCharacter(wchar_t c) {
    return
        (c >= L'a' && c <= L'z') ||
        (c >= L'A' && c <= L'Z') ||
        (c >= L'0' && c <= L'9') ||
        c == L'_';
}

static void deleteSelection() {
    if (!selectionActive)
        return;

    Position start;
    Position end;

    getOrderedSelection(start, end);

    if (positionsEqual(start, end)) {
        clearSelection();
        return;
    }

    saveUndoState();

    if (start.y == end.y) {
        lines[start.y].erase(
            static_cast<size_t>(start.x),
            static_cast<size_t>(end.x - start.x)
        );
    } else {
        std::wstring remaining =
            lines[end.y].substr(
                static_cast<size_t>(end.x)
            );

        lines[start.y].erase(
            static_cast<size_t>(start.x)
        );

        lines[start.y] += remaining;

        lines.erase(
            lines.begin() + start.y + 1,
            lines.begin() + end.y + 1
        );
    }

    cursorX = start.x;
    cursorY = start.y;

    modified = true;

    clearSelection();
}

static void insertCharacter(wchar_t c) {
    if (selectionActive)
        deleteSelection();

    saveUndoState();

    lines[cursorY].insert(
        lines[cursorY].begin() + cursorX,
        c
    );

    ++cursorX;
    modified = true;
}

static void insertNewLine() {
    if (selectionActive)
        deleteSelection();

    saveUndoState();

    std::wstring right =
        lines[cursorY].substr(
            static_cast<size_t>(cursorX)
        );

    lines[cursorY].erase(
        static_cast<size_t>(cursorX)
    );

    lines.insert(
        lines.begin() + cursorY + 1,
        right
    );

    ++cursorY;
    cursorX = 0;

    modified = true;
}

static void backspace() {
    if (selectionActive) {
        deleteSelection();
        return;
    }

    if (cursorX > 0) {
        saveUndoState();

        lines[cursorY].erase(
            static_cast<size_t>(cursorX - 1),
            1
        );

        --cursorX;
        modified = true;
        return;
    }

    if (cursorY > 0) {
        saveUndoState();

        cursorX =
            static_cast<int>(
                lines[cursorY - 1].size()
            );

        lines[cursorY - 1] += lines[cursorY];

        lines.erase(
            lines.begin() + cursorY
        );

        --cursorY;

        modified = true;
    }
}

static void deleteCharacter() {
    if (selectionActive) {
        deleteSelection();
        return;
    }

    if (cursorX <
        static_cast<int>(lines[cursorY].size())) {

        saveUndoState();

        lines[cursorY].erase(
            static_cast<size_t>(cursorX),
            1
        );

        modified = true;
        return;
    }

    if (cursorY + 1 <
        static_cast<int>(lines.size())) {

        saveUndoState();

        lines[cursorY] += lines[cursorY + 1];

        lines.erase(
            lines.begin() + cursorY + 1
        );

        modified = true;
    }
}

static void ctrlBackspace() {
    if (selectionActive) {
        deleteSelection();
        return;
    }

    if (cursorX == 0) {
        backspace();
        return;
    }

    int target = cursorX;

    while (target > 0 &&
           !isWordCharacter(lines[cursorY][target - 1])) {
        --target;
    }

    while (target > 0 &&
           isWordCharacter(lines[cursorY][target - 1])) {
        --target;
    }

    if (target == cursorX)
        return;

    saveUndoState();

    lines[cursorY].erase(
        static_cast<size_t>(target),
        static_cast<size_t>(cursorX - target)
    );

    cursorX = target;
    modified = true;
}

static void ctrlDelete() {
    if (selectionActive) {
        deleteSelection();
        return;
    }

    std::wstring& line = lines[cursorY];

    if (cursorX >= static_cast<int>(line.size())) {
        deleteCharacter();
        return;
    }

    int target = cursorX;

    while (target < static_cast<int>(line.size()) &&
           !isWordCharacter(line[target])) {
        ++target;
    }

    while (target < static_cast<int>(line.size()) &&
           isWordCharacter(line[target])) {
        ++target;
    }

    if (target == cursorX)
        return;

    saveUndoState();

    line.erase(
        static_cast<size_t>(cursorX),
        static_cast<size_t>(target - cursorX)
    );

    modified = true;
}

// -----------------------------------------------------------------------------
// Clipboard
// -----------------------------------------------------------------------------

static bool setClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr))
        return false;

    EmptyClipboard();

    SIZE_T bytes =
        (text.size() + 1) * sizeof(wchar_t);

    HGLOBAL memory =
        GlobalAlloc(GMEM_MOVEABLE, bytes);

    if (!memory) {
        CloseClipboard();
        return false;
    }

    void* ptr = GlobalLock(memory);

    if (!ptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }

    memcpy(
        ptr,
        text.c_str(),
        bytes
    );

    GlobalUnlock(memory);

    SetClipboardData(
        CF_UNICODETEXT,
        memory
    );

    CloseClipboard();

    return true;
}

static std::wstring getClipboardText() {
    std::wstring result;

    if (!OpenClipboard(nullptr))
        return result;

    HANDLE data =
        GetClipboardData(CF_UNICODETEXT);

    if (data) {
        wchar_t* ptr =
            static_cast<wchar_t*>(
                GlobalLock(data)
            );

        if (ptr) {
            result = ptr;
            GlobalUnlock(data);
        }
    }

    CloseClipboard();

    return result;
}

static std::wstring getSelectedText() {
    if (!selectionActive)
        return L"";

    Position start;
    Position end;

    getOrderedSelection(start, end);

    if (positionsEqual(start, end))
        return L"";

    std::wstring result;

    if (start.y == end.y) {
        result =
            lines[start.y].substr(
                static_cast<size_t>(start.x),
                static_cast<size_t>(end.x - start.x)
            );

        return result;
    }

    result +=
        lines[start.y].substr(
            static_cast<size_t>(start.x)
        );

    result += L"\r\n";

    for (int y = start.y + 1; y < end.y; ++y) {
        result += lines[y];
        result += L"\r\n";
    }

    result +=
        lines[end.y].substr(
            0,
            static_cast<size_t>(end.x)
        );

    return result;
}

static void copySelection() {
    if (!selectionActive)
        return;

    std::wstring text = getSelectedText();

    if (!text.empty())
        setClipboardText(text);
}

static void copyEntireLine() {
    if (lines.empty())
        return;

    setClipboardText(lines[cursorY]);
}

static void paste() {
    std::wstring text = getClipboardText();

    if (text.empty())
        return;

    if (selectionActive)
        deleteSelection();

    saveUndoState();

    std::vector<std::wstring> pastedLines;

    size_t start = 0;

    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);

        if (end == std::wstring::npos) {
            std::wstring line =
                text.substr(start);

            if (!line.empty() &&
                line.back() == L'\r')
                line.pop_back();

            pastedLines.push_back(line);
            break;
        }

        std::wstring line =
            text.substr(
                start,
                end - start
            );

        if (!line.empty() &&
            line.back() == L'\r')
            line.pop_back();

        pastedLines.push_back(line);

        start = end + 1;
    }

    if (pastedLines.empty())
        return;

    std::wstring right =
        lines[cursorY].substr(
            static_cast<size_t>(cursorX)
        );

    lines[cursorY].erase(
        static_cast<size_t>(cursorX)
    );

    lines[cursorY] += pastedLines[0];

    for (size_t i = 1; i < pastedLines.size(); ++i) {
        lines.insert(
            lines.begin() + cursorY + static_cast<int>(i),
            pastedLines[i]
        );
    }

    cursorY +=
        static_cast<int>(pastedLines.size()) - 1;

    cursorX =
        static_cast<int>(
            lines[cursorY].size()
        );

    lines[cursorY] += right;

    modified = true;
}

// -----------------------------------------------------------------------------
// Mouse
// -----------------------------------------------------------------------------

static Position screenToDocument(COORD mouse) {
    constexpr int lineNumberWidth = 7;
    constexpr int editorTop = 1;

    int y =
        mouse.Y -
        editorTop +
        scrollY;

    int x =
        mouse.X -
        lineNumberWidth +
        scrollX;

    if (y < 0)
        y = 0;

    if (y >= static_cast<int>(lines.size()))
        y = static_cast<int>(lines.size()) - 1;

    if (y < 0)
        y = 0;

    x = std::max(x, 0);

    if (y < static_cast<int>(lines.size()))
        x = std::min(
            x,
            static_cast<int>(lines[y].size())
        );

    return {x, y};
}

static void handleMouse(
    const MOUSE_EVENT_RECORD& mouse
) {
    if (helpVisible)
        return;

    if (mouse.dwEventFlags == 0) {
        if (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) {
            Position p =
                screenToDocument(mouse.dwMousePosition);

            selectionAnchor = p;
            selectionCursor = p;

            cursorX = p.x;
            cursorY = p.y;

            selectionActive = false;
            selectingWithMouse = true;
        }
    }

    if (mouse.dwEventFlags == MOUSE_MOVED) {
        if (selectingWithMouse &&
            (mouse.dwButtonState &
             FROM_LEFT_1ST_BUTTON_PRESSED)) {

            Position p =
                screenToDocument(mouse.dwMousePosition);

            selectionCursor = p;

            cursorX = p.x;
            cursorY = p.y;

            selectionActive =
                !positionsEqual(
                    selectionAnchor,
                    selectionCursor
                );
        }
    }

    if (mouse.dwEventFlags == 0 &&
        !(mouse.dwButtonState &
          FROM_LEFT_1ST_BUTTON_PRESSED)) {

        if (selectingWithMouse) {
            Position p =
                screenToDocument(mouse.dwMousePosition);

            selectionCursor = p;

            cursorX = p.x;
            cursorY = p.y;

            selectionActive =
                !positionsEqual(
                    selectionAnchor,
                    selectionCursor
                );

            selectingWithMouse = false;
        }
    }
}

// -----------------------------------------------------------------------------
// Scrolling
// -----------------------------------------------------------------------------

static void ensureCursorVisible() {
    const int editorHeight =
        std::max(1, screenHeight - 2);

    if (cursorY < scrollY)
        scrollY = cursorY;

    if (cursorY >= scrollY + editorHeight)
        scrollY =
            cursorY - editorHeight + 1;

    constexpr int lineNumberWidth = 7;

    const int editorWidth =
        std::max(
            1,
            screenWidth - lineNumberWidth
        );

    if (cursorX < scrollX)
        scrollX = cursorX;

    if (cursorX >= scrollX + editorWidth)
        scrollX =
            cursorX - editorWidth + 1;

    scrollY = std::max(scrollY, 0);
    scrollX = std::max(scrollX, 0);
}

static void pageUp() {
    int amount =
        std::max(1, screenHeight - 2);

    cursorY =
        std::max(
            0,
            cursorY - amount
        );

    cursorX =
        std::min(
            cursorX,
            static_cast<int>(lines[cursorY].size())
        );

    clearSelection();
    ensureCursorVisible();
}

static void pageDown() {
    int amount =
        std::max(1, screenHeight - 2);

    cursorY =
        std::min(
            static_cast<int>(lines.size()) - 1,
            cursorY + amount
        );

    cursorX =
        std::min(
            cursorX,
            static_cast<int>(lines[cursorY].size())
        );

    clearSelection();
    ensureCursorVisible();
}

// -----------------------------------------------------------------------------
// Help screen
// -----------------------------------------------------------------------------

static void drawHelp() {
    const WORD blueBackground =
        BACKGROUND_BLUE;

    const WORD panelAttr =
        BACKGROUND_INTENSITY |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED;

    const WORD panelBright =
        BACKGROUND_INTENSITY |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED |
        FOREGROUND_INTENSITY;

    const WORD blackText =
        BACKGROUND_INTENSITY;

    const WORD whiteOnBlue =
        BACKGROUND_BLUE |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED |
        FOREGROUND_INTENSITY;

    fillBuffer(L' ', blueBackground);

    // -----------------------------------------------------------------
    // Header
    // -----------------------------------------------------------------

    for (int x = 0; x < screenWidth; ++x) {
        putChar(
            x,
            0,
            L' ',
            panelBright
        );
    }

    const std::wstring header =
        L"MILLI TEXT EDITOR  |  HELP";

    int headerX =
        std::max(
            1,
            (screenWidth -
             static_cast<int>(header.size())) / 2
        );

    putText(
        headerX,
        0,
        header,
        panelBright
    );

    // -----------------------------------------------------------------
    // Help content
    // -----------------------------------------------------------------

    std::vector<std::wstring> content;

    auto addSection =
        [&](const std::wstring& name) {

        content.push_back(L"");
        content.push_back(
            L"[" + name + L"]"
        );
        content.push_back(
            L"----------------------------------------"
        );
    };

    auto addKey =
        [&](const std::wstring& key,
            const std::wstring& description) {

        content.push_back(
            L"  " +
            key +
            std::wstring(
                std::max(
                    1,
                    24 -
                    static_cast<int>(key.size())
                ),
                L' '
            ) +
            description
        );
    };

    content.push_back(
        L"  Milli Text Editor"
    );

    content.push_back(
        L"  Version 1.0.0"
    );

    content.push_back(
        L"  by Githy912"
    );

    addSection(L"NAVIGATION");

    addKey(L"ARROW KEYS", L"Move the cursor");
    addKey(L"HOME", L"Beginning of line");
    addKey(L"END", L"End of line");
    addKey(L"PAGE UP", L"Scroll one page upward");
    addKey(L"PAGE DOWN", L"Scroll one page downward");

    addSection(L"EDITING");

    addKey(L"BACKSPACE", L"Delete previous character");
    addKey(L"DELETE", L"Delete next character");
    addKey(L"CTRL+BACKSPACE", L"Delete previous word");
    addKey(L"CTRL+DELETE", L"Delete next word");
    addKey(L"ENTER", L"Insert a new line");

    addSection(L"SELECTION");

    addKey(L"CTRL+A", L"Select entire document");
    addKey(L"MOUSE DRAG", L"Select a range of text");

    addSection(L"CLIPBOARD");

    addKey(L"CTRL+ALT+C", L"Copy selected text");
    addKey(L"CTRL+ALT+SHIFT+C", L"Copy current line");
    addKey(L"CTRL+V", L"Paste clipboard contents");

    addSection(L"HISTORY");

    addKey(L"CTRL+Z", L"Undo last edit");
    addKey(L"CTRL+Y", L"Redo last undone edit");

    addSection(L"FILE");

    addKey(L"CTRL+S", L"Save current file");
    addKey(L"CTRL+Q", L"Quit Milli");

    addSection(L"HELP");

    addKey(L"CTRL+H", L"Toggle this help screen");
    addKey(L"ESC", L"Return to editor");
    addKey(L"UP / DOWN", L"Scroll this help screen");
    addKey(L"PAGE UP / DOWN", L"Scroll by one page");
    addKey(L"HOME", L"Go to top of help");
    addKey(L"END", L"Go to bottom of help");

    content.push_back(L"");
    content.push_back(
        L"Use the keyboard to navigate this screen."
    );

    content.push_back(
        L"All controls are available without a mouse."
    );

    // -----------------------------------------------------------------
    // Clamp scroll
    // -----------------------------------------------------------------

    int visibleHeight =
        std::max(
            1,
            screenHeight - 3
        );

    int maxScroll =
        std::max(
            0,
            static_cast<int>(content.size()) -
            visibleHeight
        );

    helpScroll =
        std::clamp(
            helpScroll,
            0,
            maxScroll
        );

    // -----------------------------------------------------------------
    // Main content area
    // -----------------------------------------------------------------

    for (int row = 1;
         row < screenHeight - 2;
         ++row) {

        int contentIndex =
            helpScroll +
            (row - 1);

        if (contentIndex < 0 ||
            contentIndex >=
                static_cast<int>(content.size())) {

            continue;
        }

        const std::wstring& line =
            content[contentIndex];

        bool section =
            line.size() >= 2 &&
            line.front() == L'[' &&
            line.back() == L']';

        bool separator =
            line.find(
                L"----------------------------------------"
            ) != std::wstring::npos;

        WORD attr =
            section
                ? panelBright
                : blackText;

        if (separator)
            attr = panelAttr;

        // Fill the whole row with panel colour.
        for (int x = 0;
             x < screenWidth;
             ++x) {

            putChar(
                x,
                row,
                L' ',
                attr
            );
        }

        // Keep content inside the terminal.
        std::wstring display = line;

        if (static_cast<int>(display.size()) >
            screenWidth - 2) {

            display.resize(
                static_cast<size_t>(
                    screenWidth - 2
                )
            );
        }

        putText(
            1,
            row,
            display,
            attr
        );
    }

    // -----------------------------------------------------------------
    // Footer
    // -----------------------------------------------------------------

    const int footerY =
        screenHeight - 2;

    for (int x = 0;
         x < screenWidth;
         ++x) {

        putChar(
            x,
            footerY,
            L' ',
            whiteOnBlue
        );
    }

    std::wstring footer =
        L"↑ ↓ SCROLL   PgUp/PgDn PAGE   "
        L"Home/End TOP/BOTTOM   Esc RETURN";

    if (static_cast<int>(footer.size()) >
        screenWidth) {

        footer.resize(
            static_cast<size_t>(
                screenWidth
            )
        );
    }

    putText(
        1,
        footerY,
        footer,
        whiteOnBlue
    );

    // -----------------------------------------------------------------
    // Bottom status bar
    // -----------------------------------------------------------------

    const int statusY =
        screenHeight - 1;

    std::wstring status =
        L"HELP  |  "
        L"Line " +
        std::to_wstring(
            std::min(
                helpScroll + 1,
                static_cast<int>(content.size())
            )
        ) +
        L"/" +
        std::to_wstring(
            static_cast<int>(content.size())
        ) +
        L"  |  CTRL+H / ESC = EXIT";

    for (int x = 0;
         x < screenWidth;
         ++x) {

        putChar(
            x,
            statusY,
            L' ',
            panelBright
        );
    }

    if (static_cast<int>(status.size()) >
        screenWidth) {

        status.resize(
            static_cast<size_t>(
                screenWidth
            )
        );
    }

    putText(
        0,
        statusY,
        status,
        panelBright
    );
}

// -----------------------------------------------------------------------------
// Editor rendering
// -----------------------------------------------------------------------------

static int lineNumberWidth() {
    return 7;
}

static void drawTitleBar() {
    WORD attr =
        BACKGROUND_BLUE |
        BACKGROUND_GREEN |
        BACKGROUND_RED |
        BACKGROUND_INTENSITY |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED;

    std::wstring title =
        L"MILLI - 1.0.0 by Githy912";

    putText(0, 0, title, attr);

    for (int x = static_cast<int>(title.size());
         x < screenWidth;
         ++x) {
        putChar(x, 0, L' ', attr);
    }
}

static std::wstring numberString(
    int number,
    int width
) {
    std::wstring s =
        std::to_wstring(number);

    if (static_cast<int>(s.size()) < width) {
        s.insert(
            s.begin(),
            width - static_cast<int>(s.size()),
            L' '
        );
    }

    return s;
}

static void drawDocument() {
    constexpr int editorTop = 1;
    const int editorBottom =
        screenHeight - 2;

    const int numberWidth =
        lineNumberWidth();

    WORD normalAttr =
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED;

    WORD lineNumberAttr =
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED |
        FOREGROUND_INTENSITY;

    WORD selectionAttr =
        BACKGROUND_BLUE |
        BACKGROUND_GREEN |
        BACKGROUND_RED |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED |
        FOREGROUND_INTENSITY;

    for (int row = editorTop;
         row <= editorBottom;
         ++row) {

        int documentY =
            scrollY +
            (row - editorTop);

        if (documentY >=
            static_cast<int>(lines.size())) {

            continue;
        }

        const std::wstring& line =
            lines[documentY];

        std::wstring number =
            numberString(
                documentY + 1,
                numberWidth - 1
            );

        putText(
            0,
            row,
            number + L" ",
            lineNumberAttr
        );

        for (int screenX = numberWidth;
             screenX < screenWidth;
             ++screenX) {

            int documentX =
                scrollX +
                (screenX - numberWidth);

            if (documentX >=
                static_cast<int>(line.size())) {

                putChar(
                    screenX,
                    row,
                    L' ',
                    normalAttr
                );

                continue;
            }

            wchar_t c =
                line[documentX];

            WORD attr =
                positionInSelection(
                    {documentX, documentY}
                )
                    ? selectionAttr
                    : normalAttr;

            putChar(
                screenX,
                row,
                c,
                attr
            );
        }
    }
}

static size_t documentCharacterCount() {
    size_t count = 0;

    for (const auto& line : lines)
        count += line.size();

    return count;
}

static void drawStatusBar() {
    WORD attr =
        BACKGROUND_INTENSITY |
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED;

    std::wstring file =
        filename.empty()
            ? L"Untitled"
            : filename;

    std::wstring status =
        L"Encoding: UTF-8 | Letters Typed: " +
        std::to_wstring(
            documentCharacterCount()
        ) +
        L" | File: " +
        file +
        L" | Ctrl + H for all help needed";

    if (modified)
        status += L" [Modified]";

    if (static_cast<int>(status.size()) >
        screenWidth) {

        status.resize(
            static_cast<size_t>(screenWidth)
        );
    }

    putText(
        0,
        screenHeight - 1,
        status,
        attr
    );

    for (int x =
             static_cast<int>(status.size());
         x < screenWidth;
         ++x) {

        putChar(
            x,
            screenHeight - 1,
            L' ',
            attr
        );
    }
}

static void render() {
    if (helpVisible) {
        drawHelp();
        present();
        return;
    }

    WORD normal =
        FOREGROUND_BLUE |
        FOREGROUND_GREEN |
        FOREGROUND_RED;

    fillBuffer(L' ', normal);

    drawTitleBar();
    drawDocument();
    drawStatusBar();

    ensureCursorVisible();

    present();

    COORD consoleCursor{
        static_cast<SHORT>(
            lineNumberWidth() +
            cursorX -
            scrollX
        ),
        static_cast<SHORT>(
            1 +
            cursorY -
            scrollY
        )
    };

    if (
        consoleCursor.X >= 0 &&
        consoleCursor.X < screenWidth &&
        consoleCursor.Y >= 1 &&
        consoleCursor.Y < screenHeight - 1
    ) {
        SetConsoleCursorPosition(
            hOutput,
            consoleCursor
        );
    }
}

// -----------------------------------------------------------------------------
// Keyboard
// -----------------------------------------------------------------------------

static bool ctrlDown(DWORD controlState) {
    return
        (controlState &
         (LEFT_CTRL_PRESSED |
          RIGHT_CTRL_PRESSED)) != 0;
}

static bool altDown(DWORD controlState) {
    return
        (controlState &
         (LEFT_ALT_PRESSED |
          RIGHT_ALT_PRESSED)) != 0;
}

static bool shiftDown(DWORD controlState) {
    return
        (controlState &
         SHIFT_PRESSED) != 0;
}

static void handleKey(
    const KEY_EVENT_RECORD& key
) {
    if (!key.bKeyDown)
        return;

    DWORD state =
        key.dwControlKeyState;

    bool ctrl = ctrlDown(state);
    bool alt = altDown(state);
    bool shift = shiftDown(state);

    WORD vk = key.wVirtualKeyCode;

    // ---------------------------------------------------------
    // Help
    // ---------------------------------------------------------

    if (ctrl && vk == 'H') {
		helpVisible = !helpVisible;

		if (helpVisible)
			helpScroll = 0;

        if (helpVisible) {
            CONSOLE_CURSOR_INFO info{};
            info.dwSize = 1;
            info.bVisible = FALSE;
            SetConsoleCursorInfo(hOutput, &info);
        } else {
            CONSOLE_CURSOR_INFO info{};
            info.dwSize = 1;
            info.bVisible = TRUE;
            SetConsoleCursorInfo(hOutput, &info);
        }

        return;
    }

    if (helpVisible) {
    if (vk == VK_ESCAPE) {
        helpVisible = false;
        helpScroll = 0;

        CONSOLE_CURSOR_INFO info{};
        info.dwSize = 1;
        info.bVisible = TRUE;
        SetConsoleCursorInfo(hOutput, &info);

        return;
    }

    if (ctrl && vk == 'H') {
        helpVisible = false;
        helpScroll = 0;

        CONSOLE_CURSOR_INFO info{};
        info.dwSize = 1;
        info.bVisible = TRUE;
        SetConsoleCursorInfo(hOutput, &info);

        return;
    }

    // Scroll up
    if (vk == VK_UP) {
        helpScroll =
            std::max(
                0,
                helpScroll - 1
            );

        return;
    }

    // Scroll down
    if (vk == VK_DOWN) {
        ++helpScroll;
        return;
    }

    // Page up
    if (vk == VK_PRIOR) {
        helpScroll =
            std::max(
                0,
                helpScroll -
                std::max(1, screenHeight - 3)
            );

        return;
    }

    // Page down
    if (vk == VK_NEXT) {
        helpScroll +=
            std::max(1, screenHeight - 3);

        return;
		}

		// Top
		if (vk == VK_HOME) {
			helpScroll = 0;
			return;
		}

		// Bottom
		if (vk == VK_END) {
			// drawHelp() clamps this to the real maximum.
			helpScroll = 1000000;
			return;
		}

		return;
	}

    // ---------------------------------------------------------
    // Quit
    // ---------------------------------------------------------

    if (ctrl && vk == 'Q') {
        running = false;
        return;
    }

    // ---------------------------------------------------------
    // Save
    // ---------------------------------------------------------

    if (ctrl && vk == 'S') {
        saveCurrent();
        return;
    }

    // ---------------------------------------------------------
    // Undo / redo
    // ---------------------------------------------------------

    if (ctrl && vk == 'Z') {
        undo();
        return;
    }

    if (ctrl && vk == 'Y') {
        redo();
        return;
    }

    // ---------------------------------------------------------
    // Select all
    // ---------------------------------------------------------

    if (ctrl && vk == 'A') {
        selectAll();
        return;
    }

    // ---------------------------------------------------------
    // Copy selection
    // ---------------------------------------------------------

    if (ctrl && alt && shift && vk == 'C') {
        copyEntireLine();
        return;
    }

    if (ctrl && alt && vk == 'C') {
        copySelection();
        return;
    }

    // ---------------------------------------------------------
    // Paste
    // ---------------------------------------------------------

    if (ctrl && vk == 'V') {
        paste();
        return;
    }

    // ---------------------------------------------------------
    // Ctrl + Backspace
    // ---------------------------------------------------------

    if (ctrl && vk == VK_BACK) {
        ctrlBackspace();
        return;
    }

    // ---------------------------------------------------------
    // Ctrl + Delete
    // ---------------------------------------------------------

    if (ctrl && vk == VK_DELETE) {
        ctrlDelete();
        return;
    }

    // ---------------------------------------------------------
    // Page navigation
    // ---------------------------------------------------------

    if (vk == VK_PRIOR) {
        pageUp();
        return;
    }

    if (vk == VK_NEXT) {
        pageDown();
        return;
    }

    // ---------------------------------------------------------
    // Delete
    // ---------------------------------------------------------

    if (vk == VK_DELETE) {
        deleteCharacter();
        return;
    }

    // ---------------------------------------------------------
    // Backspace
    // ---------------------------------------------------------

    if (vk == VK_BACK) {
        backspace();
        return;
    }

    // ---------------------------------------------------------
    // Enter
    // ---------------------------------------------------------

    if (vk == VK_RETURN) {
        insertNewLine();
        return;
    }

    // ---------------------------------------------------------
    // Home / End
    // ---------------------------------------------------------

    if (vk == VK_HOME) {
        cursorX = 0;
        clearSelection();
        return;
    }

    if (vk == VK_END) {
        cursorX =
            static_cast<int>(
                lines[cursorY].size()
            );

        clearSelection();
        return;
    }

    // ---------------------------------------------------------
    // Arrows
    // ---------------------------------------------------------

    if (vk == VK_LEFT) {
        if (cursorX > 0) {
            --cursorX;
        } else if (cursorY > 0) {
            --cursorY;
            cursorX =
                static_cast<int>(
                    lines[cursorY].size()
                );
        }

        clearSelection();
        return;
    }

    if (vk == VK_RIGHT) {
        if (cursorX <
            static_cast<int>(
                lines[cursorY].size()
            )) {

            ++cursorX;

        } else if (
            cursorY + 1 <
            static_cast<int>(lines.size())
        ) {

            ++cursorY;
            cursorX = 0;
        }

        clearSelection();
        return;
    }

    if (vk == VK_UP) {
        if (cursorY > 0)
            --cursorY;

        cursorX =
            std::min(
                cursorX,
                static_cast<int>(
                    lines[cursorY].size()
                )
            );

        clearSelection();
        return;
    }

    if (vk == VK_DOWN) {
        if (cursorY + 1 <
            static_cast<int>(lines.size())) {

            ++cursorY;
        }

        cursorX =
            std::min(
                cursorX,
                static_cast<int>(
                    lines[cursorY].size()
                )
            );

        clearSelection();
        return;
    }

    // ---------------------------------------------------------
    // Printable character
    // ---------------------------------------------------------

    wchar_t c = key.uChar.UnicodeChar;

    if (!ctrl && !alt && c >= 32) {
        insertCharacter(c);
        return;
    }

    (void)shift;
}

// -----------------------------------------------------------------------------
// Main editor loop
// -----------------------------------------------------------------------------

static void runEditor() {
    hInput = GetStdHandle(STD_INPUT_HANDLE);
    hOutput = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hInput == INVALID_HANDLE_VALUE ||
        hOutput == INVALID_HANDLE_VALUE) {

        return;
    }

    saveTerminalSnapshot();
    clearTerminal();

    enableConsole();
    updateScreenSize();

    clearTerminal();
    render();

    INPUT_RECORD record{};
    DWORD eventsRead = 0;

    while (running) {
        if (!ReadConsoleInputW(
                hInput,
                &record,
                1,
                &eventsRead
            )) {
            break;
        }

        switch (record.EventType) {

        case KEY_EVENT:
            handleKey(record.Event.KeyEvent);
            break;

        case MOUSE_EVENT:
            handleMouse(record.Event.MouseEvent);
            break;

        case WINDOW_BUFFER_SIZE_EVENT:
            updateScreenSize();
            break;

        default:
            break;
        }

        ensureCursorVisible();
        render();
    }

    disableConsole();

    clearTerminal();
    restoreTerminalSnapshot();
}

// -----------------------------------------------------------------------------
// Entry
// -----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    filename.clear();

    if (argc >= 2) {
        filename = utf8ToWide(argv[1]);

        if (!loadFile(filename)) {
            filename = utf8ToWide(argv[1]);
            lines.clear();
            lines.push_back(L"");
        }
    }

    runEditor();

    return 0;
}