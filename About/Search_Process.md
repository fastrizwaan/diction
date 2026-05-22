# Search Process Overview

The search process in Diction is an intricate pipeline designed to transform a user's raw keystrokes into a beautifully rendered HTML document without blocking the UI thread. 

```text
[User Types "apple"] -> [Debounce Timer (350ms)]
                             |
                      (Timer Fires)
                             v
                 [g_cancellable_cancel()] (Cancels previous tasks)
                             |
                             v
             [g_task_run_in_thread (Sidebar Search)]
                             |
           +-----------------+-----------------+
           v                                   v
[Scan Dict 1 FlatIndex]             [Scan Dict 2 FlatIndex]  ...
           |                                   |
           +---------- [Matches Found] --------+
                             |
                             v
               [g_idle_add (Batch to UI)]
                             |
                             v
            [User clicks "apple" in Sidebar]
                             |
           [g_task_run_in_thread (Webview Search)]
                             |
                             v
                  [dict_get_definition()]
                             |
                 [render_entry_def_to_html()]
                             |
                             v
                 [webkit_web_view_load_html()]
```


## 1. Input Debouncing
As the user types in the `GtkEntry` search bar, the UI triggers a `changed` signal. To prevent the engine from executing 10 simultaneous searches if a user types a 10-letter word quickly, Diction enforces a **350ms debounce timer**. The engine only initiates the search sequence if the user pauses typing for a third of a second.

## 2. Dispatching the GTask
Once debounced, `execute_search_now` is called. The engine destroys any currently running background search tasks via `g_cancellable_cancel` and drops references. 
A new `SidebarSearchState` is allocated, and `g_task_run_in_thread` is called to push the heavy lifting to GLib's background thread pool.

## 3. Sidebar Seeding
The background thread (`sidebar_search_task_func`) begins by executing the fast prefix lookup. As it finds matches in the `FlatIndex`, it categorizes them into buckets (Exact, Prefix, Substring). 
Instead of waiting to process all 100 dictionaries, the thread batches results into 50ms chunks and fires them back to the GTK Main Thread using `g_idle_add`. The main thread appends these `GtkListView` rows instantly, making the application feel incredibly responsive.

## 4. Definition Extraction
When the user clicks a specific word in the sidebar (or hits `Enter`), the UI requests the definition body.
Another `GTask` is spawned (`search_webview_task_thread`). This thread retrieves the exact byte offset from the `FlatIndex` and requests the raw definition markup from the `DictMmap` loader.

## 5. CSS and Theme Injection
Dictionaries contain raw markup, but they usually lack styling. Diction generates a unified CSS payload dynamically based on the user's current settings.
- It detects Light/Dark mode via `AdwStyleManager`.
- It loads user preferences for Font Family, Font Size, and Color Themes (e.g., Sepia, Nord, Dracula).
- It injects a global `<style>` block at the `head` of the output HTML.

## 6. WebKitGTK Rendering
The fully assembled HTML string is finally passed to WebKitGTK via `webkit_web_view_load_html`. 
WebKit parses the DOM, applies the injected CSS, and renders the dictionary definition natively. Any requests for local resources (like images or audio) inside the WebKit DOM trigger Diction's custom `dict://` URI scheme interceptor, which streams the assets directly from the `.mdd` or resource folders into the browser.
