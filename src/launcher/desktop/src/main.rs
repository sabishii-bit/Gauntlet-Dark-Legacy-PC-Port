//! The launcher's window. Only its `platform` module may call the OS directly.
#![deny(unsafe_code)]
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|_app, _args, _cwd| {}))
        .plugin(tauri_plugin_dialog::init())
        // Links and folders open from Rust; the page has no opener of its own.
        .plugin(
            tauri_plugin_opener::Builder::new()
                .open_js_links_on_click(false)
                .build(),
        )
        .run(tauri::generate_context!())
        .expect("the launcher's window could not start");
}
