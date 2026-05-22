# System Integration & Global Workflows

Diction is designed to seamlessly integrate into the user's desktop environment. It extends beyond a simple window by providing global clipboard monitoring, system tray access, and global keyboard shortcuts.

## 1. Clipboard & Selection Monitoring (Scan Popup)

Diction can act as a global "Scan Dictionary", instantly translating or defining text as the user copies or highlights it in other applications (like web browsers or PDF readers). This is managed by `scan-popup.c`.

### Dual-Clipboard Architecture
Linux environments utilize two distinct clipboard systems:
- **`CLIPBOARD`**: Triggered by explicit copy commands (e.g., `Ctrl+C`).
- **`PRIMARY`**: Triggered automatically when text is simply highlighted/selected with the mouse.

Diction initializes listeners for both systems via `GdkClipboard`. Because the `PRIMARY` selection does not always emit reliable `"changed"` signals across all display servers (specifically under some Wayland compositors), Diction implements a lightweight asynchronous polling fallback (`g_timeout_add`) to query the clipboard state every few hundred milliseconds.

### Debouncing and Modifiers
To prevent spamming the user with definitions every time they click a word, the scanning system includes:
- **Modifier Checks**: Users can configure the scanner to only trigger if a specific modifier key (e.g., `Ctrl`, `Alt`, `Meta/Super`) is held down during the selection.
- **State Debouncing**: The system stores the `last_primary_text` and `last_clipboard_text`. It only triggers a definition lookup if the text has actually changed, preventing redundant search execution loops.

### The Scan Popup Rendering
When valid text is detected, it is immediately trimmed of leading/trailing whitespace and dispatched to the search engine.
Instead of forcing the main application window to the foreground in full size, Diction reuses the main window but resizes it to a smaller popup size (e.g., 400x500), bringing it near the cursor to render the definition instantly.

```text
[User selects text]
       |
[scan-popup.c (PRIMARY/CLIPBOARD Poll)]
       |
  (Text changed?) -> Yes -> Trim Whitespace -> scan_word_callback()
       |
[main.c]
  -> Resize main_window to 400x500
  -> gtk_window_present()
  -> gtk_editable_set_text(search_entry)
       |
[Search Pipeline executes & renders HTML in WebKit]
```

## 2. System Tray / Status Icon

To allow Diction to run persistently in the background for clipboard monitoring, it provides a native system tray icon (`tray-icon.c`).

### StatusNotifierItem (SNI) via DBus
Modern Linux desktops have deprecated the legacy X11 `GtkStatusIcon` (system tray) in favor of the `StatusNotifierItem` (SNI) DBus specification (commonly known as AppIndicator).
- Diction manually registers an SNI interface (`org.kde.StatusNotifierItem`) directly over the session DBus.
- It also registers a complete DBusMenu (`com.canonical.dbusmenu`) layout, exposing options like "Show/Hide Diction", "Toggle Scan Popup", and "Quit".
- This ensures native tray integration across GNOME (with AppIndicator extensions), KDE Plasma, XFCE, and other desktop environments without requiring heavy external dependencies.

## 3. Global Keyboard Shortcuts

Diction leverages modern Wayland-compatible APIs to register global system-wide keyboard shortcuts (`global-shortcut.c`).

### XDG Desktop Portal Integration
Historically, global shortcuts on Linux relied on insecure X11 keyloggers. Wayland strictly forbids applications from globally listening to keystrokes. 
To bypass this limitation securely, Diction uses the `org.freedesktop.portal.GlobalShortcuts` DBus API (XDG Desktop Portal).
1. Diction requests the creation of a global shortcut session.
2. It requests to bind a specific hotkey (e.g., `Super+Alt+L`).
3. The desktop environment handles the keystroke listening globally. When the user presses the combination, the compositor sends an `Activated` DBus signal back to Diction.

When triggered, Diction can be configured to instantly parse the current clipboard or pop the main window to the foreground, allowing dictionary access from anywhere in the OS without relying on the mouse.
