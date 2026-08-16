// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use eframe::egui::{self, Color32, RichText};
use serde::{Deserialize, Serialize};

const APP_NAME: &str = "LittleBigPlanet™ 3 Launcher";
const CORE_EXECUTABLE: &str = "shadps4-core";
const LAUNCHER_UI_ARGUMENT: &str = "--launcher-ui";
const SUPPORTED_PATCH_APP_VERSION: &str = "01.26";
const CORE_RUNTIME_DEPENDENCIES: &[&str] = &[
    "libvulkan.dylib",
    "libvulkan_kosmickrisp.dylib",
    "kosmickrisp_mesa_icd.json",
];
const PARTYCHAT_ADDRESS: &str = "127.0.0.1:18063";
const RESOLUTIONS: &[&str] = &[
    "1280x720",
    "1600x900",
    "1920x1080",
    "2560x1440",
    "3840x2160",
];

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
struct LauncherConfig {
    external_eboot: String,
    prefer_bundled_game: bool,
    addon_root: String,
    resolution: String,
    lbp3_online: bool,
    patch_prize_bubbles: bool,
    disable_sprite_lights: bool,
    disable_tone_map: bool,
    fullscreen: bool,
    show_fps: bool,
}

impl Default for LauncherConfig {
    fn default() -> Self {
        Self {
            external_eboot: String::new(),
            prefer_bundled_game: true,
            addon_root: String::new(),
            resolution: "1920x1080".to_owned(),
            lbp3_online: false,
            patch_prize_bubbles: true,
            disable_sprite_lights: false,
            disable_tone_map: false,
            fullscreen: false,
            show_fps: true,
        }
    }
}

impl LauncherConfig {
    fn has_selected_patches(&self) -> bool {
        self.patch_prize_bubbles || self.disable_sprite_lights || self.disable_tone_map
    }

    fn with_patches_disabled(&self) -> Self {
        let mut config = self.clone();
        config.patch_prize_bubbles = false;
        config.disable_sprite_lights = false;
        config.disable_tone_map = false;
        config
    }
}

#[derive(Debug, Clone, Default)]
enum PartyChatState {
    #[default]
    Checking,
    Offline,
    Running,
}

