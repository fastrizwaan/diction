# Diction

**The ultimate fast, multi-dictionary offline reader for Linux.**

## 📖 The Gist
**Diction** is a lightning-fast desktop application that lets you look up words without needing an internet connection. 

Instead of relying on websites or simple built-in dictionaries, Diction allows you to download massive, high-quality dictionary files from the internet and load **hundreds of them at the same time** (supporting up to ~1,000 dictionaries concurrently, depending on your system's open-file `ulimit`). Whether you are learning a new language, reading medical textbooks, or just need a good thesaurus, Diction searches through all of your loaded dictionaries instantly and displays the results in a beautiful, modern interface.

## ✨ Why use Diction?

* 🚀 **Blazing Fast & Multi-Dictionary:** Search across dozens (or even hundreds!) of offline dictionaries simultaneously (up to the ~1000 open-file system limit). The results appear instantly as you type.
* 🔍 **Global Scan Popup:** You don't even need to open the app! Just highlight a word in your web browser, PDF reader, or terminal, hit your custom keyboard shortcut (e.g., `Super+Alt+L`), and a small popup will appear right at your mouse cursor with the definition.
* 📚 **Supports Almost Everything:** You can easily find and download dictionary files online. Diction natively supports almost all popular formats without needing manual conversion:
  * **MDX / MDD** (MDict dictionaries, often containing images and audio)
  * **DSL / DSL.DZ** (ABBYY Lingvo dictionaries)
  * **StarDict** (`.ifo` / `.dict.dz`)
  * **Slob** (Used for offline Wikipedia)
  * **BGL** (Babylon Glossaries)
  * **XDXF** & **Dictd**
* 🎨 **Beautiful & Modern:** Built specifically for Linux using GTK4 and Libadwaita. It perfectly matches your system, supports Dark Mode, and lets you customize colors and fonts so reading definitions is easy on the eyes.
* 🔊 **Audio Pronunciations:** Click on audio icons to hear how a word is pronounced (supports audio embedded inside MDX and DSL dictionaries).

## 🛠️ How to Install & Build

Diction uses the `meson` and `ninja` build system.

**1. Install Dependencies**

You will need a C compiler (`gcc`), `meson`, `ninja-build`, and development headers for the required libraries.

**For Debian / Ubuntu:**
```bash
sudo apt update
sudo apt install build-essential meson ninja-build libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev libglib2.0-dev libjson-glib-dev zlib1g-dev libarchive-dev libzstd-dev liblzma-dev libbz2-dev
```


**For Fedora / RHEL:**
```bash
sudo dnf install gcc meson ninja-build gtk4-devel libadwaita-devel webkitgtk6.0-devel glib2-devel json-glib-devel zlib-devel libarchive-devel libzstd-devel xz-devel bzip2-devel
```
*(Note: On Fedora Silverblue or Kinoite, use `rpm-ostree install` instead of `dnf install`, or compile inside a `toolbox`/`distrobox` container.)*

**2. Compile the Application:**
```bash
# Setup the build directory
meson setup build

# Compile the application
ninja -C build

# Install the application to your system
sudo ninja -C build install
```
*Note: To uninstall later, you can run `sudo ninja -C build uninstall`.*

## 🚀 How to Use

1. Open **Diction** from your app launcher.
2. Go to **Preferences** and select the folder on your computer where you downloaded your dictionary files.
3. Diction will automatically scan the folder, index your dictionaries, and make them ready for instant searching.
4. Start typing in the search bar!

---

### 🤓 For Developers
Are you curious about how Diction can search millions of words instantly, or how it renders custom dictionary formats? Check out the [Technical Documentation Index](About/README.md) inside the `About/` folder for a deep dive into the engine's architecture and source code.
