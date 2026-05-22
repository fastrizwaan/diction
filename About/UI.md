# UI & Rendering Pipeline

Diction provides a modern, adaptive user interface built on the GNOME technology stack. By combining native GTK4 widgets with a WebKit backend, the application delivers a seamless blend of native desktop integration and rich HTML rendering.

## GTK4 and Libadwaita
The primary user interface is constructed using GTK4 and Libadwaita. This ensures Diction perfectly adheres to modern Linux desktop HIG (Human Interface Guidelines).
- **AdwWindow**: The main application window features rounded corners, native window shadows, and dynamic resizing behavior.
- **AdwHeaderBar**: A modern header bar housing the search entry, dictionary scope toggles, and global application menus.
- **GtkListView / GtkStringList**: Used for the primary search results sidebar. Unlike older GTK3 tree views, `GtkListView` is highly optimized for performance. It recycles a small pool of list item widgets as the user scrolls, allowing it to display millions of search results without consuming massive amounts of RAM.

## The WebKitGTK Definition View
While GTK handles the native UI, the actual dictionary definitions (which are almost exclusively authored in HTML or custom XML/DSL markups) are rendered using an embedded `WebKitWebView`.

When a user selects a word, Diction synthesizes a complete HTML5 document in memory and loads it into the `WebKitWebView`.

### Dynamic Theming
Diction actively syncs the WebKit rendering with the surrounding GTK theme.
- When Libadwaita switches to dark mode, Diction intercepts the signal and re-compiles the CSS payload with inverted color tokens, forcing the WebKit view into dark mode.
- Users can select specialized color themes (e.g., Solarized, Nord) from the settings dialog. These are dynamically injected into the `<style>` block of the WebKit DOM, overriding the dictionary's default styles to ensure legibility and visual consistency.

## The `dict://` URI Scheme Handler
Dictionary archives like MDX often bundle thousands of embedded images or audio pronunciation files inside a companion `.mdd` archive. 

Extracting these files to disk would be slow and waste massive amounts of storage. Instead, Diction registers a custom URI scheme interceptor (`dict://`) with WebKitGTK.

When the WebKit renderer encounters an `<img>` tag like `<img src="dict://picture.png">`, the interceptor catches the network request. The C engine searches the loaded `.mdd` or resource folders in memory, extracts the binary image payload, and streams it directly back to WebKit via a `GInputStream`. This completely eliminates disk I/O bottlenecks and keeps the user's filesystem clean.