impl PartyChatState {
    fn is_running(&self) -> bool {
        matches!(self, Self::Running)
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
struct LaunchWarning {
    patch_version_mismatch: bool,
    detected_app_version: Option<String>,
    partychat_offline: bool,
}

impl LaunchWarning {
    fn is_empty(&self) -> bool {
        !self.patch_version_mismatch && !self.partychat_offline
    }
}

fn derive_launch_warning(
    config: &LauncherConfig,
    detected_app_version: Option<String>,
    partychat_running: bool,
) -> LaunchWarning {
    LaunchWarning {
        patch_version_mismatch: config.has_selected_patches()
            && detected_app_version.as_deref() != Some(SUPPORTED_PATCH_APP_VERSION),
        detected_app_version,
        partychat_offline: config.lbp3_online && !partychat_running,
    }
}

#[derive(Debug, Deserialize)]
struct PartyChatResponse {
    status: Option<String>,
    service: Option<String>,
}

struct PartyChatMonitor {
    state: Arc<Mutex<PartyChatState>>,
    stop: Arc<AtomicBool>,
}

impl PartyChatMonitor {
    fn new(context: egui::Context) -> Self {
        let state = Arc::new(Mutex::new(PartyChatState::Checking));
        let stop = Arc::new(AtomicBool::new(false));
        let thread_state = Arc::clone(&state);
        let thread_stop = Arc::clone(&stop);
        thread::spawn(move || {
            while !thread_stop.load(Ordering::Relaxed) {
                let new_state = probe_partychat();
                if let Ok(mut current) = thread_state.lock() {
                    *current = new_state;
                }
                context.request_repaint();
                for _ in 0..15 {
                    if thread_stop.load(Ordering::Relaxed) {
                        return;
                    }
                    thread::sleep(Duration::from_millis(100));
                }
            }
        });
        Self { state, stop }
    }

    fn state(&self) -> PartyChatState {
        self.state
            .lock()
            .map(|state| state.clone())
            .unwrap_or_default()
    }

    fn set_state(&self, state: PartyChatState) {
        if let Ok(mut current) = self.state.lock() {
            *current = state;
        }
    }
}

impl Drop for PartyChatMonitor {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

struct LauncherApp {
    config: LauncherConfig,
    config_path: PathBuf,
    bundled_eboot: Option<PathBuf>,
    bundled_addons: Option<PathBuf>,
    partychat: PartyChatMonitor,
    child: Option<Child>,
    error: Option<String>,
    pending_warning: Option<LaunchWarning>,
}

impl LauncherApp {
    fn new(context: &eframe::CreationContext<'_>, initial_eboot: Option<PathBuf>) -> Self {
        configure_style(&context.egui_ctx);
        egui_extras::install_image_loaders(&context.egui_ctx);
        let config_path = launcher_config_path();
        let mut config = load_config(&config_path);
        let contents_dir = bundle_contents_dir();
        let bundled_eboot = contents_dir.as_deref().and_then(find_bundled_eboot);
        let bundled_addons = contents_dir.as_deref().and_then(find_bundled_addons);

        if bundled_eboot.is_none() {
            config.prefer_bundled_game = false;
        }
        if config.external_eboot.is_empty()
            && let Some(candidate) = default_external_eboot()
        {
            config.external_eboot = candidate.to_string_lossy().into_owned();
        }
        if config.addon_root.is_empty()
            && let Some(addons) = &bundled_addons
        {
            config.addon_root = addons.to_string_lossy().into_owned();
        }
        if !RESOLUTIONS.contains(&config.resolution.as_str()) {
            config.resolution = "1920x1080".to_owned();
        }
        if let Some(eboot) = initial_eboot {
            config.external_eboot = eboot.to_string_lossy().into_owned();
            config.prefer_bundled_game = false;
        }

        Self {
            config,
            config_path,
            bundled_eboot,
            bundled_addons,
            partychat: PartyChatMonitor::new(context.egui_ctx.clone()),
            child: None,
            error: None,
            pending_warning: None,
        }
    }

    fn selected_eboot(&self) -> Option<PathBuf> {
        if self.config.prefer_bundled_game {
            self.bundled_eboot.clone()
        } else if self.config.external_eboot.is_empty() {
            None
        } else {
            Some(PathBuf::from(&self.config.external_eboot))
        }
    }

    fn addon_root(&self) -> Option<PathBuf> {
        if self.config.addon_root.is_empty() {
            None
        } else {
            Some(PathBuf::from(&self.config.addon_root))
        }
    }

    fn poll_child(&mut self) {
        let Some(child) = self.child.as_mut() else {
            return;
        };
        match child.try_wait() {
            Ok(Some(status)) => {
                let _ = status;
                self.child = None;
            }
            Ok(None) => {}
            Err(error) => {
                self.error = Some(error.to_string());
                self.child = None;
            }
        }
    }

    fn launch_inputs(&self) -> Result<(PathBuf, Option<PathBuf>, PathBuf), String> {
        if self.child.is_some() {
            return Err("The game is already running".to_owned());
        }
        let eboot = self
            .selected_eboot()
            .ok_or_else(|| "Choose an existing eboot.bin".to_owned())?;
        if !is_eboot(&eboot) {
            return Err(format!("eboot.bin was not found: {}", eboot.display()));
        }
        let addon_root = self.addon_root();
        if let Some(path) = &addon_root
            && !path.is_dir()
        {
            return Err(format!("DLC folder was not found: {}", path.display()));
        }
        let core = core_executable_path();
        if !core.is_file() {
            return Err(format!("{CORE_EXECUTABLE} is missing from the .app"));
        }
        Ok((eboot, addon_root, core))
    }

    fn refresh_partychat(&self) -> PartyChatState {
        let state = probe_partychat();
        self.partychat.set_state(state.clone());
        state
    }

    fn request_launch(&mut self) -> bool {
        self.error = None;
        let (eboot, _, _) = match self.launch_inputs() {
            Ok(inputs) => inputs,
            Err(error) => {
                self.error = Some(error);
                return false;
            }
        };

        let partychat_running = !self.config.lbp3_online || self.refresh_partychat().is_running();
        let warning = derive_launch_warning(
            &self.config,
            read_game_param(&eboot, "APP_VER"),
            partychat_running,
        );
        if !warning.is_empty() {
            self.pending_warning = Some(warning);
            return false;
        }
        self.launch_now(false)
    }

    fn launch_now(&mut self, disable_patches: bool) -> bool {
        self.error = None;
        let (eboot, addon_root, core) = match self.launch_inputs() {
            Ok(inputs) => inputs,
            Err(error) => {
                self.error = Some(error);
                return false;
            }
        };
        if let Err(error) = save_config(&self.config_path, &self.config) {
            self.error = Some(format!("Could not save launcher settings: {error}"));
            return false;
        }

        let named_core = match prepare_named_core(&core, &eboot) {
            Ok(path) => path,
            Err(error) => {
                self.error = Some(format!("Could not prepare the game process: {error}"));
                return false;
            }
        };
        let effective_config = if disable_patches {
            self.config.with_patches_disabled()
        } else {
            self.config.clone()
        };
        let mut command = Command::new(&named_core);
        command.args(build_core_args(
            &effective_config,
            &eboot,
            addon_root.as_deref(),
        ));
        if let Some(parent) = named_core.parent() {
            command.current_dir(parent);
        }

        if let Ok(log) = open_core_log() {
            match log.try_clone() {
                Ok(stdout) => {
                    command.stdout(Stdio::from(stdout));
                    command.stderr(Stdio::from(log));
                }
                Err(_) => {
                    command.stdout(Stdio::null());
                    command.stderr(Stdio::null());
                }
            }
        }

        match command.spawn() {
            Ok(child) => {
                self.child = Some(child);
                true
            }
            Err(error) => {
                self.error = Some(format!("Could not start the emulator: {error}"));
                false
            }
        }
    }

    fn draw_game_paths(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        ui.label(RichText::new("eboot.bin").size(19.0).strong());
        ui.add_space(4.0);
        let bundled_game_active = self.config.prefer_bundled_game && self.bundled_eboot.is_some();
        let (field_changed, browse_clicked) = draw_path_field(
            ui,
            &mut self.config.external_eboot,
            bundled_game_active.then_some("Using bundled game"),
            "/path/to/CUSA00063/eboot.bin",
            "Choose an external eboot.bin",
        );
        changed |= field_changed;
        if browse_clicked
            && let Some(path) = rfd::FileDialog::new()
                .add_filter("PlayStation executable", &["bin"])
                .set_file_name("eboot.bin")
                .pick_file()
        {
            self.config.external_eboot = path.to_string_lossy().into_owned();
            self.config.prefer_bundled_game = false;
            changed = true;
        }
        if let Some(path) = &self.bundled_eboot {
            ui.horizontal(|ui| {
                if !bundled_game_active && ui.small_button("Use bundled game").clicked() {
                    self.config.prefer_bundled_game = true;
                    changed = true;
                }
                if bundled_game_active {
                    ui.label(RichText::new(path.display().to_string()).small().weak());
                }
            });
        }

        ui.add_space(15.0);
        ui.label(RichText::new("DLC Path").size(19.0).strong());
        ui.add_space(4.0);
        let bundled_addons_active = self.bundled_addons.as_ref().is_some_and(|path| {
            Path::new(&self.config.addon_root) == path && !self.config.addon_root.is_empty()
        });
        let (field_changed, browse_clicked) = draw_path_field(
            ui,
            &mut self.config.addon_root,
            bundled_addons_active.then_some("Using bundled DLC"),
            "Optional add-on root",
            "Choose an external DLC folder",
        );
        changed |= field_changed;
        if browse_clicked && let Some(path) = rfd::FileDialog::new().pick_folder() {
            self.config.addon_root = path.to_string_lossy().into_owned();
            changed = true;
        }
        ui.horizontal(|ui| {
            if let Some(path) = &self.bundled_addons
                && !bundled_addons_active
                && ui.small_button("Use bundled DLC").clicked()
            {
                self.config.addon_root = path.to_string_lossy().into_owned();
                changed = true;
            }
            if !self.config.addon_root.is_empty()
                && !bundled_addons_active
                && ui.small_button("No DLC").clicked()
            {
                self.config.addon_root.clear();
                changed = true;
            }
        });
        changed
    }

    fn patch_summary(&self) -> String {
        let labels = [
            (self.config.patch_prize_bubbles, "Prize bubbles"),
            (self.config.disable_sprite_lights, "Disable sprite lights"),
            (self.config.disable_tone_map, "Disable tone map"),
        ];
        let selected = labels
            .into_iter()
            .filter_map(|(enabled, label)| enabled.then_some(label))
            .collect::<Vec<_>>();
        match selected.as_slice() {
            [] => "No patches".to_owned(),
            [label] => (*label).to_owned(),
            labels => format!("{} patches enabled", labels.len()),
        }
    }

    fn draw_patch_and_video_options(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        let available_width = ui.available_width();
        let option_width = (available_width * 0.305).clamp(170.0, 230.0);
        let option_gap = (available_width * 0.03).clamp(14.0, 22.0);
        ui.horizontal_top(|ui| {
            ui.allocate_ui_with_layout(
                egui::vec2(option_width, 58.0),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    ui.label(RichText::new("Patches").size(17.0).strong());
                    ui.add_space(3.0);
                    let patch_summary = self.patch_summary();
                    egui::ComboBox::from_id_salt("lbp3-patches")
                        .selected_text(patch_summary)
                        .width(option_width)
                        .close_behavior(egui::PopupCloseBehavior::CloseOnClickOutside)
                        .show_ui(ui, |ui| {
                            ui.set_min_width(285.0);
                            changed |= ui
                                .checkbox(
                                    &mut self.config.patch_prize_bubbles,
                                    "Prize bubbles (recommended)",
                                )
                                .changed();
                            changed |= ui
                                .checkbox(
                                    &mut self.config.disable_sprite_lights,
                                    "Disable sprite lights",
                                )
                                .changed();
                            changed |= ui
                                .checkbox(
                                    &mut self.config.disable_tone_map,
                                    "Disable sprite-light tone map",
                                )
                                .changed();
                            ui.separator();
                            ui.label(
                                RichText::new(format!(
                                    "Prepared for LBP3 {SUPPORTED_PATCH_APP_VERSION}"
                                ))
                                .small()
                                .weak(),
                            );
                        });
                },
            );
            ui.add_space(option_gap);
            ui.allocate_ui_with_layout(
                egui::vec2(option_width, 58.0),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    ui.label(RichText::new("Resolution").size(17.0).strong());
                    ui.add_space(3.0);
                    egui::ComboBox::from_id_salt("resolution")
                        .selected_text(&self.config.resolution)
                        .width(option_width)
                        .show_ui(ui, |ui| {
                            for resolution in RESOLUTIONS {
                                changed |= ui
                                    .selectable_value(
                                        &mut self.config.resolution,
                                        (*resolution).to_owned(),
                                        *resolution,
                                    )
                                    .changed();
                            }
                        });
                },
            );
        });
        changed
    }

    fn draw_online_and_display_options(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        changed |= ui
            .checkbox(
                &mut self.config.lbp3_online,
                RichText::new("LBP Online").size(17.0),
            )
            .changed();
        ui.label(
            RichText::new("Enable LittleBigPlanet™ 3 online features")
                .small()
                .weak(),
        );
        ui.add_space(10.0);
        ui.horizontal(|ui| {
            changed |= ui
                .checkbox(&mut self.config.fullscreen, "Fullscreen")
                .changed();
            ui.add_space(18.0);
            changed |= ui.checkbox(&mut self.config.show_fps, "Show FPS").changed();
        });
        changed
    }

    fn draw_launch_footer(&mut self, ui: &mut egui::Ui) -> bool {
        if let Some(error) = &self.error {
            ui.colored_label(Color32::from_rgb(240, 105, 105), error);
            ui.add_space(6.0);
        }

        let eboot_valid = self.selected_eboot().is_some_and(|path| is_eboot(&path));
        let addon_valid = self.addon_root().is_none_or(|path| path.is_dir());
        let core_valid = core_executable_path().is_file();
        let can_launch = self.child.is_none() && eboot_valid && addon_valid && core_valid;
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            let response = ui.add_enabled(
                can_launch,
                egui::Button::new(RichText::new("Launch").size(18.0).strong())
                    .fill(Color32::from_rgb(28, 132, 245))
                    .corner_radius(10.0)
                    .min_size([180.0, 46.0].into()),
            );
            response.clicked() && self.request_launch()
        })
        .inner
    }

    fn draw_warning_modal(&mut self, context: &egui::Context) -> bool {
        let Some(warning) = self.pending_warning.clone() else {
            return false;
        };
        #[derive(Clone, Copy)]
        enum Action {
            Continue,
            Cancel,
        }
        let mut action = None;
        let modal = egui::Modal::new(egui::Id::new("launch-warning"))
            .backdrop_color(Color32::from_black_alpha(185))
            .frame(
                egui::Frame::popup(&context.style())
                    .fill(Color32::from_rgb(36, 38, 44))
                    .corner_radius(14.0)
                    .inner_margin(egui::Margin::symmetric(24, 20)),
            )
            .show(context, |ui| {
                ui.set_min_width(470.0);
                ui.heading(RichText::new("Before launching").size(21.0));
                ui.add_space(8.0);

                if warning.patch_version_mismatch {
                    let detected = warning
                        .detected_app_version
                        .as_deref()
                        .unwrap_or("not detected");
                    ui.label(
                        RichText::new("Patches do not match this game version").strong(),
                    );
                    ui.label(format!(
                        "Selected patches are prepared for APP_VER {SUPPORTED_PATCH_APP_VERSION}, but the selected game reports {detected}. If you continue, the patches will not be applied."
                    ));
                }

                if warning.patch_version_mismatch && warning.partychat_offline {
                    ui.add_space(10.0);
                    ui.separator();
                    ui.add_space(10.0);
                }

                if warning.partychat_offline {
                    ui.label(RichText::new("PartyChat is not running").strong());
                    if self.partychat.state().is_running() {
                        ui.colored_label(
                            Color32::from_rgb(95, 215, 135),
                            "PartyChat is now detected. Continue will connect to it.",
                        );
                    } else {
                        ui.label(
                            "LBP Online is enabled. You can start PartyChat while this warning is open, then press Continue. It will be checked again immediately before launch; if it is still unavailable, the game will fall back to offline mode.",
                        );
                    }
                }

                ui.add_space(18.0);
                ui.horizontal(|ui| {
                    if ui
                        .add_sized([110.0, 36.0], egui::Button::new("Cancel"))
                        .clicked()
                    {
                        action = Some(Action::Cancel);
                    }
                    if ui
                        .add_sized(
                            [130.0, 36.0],
                            egui::Button::new(RichText::new("Continue").strong())
                                .fill(Color32::from_rgb(28, 132, 245)),
                        )
                        .clicked()
                    {
                        action = Some(Action::Continue);
                    }
                });
            });
        if modal.should_close() && action.is_none() {
            action = Some(Action::Cancel);
        }

        match action {
            Some(Action::Cancel) => {
                self.pending_warning = None;
                false
            }
            Some(Action::Continue) => {
                self.pending_warning = None;
                if self.config.lbp3_online {
                    self.refresh_partychat();
                }
                let disable_patches = self.config.has_selected_patches()
                    && self
                        .selected_eboot()
                        .and_then(|eboot| read_game_param(&eboot, "APP_VER"))
                        .as_deref()
                        != Some(SUPPORTED_PATCH_APP_VERSION);
                self.launch_now(disable_patches)
            }
            None => false,
        }
    }
}

