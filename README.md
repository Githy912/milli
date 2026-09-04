### THE DAY HAS ARRIVED!

# 📂 Milli

**For years, developers thought that someday, someone would create a text editor after Pico, Nano, and Micro named Milli. Someday is today.**

`milli` is a modern, lightweight terminal text editor built to bridge the gap between absolute simplicity and powerful usability. It honors the metric naming tradition by scaling down complexity while scaling up developer experience.

---

## 📈 The Metric Lineage

| Editor | Scale | Era | Core Philosophy |
| :--- | :--- | :--- | :--- |
| **Pico** | $10^{-12}$ | 1992 | Bundled, basic email text editing. |
| **Nano** | $10^{-9}$ | 1999 | The open-source, omnipresent terminal default. |
| **Micro** | $10^{-6}$ | 2016 | Modern terminal editing with mouse & modern shortcuts. |
| **Milli** | $10^{-3}$ | **Today** | **Native Windows terminal power editing.** |

---

## ✨ Features

* **Native Windows API** - Built entirely on Windows console buffers for smooth, standalone terminal performance.
* **Mouse Text Selection** - Native support for dragging to select text ranges and clicking to place your cursor.
* **Deep Edit History** - Built-in multi-level undo and redo tracking up to 500 steps of code modifications.
* **UTF-8 Support** - Seamlessly reads and writes standard UTF-8 text with local wide-character conversion.
* **No-Config Interactive Help** - Full offline interactive controls and documentation dashboard built right inside the terminal screen.

---

## 🚀 Quick Start

### Installation

`milli` is written in standard C++ and interfaces directly with the Win32 API. You can compile it using `g++` (via MinGW) or MSVC.

```bash
# Get it from WinGet:

winget install Githy912.Milli
```
Or:

Download the `milli.exe` from the repository and add it to PATH, or if you are a developer, download the `main.cpp` and `main.hpp` and compile using MinGW:

```bash
g++ -std=c++20 -O2 -Wall -Wextra main.cpp -o milli.exe
```

### Usage

```bash
milli [filename]
```

---

## 🎹 Keybindings

### File & System
* `Ctrl + S` - Save current file (or opens Save As menu)
* `Ctrl + Q` - Exit editor cleanly and restore standard terminal states
* `Ctrl + H` - Toggle full interactive screen help / Controls directory

### Editing & Clipboard
* `Ctrl + Z` - Undo last text modification
* `Ctrl + Y` - Redo last undone action
* `Ctrl + A` - Select entire file buffer
* `Ctrl + Alt + C` - Copy selected text range to the Windows clipboard
* `Ctrl + Alt + Shift + C` - Copy current line to the Windows clipboard
* `Ctrl + V` - Paste clipboard string contents at cursor location

### Navigation & Word Deletion
* `Ctrl + Backspace` - Delete previous full word block
* `Ctrl + Delete` - Delete next full word block
* `Home` / `End` - Instantly snap cursor to line bounds
* `Page Up` / `Page Down` - Page navigation leaps across the document

---

## 🤝 Contributing

The metric scale doesn't stop here. Contributions, bug reports, and feature requests are welcome! Feel free to open an issue or submit a pull request to help make `milli` even better.
