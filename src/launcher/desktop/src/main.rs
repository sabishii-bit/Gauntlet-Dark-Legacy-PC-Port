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
        .run(context())
        .expect("the launcher's window could not start");
}

/// What Tauri builds into the program: its configuration, front end, icons and package
/// information.
fn context() -> tauri::Context<tauri::Wry> {
    tauri::generate_context!()
}

#[cfg(test)]
mod tests {
    #[test]
    fn the_window_reports_the_version_its_build_gave_it() {
        // Tauri wrote the same version into the program's Windows version resource.
        let version = super::context().package_info().version.to_string();
        assert_eq!(version, env!("GDL_VERSION"));
        if option_env!("TAURI_CONFIG").is_none() {
            let file = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../../VERSION"));
            assert_eq!(version, file.trim());
        }
    }
}