fn draw_path_field(
    ui: &mut egui::Ui,
    value: &mut String,
    read_only_label: Option<&str>,
    hint: &str,
    browse_tooltip: &str,
) -> (bool, bool) {
    let mut changed = false;
    let mut browse_clicked = false;
    egui::Frame::new()
        .fill(Color32::from_rgb(76, 77, 82))
        .corner_radius(10.0)
        .inner_margin(egui::Margin::symmetric(12, 2))
        .show(ui, |ui| {
            let row_width = ui.available_width();
            ui.allocate_ui_with_layout(
                egui::vec2(row_width, 36.0),
                egui::Layout::left_to_right(egui::Align::Center),
                |ui| {
                    draw_folder_icon(ui);
                    let text_width = (ui.available_width() - 48.0).max(80.0);
                    if let Some(label) = read_only_label {
                        ui.allocate_ui_with_layout(
                            egui::vec2(text_width, 34.0),
                            egui::Layout::left_to_right(egui::Align::Center),
                            |ui| {
                                ui.add(egui::Label::new(label).truncate());
                            },
                        );
                    } else {
                        changed |= ui
                            .add_sized(
                                [text_width, 34.0],
                                egui::TextEdit::singleline(value)
                                    .hint_text(hint)
                                    .frame(false)
                                    .margin(egui::Margin::ZERO)
                                    .vertical_align(egui::Align::Center),
                            )
                            .changed();
                    }
                    browse_clicked = ui
                        .add_sized(
                            [32.0, 32.0],
                            egui::Button::new(RichText::new("•••").size(13.0))
                                .fill(Color32::from_rgb(105, 106, 111))
                                .stroke(egui::Stroke::NONE)
                                .corner_radius(16.0),
                        )
                        .on_hover_text(browse_tooltip)
                        .clicked();
                },
            );
        });
    (changed, browse_clicked)
}

fn draw_folder_icon(ui: &mut egui::Ui) {
    ui.add(
        egui::Image::new(egui::include_image!("../assets/foldericon.svg"))
            .fit_to_exact_size(egui::vec2(21.0, 16.33)),
    );
}

impl eframe::App for LauncherApp {
    fn update(&mut self, context: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_child();
        context.request_repaint_after(Duration::from_millis(500));
        let launched = egui::CentralPanel::default()
            .frame(
                egui::Frame::new()
                    .fill(Color32::from_rgb(43, 43, 46))
                    .inner_margin(egui::Margin::symmetric(26, 22)),
            )
            .show(context, |ui| {
                let mut changed = false;
                changed |= self.draw_game_paths(ui);
                ui.add_space(15.0);
                changed |= self.draw_patch_and_video_options(ui);
                ui.add_space(15.0);
                changed |= self.draw_online_and_display_options(ui);
                let footer_space = (ui.available_height() - 78.0).max(12.0);
                ui.add_space(footer_space);
                ui.separator();
                ui.add_space(10.0);
                let launched = self.draw_launch_footer(ui);

                if changed && let Err(error) = save_config(&self.config_path, &self.config) {
                    self.error = Some(format!("Could not save launcher settings: {error}"));
                }
                launched
            })
            .inner;

        let launched = self.draw_warning_modal(context) || launched;
        if launched {
            // Closing the last viewport does not quit a macOS application. The core is an
            // independent process and the configuration was saved before it was spawned,
            // so terminate the launcher explicitly and leave only the game in the Dock.
            std::process::exit(0);
        }
    }
}

impl Drop for LauncherApp {
    fn drop(&mut self) {
        let _ = save_config(&self.config_path, &self.config);
    }
}

fn configure_style(context: &egui::Context) {
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::from_rgb(31, 31, 33);
    visuals.window_fill = Color32::from_rgb(43, 43, 46);
    visuals.window_stroke = egui::Stroke::NONE;
    visuals.extreme_bg_color = Color32::from_rgb(82, 83, 88);
    visuals.text_edit_bg_color = Some(Color32::from_rgb(82, 83, 88));
    visuals.faint_bg_color = Color32::from_rgb(51, 51, 54);
    visuals.selection.bg_fill = Color32::from_rgb(28, 132, 245);
    visuals.widgets.noninteractive.bg_stroke = egui::Stroke::NONE;
    visuals.widgets.inactive.bg_fill = Color32::from_rgb(82, 83, 88);
    visuals.widgets.inactive.weak_bg_fill = Color32::from_rgb(82, 83, 88);
    visuals.widgets.inactive.bg_stroke = egui::Stroke::NONE;
    visuals.widgets.hovered.bg_fill = Color32::from_rgb(94, 95, 100);
    visuals.widgets.hovered.weak_bg_fill = Color32::from_rgb(94, 95, 100);
    visuals.widgets.hovered.bg_stroke = egui::Stroke::NONE;
    visuals.widgets.active.bg_fill = Color32::from_rgb(28, 132, 245);
    visuals.widgets.active.weak_bg_fill = Color32::from_rgb(28, 132, 245);
    visuals.widgets.active.bg_stroke = egui::Stroke::NONE;
    visuals.widgets.open.bg_fill = Color32::from_rgb(94, 95, 100);
    visuals.widgets.open.weak_bg_fill = Color32::from_rgb(94, 95, 100);
    visuals.widgets.open.bg_stroke = egui::Stroke::NONE;
    visuals.widgets.inactive.corner_radius = 9.0.into();
    visuals.widgets.hovered.corner_radius = 9.0.into();
    visuals.widgets.active.corner_radius = 9.0.into();
    visuals.widgets.open.corner_radius = 9.0.into();
    visuals.window_corner_radius = 14.0.into();
    visuals.menu_corner_radius = 10.0.into();
    context.set_visuals(visuals);

    let mut style = (*context.style()).clone();
    style.spacing.item_spacing = egui::vec2(8.0, 6.0);
    style.spacing.button_padding = egui::vec2(13.0, 8.0);
    style.spacing.combo_width = 220.0;
    context.set_style(style);
}

fn launcher_config_path() -> PathBuf {
    let base = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir);
    base.join("Library")
        .join("Application Support")
        .join("shadPS4")
        .join("lbp3-launcher.json")
}

fn launcher_core_log_path() -> PathBuf {
    launcher_config_path().with_file_name("lbp3-launcher-core.log")
}

fn load_config(path: &Path) -> LauncherConfig {
    File::open(path)
        .ok()
        .and_then(|file| serde_json::from_reader(file).ok())
        .unwrap_or_default()
}

fn save_config(path: &Path, config: &LauncherConfig) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("json.tmp");
    let bytes = serde_json::to_vec_pretty(config).map_err(std::io::Error::other)?;
    fs::write(&temporary, bytes)?;
    fs::rename(temporary, path)
}

fn open_core_log() -> std::io::Result<File> {
    let path = launcher_core_log_path();
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(path)
}

fn bundle_contents_dir() -> Option<PathBuf> {
    let executable = std::env::current_exe().ok()?;
    let contents = executable.parent()?.parent()?;
    (contents.file_name() == Some(OsStr::new("Contents"))).then(|| contents.to_path_buf())
}

fn core_executable_path() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(|parent| parent.join(CORE_EXECUTABLE)))
        .unwrap_or_else(|| PathBuf::from(CORE_EXECUTABLE))
}

fn launcher_runtime_dir() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join("Library")
        .join("Caches")
        .join("shadPS4")
        .join("lbp3-runtime")
}

fn prepare_named_core(core: &Path, eboot: &Path) -> io::Result<PathBuf> {
    let title = read_game_title(eboot).ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "TITLE was not found in {}/sce_sys/param.sfo",
                eboot.parent().unwrap_or(eboot).display()
            ),
        )
    })?;
    let process_name = sanitize_process_file_name(&title);
    let runtime_dir = launcher_runtime_dir();
    fs::create_dir_all(&runtime_dir)?;

    let core_dir = core.parent().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "core executable has no parent directory",
        )
    })?;
    for dependency in CORE_RUNTIME_DEPENDENCIES {
        install_runtime_file(&core_dir.join(dependency), &runtime_dir.join(dependency))?;
    }

    let named_core = runtime_dir.join(process_name);
    install_runtime_file(core, &named_core)?;
    Ok(named_core)
}

fn install_runtime_file(source: &Path, destination: &Path) -> io::Result<()> {
    if !source.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("{} was not found", source.display()),
        ));
    }

    let file_name = destination
        .file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("runtime-file");
    let temporary = destination.with_file_name(format!(".{file_name}.{}.tmp", std::process::id()));
    if temporary.symlink_metadata().is_ok() {
        fs::remove_file(&temporary)?;
    }

    if fs::hard_link(source, &temporary).is_err() {
        fs::copy(source, &temporary)?;
    }
    if let Err(error) = fs::rename(&temporary, destination) {
        let _ = fs::remove_file(&temporary);
        return Err(error);
    }
    Ok(())
}

fn read_game_param(eboot: &Path, key: &str) -> Option<String> {
    let param_sfo = eboot.parent()?.join("sce_sys").join("param.sfo");
    parse_param_sfo_string(&fs::read(param_sfo).ok()?, key)
}

fn read_game_title(eboot: &Path) -> Option<String> {
    read_game_param(eboot, "TITLE")
}

fn parse_param_sfo_string(bytes: &[u8], requested_key: &str) -> Option<String> {
    const HEADER_SIZE: usize = 20;
    const ENTRY_SIZE: usize = 16;
    const PSF_MAGIC: u32 = 0x4653_5000;

    if read_u32(bytes, 0)? != PSF_MAGIC {
        return None;
    }
    let key_table = read_u32(bytes, 8)? as usize;
    let data_table = read_u32(bytes, 12)? as usize;
    let entry_count = read_u32(bytes, 16)? as usize;
    let index_end = HEADER_SIZE.checked_add(entry_count.checked_mul(ENTRY_SIZE)?)?;
    if index_end > bytes.len() || key_table > bytes.len() || data_table > bytes.len() {
        return None;
    }

    for index in 0..entry_count {
        let entry = HEADER_SIZE.checked_add(index.checked_mul(ENTRY_SIZE)?)?;
        let key_offset = read_u16(bytes, entry)? as usize;
        let value_length = read_u32(bytes, entry + 4)? as usize;
        let value_offset = read_u32(bytes, entry + 12)? as usize;
        let key_start = key_table.checked_add(key_offset)?;
        let key_tail = bytes.get(key_start..)?;
        let key_end = key_start.checked_add(key_tail.iter().position(|byte| *byte == 0)?)?;
        if bytes.get(key_start..key_end)? != requested_key.as_bytes() {
            continue;
        }

        let value_start = data_table.checked_add(value_offset)?;
        let value_end = value_start.checked_add(value_length)?;
        let value = bytes.get(value_start..value_end)?;
        let text_end = value
            .iter()
            .position(|byte| *byte == 0)
            .unwrap_or(value.len());
        let text = std::str::from_utf8(value.get(..text_end)?).ok()?.trim();
        return (!text.is_empty()).then(|| text.to_owned());
    }
    None
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn sanitize_process_file_name(title: &str) -> String {
    const MAX_BYTES: usize = 200;
    let mut name = String::new();
    for character in title.trim().chars() {
        let character = if character == '/' || character == ':' || character.is_control() {
            '_'
        } else {
            character
        };
        if name.len() + character.len_utf8() > MAX_BYTES {
            break;
        }
        name.push(character);
    }
    if name.is_empty() || name == "." || name == ".." {
        "shadPS4 game".to_owned()
    } else {
        name
    }
}

fn game_path_from_arguments(arguments: &[OsString]) -> Option<PathBuf> {
    arguments
        .windows(2)
        .find(|pair| pair[0] == OsStr::new("-g") || pair[0] == OsStr::new("--game"))
        .map(|pair| PathBuf::from(&pair[1]))
}

fn default_external_eboot() -> Option<PathBuf> {
    let home = std::env::var_os("HOME").map(PathBuf::from)?;
    let candidate = home
        .join("Desktop")
        .join("LBP3-macOS")
        .join("CUSA00063")
        .join("eboot.bin");
    candidate.is_file().then_some(candidate)
}

fn find_bundled_eboot(contents: &Path) -> Option<PathBuf> {
    let roots = [
        contents.join("Resources").join("Game"),
        contents.join("Game"),
    ];
    for root in roots {
        let canonical = root.join("CUSA00063").join("eboot.bin");
        if canonical.is_file() {
            return Some(canonical);
        }
        if let Some(found) = find_file_recursive(&root, "eboot.bin", 6) {
            return Some(found);
        }
    }
    None
}

fn find_bundled_addons(contents: &Path) -> Option<PathBuf> {
    let candidates = [
        contents.join("Resources").join("Addons"),
        contents.join("Addons"),
    ];
    candidates
        .into_iter()
        .find(|path| path.join("CUSA00063").is_dir())
}

fn find_file_recursive(root: &Path, name: &str, depth: usize) -> Option<PathBuf> {
    if depth == 0 || !root.is_dir() {
        return None;
    }
    let mut entries = fs::read_dir(root)
        .ok()?
        .filter_map(Result::ok)
        .collect::<Vec<_>>();
    entries.sort_by_key(|entry| entry.path());
    for entry in &entries {
        let path = entry.path();
        if path.is_file() && path.file_name() == Some(OsStr::new(name)) {
            return Some(path);
        }
    }
    for entry in entries {
        let path = entry.path();
        if entry.file_type().is_ok_and(|kind| kind.is_dir())
            && let Some(found) = find_file_recursive(&path, name, depth - 1)
        {
            return Some(found);
        }
    }
    None
}

fn is_eboot(path: &Path) -> bool {
    path.is_file()
        && path
            .file_name()
            .is_some_and(|name| name.eq_ignore_ascii_case("eboot.bin"))
}

fn build_core_args(config: &LauncherConfig, eboot: &Path, addons: Option<&Path>) -> Vec<OsString> {
    let mut args = vec![
        OsString::from("-g"),
        eboot.as_os_str().to_owned(),
        OsString::from("--resolution"),
        OsString::from(&config.resolution),
        OsString::from("--fullscreen"),
        OsString::from(config.fullscreen.to_string()),
        OsString::from("--lbp3-patch-bubbles"),
        OsString::from(config.patch_prize_bubbles.to_string()),
        OsString::from("--lbp3-disable-sprite-lights"),
        OsString::from(config.disable_sprite_lights.to_string()),
        OsString::from("--lbp3-disable-tone-map"),
        OsString::from(config.disable_tone_map.to_string()),
    ];
    if config.show_fps {
        args.push(OsString::from("--show-fps"));
    } else {
        args.push(OsString::from("--hide-fps"));
    }
    if config.lbp3_online {
        args.push(OsString::from("--lbp3-online"));
    }
    if let Some(path) = addons {
        args.push(OsString::from("--set-addon-folder"));
        args.push(path.as_os_str().to_owned());
    }
    args
}

fn probe_partychat() -> PartyChatState {
    let Ok(address) = PARTYCHAT_ADDRESS.parse::<SocketAddr>() else {
        return PartyChatState::Offline;
    };
    let Ok(mut stream) = TcpStream::connect_timeout(&address, Duration::from_millis(250)) else {
        return PartyChatState::Offline;
    };
    let _ = stream.set_read_timeout(Some(Duration::from_millis(350)));
    let _ = stream.set_write_timeout(Some(Duration::from_millis(350)));
    if stream
        .write_all(b"GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
        .is_err()
    {
        return PartyChatState::Offline;
    }
    let mut response = String::new();
    if stream
        .take(64 * 1024)
        .read_to_string(&mut response)
        .is_err()
    {
        return PartyChatState::Offline;
    }
    parse_partychat_response(&response).unwrap_or(PartyChatState::Offline)
}

fn parse_partychat_response(response: &str) -> Option<PartyChatState> {
    let (headers, body) = response.split_once("\r\n\r\n")?;
    if !headers.lines().next()?.contains(" 200 ") {
        return None;
    }
    let status: PartyChatResponse = serde_json::from_str(body).ok()?;
    if status.status.as_deref() != Some("ok") || status.service.as_deref() != Some("partychat-lbp3")
    {
        return None;
    }
    Some(PartyChatState::Running)
}

fn forward_to_core(arguments: &[OsString]) -> i32 {
    let core = core_executable_path();
    if !core.is_file() {
        eprintln!("{APP_NAME}: missing {}", core.display());
        return 1;
    }
    let named_core = match game_path_from_arguments(arguments) {
        Some(eboot) => match prepare_named_core(&core, &eboot) {
            Ok(path) => path,
            Err(error) => {
                eprintln!("{APP_NAME}: failed to prepare named game process: {error}");
                return 1;
            }
        },
        None => core,
    };
    let mut command = Command::new(&named_core);
    command.args(arguments);
    if let Some(parent) = named_core.parent() {
        command.current_dir(parent);
    }
    match command.status() {
        Ok(status) => status.code().unwrap_or(1),
        Err(error) => {
            eprintln!("{APP_NAME}: failed to start core: {error}");
            1
        }
    }
}

fn run_gui(initial_eboot: Option<PathBuf>) -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([680.0, 500.0])
            .with_min_inner_size([620.0, 470.0]),
        ..Default::default()
    };
    eframe::run_native(
        APP_NAME,
        options,
        Box::new(move |context| Ok(Box::new(LauncherApp::new(context, initial_eboot.clone())))),
    )
}

fn main() {
    let arguments = std::env::args_os()
        .skip(1)
        .filter(|argument| !argument.to_string_lossy().starts_with("-psn_"))
        .collect::<Vec<_>>();
    if arguments
        .iter()
        .any(|argument| argument == OsStr::new(LAUNCHER_UI_ARGUMENT))
    {
        if let Err(error) = run_gui(game_path_from_arguments(&arguments)) {
            eprintln!("{APP_NAME}: {error}");
            std::process::exit(1);
        }
        return;
    }
    if !arguments.is_empty() {
        std::process::exit(forward_to_core(&arguments));
    }
    if let Err(error) = run_gui(None) {
        eprintln!("{APP_NAME}: {error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn partychat_status_is_strictly_validated() {
        let response = concat!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n",
            r#"{"status":"ok","service":"partychat-lbp3","onlineId":"nugget","emulatorAttached":true}"#
        );
        assert!(matches!(
            parse_partychat_response(response),
            Some(PartyChatState::Running)
        ));
        assert!(parse_partychat_response("HTTP/1.1 500 Nope\r\n\r\n{}").is_none());
    }

    #[test]
    fn launcher_always_passes_a_game_and_explicit_patch_state() {
        let config = LauncherConfig::default();
        let args = build_core_args(&config, Path::new("/game/eboot.bin"), None);
        assert_eq!(args.first(), Some(&OsString::from("-g")));
        assert!(args.contains(&OsString::from("--resolution")));
        assert!(args.contains(&OsString::from("--lbp3-patch-bubbles")));
        assert!(args.contains(&OsString::from("--lbp3-disable-sprite-lights")));
        assert!(args.contains(&OsString::from("--lbp3-disable-tone-map")));
    }

    fn test_sfo(key: &str, value: &str) -> Vec<u8> {
        let key_table = 20 + 16;
        let data_table = key_table + key.len() + 1;
        let mut sfo = Vec::new();
        sfo.extend_from_slice(&0x4653_5000_u32.to_le_bytes());
        sfo.extend_from_slice(&0x0000_0101_u32.to_le_bytes());
        sfo.extend_from_slice(&(key_table as u32).to_le_bytes());
        sfo.extend_from_slice(&(data_table as u32).to_le_bytes());
        sfo.extend_from_slice(&1_u32.to_le_bytes());
        sfo.extend_from_slice(&0_u16.to_le_bytes());
        sfo.extend_from_slice(&0x0204_u16.to_le_bytes());
        sfo.extend_from_slice(&((value.len() + 1) as u32).to_le_bytes());
        sfo.extend_from_slice(&((value.len() + 1) as u32).to_le_bytes());
        sfo.extend_from_slice(&0_u32.to_le_bytes());
        sfo.extend_from_slice(key.as_bytes());
        sfo.push(0);
        sfo.extend_from_slice(value.as_bytes());
        sfo.push(0);
        sfo
    }

    #[test]
    fn parses_arbitrary_param_sfo_strings_and_sanitizes_title() {
        let title = "LittleBigPlanet™3 (EU)";
        let title_sfo = test_sfo("TITLE", title);
        let version_sfo = test_sfo("APP_VER", "01.26");

        assert_eq!(
            parse_param_sfo_string(&title_sfo, "TITLE").as_deref(),
            Some(title)
        );
        assert_eq!(
            parse_param_sfo_string(&version_sfo, "APP_VER").as_deref(),
            Some("01.26")
        );
        assert_eq!(parse_param_sfo_string(&title_sfo, "APP_VER"), None);
        assert_eq!(
            sanitize_process_file_name(" bad/name:here "),
            "bad_name_here"
        );
    }

    #[test]
    fn combines_patch_version_and_partychat_warnings() {
        let mut config = LauncherConfig {
            lbp3_online: true,
            ..LauncherConfig::default()
        };
        let warning = derive_launch_warning(&config, Some("01.25".to_owned()), false);
        assert!(warning.patch_version_mismatch);
        assert!(warning.partychat_offline);

        let warning = derive_launch_warning(&config, Some("01.26".to_owned()), true);
        assert!(warning.is_empty());

        config.patch_prize_bubbles = false;
        config.disable_sprite_lights = false;
        config.disable_tone_map = false;
        let warning = derive_launch_warning(&config, None, true);
        assert!(warning.is_empty());
    }

    #[test]
    fn incompatible_version_disables_effective_patches_without_changing_saved_choices() {
        let config = LauncherConfig {
            patch_prize_bubbles: true,
            disable_sprite_lights: true,
            disable_tone_map: true,
            ..LauncherConfig::default()
        };
        let effective = config.with_patches_disabled();
        assert!(config.patch_prize_bubbles);
        assert!(config.disable_sprite_lights);
        assert!(config.disable_tone_map);
        assert!(!effective.patch_prize_bubbles);
        assert!(!effective.disable_sprite_lights);
        assert!(!effective.disable_tone_map);

        let args = build_core_args(&effective, Path::new("/game/eboot.bin"), None);
        for flag in [
            "--lbp3-patch-bubbles",
            "--lbp3-disable-sprite-lights",
            "--lbp3-disable-tone-map",
        ] {
            let index = args
                .iter()
                .position(|argument| argument == OsStr::new(flag))
                .unwrap();
            assert_eq!(args.get(index + 1), Some(&OsString::from("false")));
        }
    }

    #[test]
    fn finds_game_argument_for_named_process() {
        let arguments = vec![
            OsString::from(LAUNCHER_UI_ARGUMENT),
            OsString::from("--show-fps"),
            OsString::from("-g"),
            OsString::from("/game/eboot.bin"),
        ];
        assert_eq!(
            game_path_from_arguments(&arguments),
            Some(PathBuf::from("/game/eboot.bin"))
        );
    }
}
