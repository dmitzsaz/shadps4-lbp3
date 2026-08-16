// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use eframe::egui::{self, Color32, RichText};
use serde::{Deserialize, Serialize};

const APP_NAME: &str = "shadPS4 LBP3";
const CORE_EXECUTABLE: &str = "shadps4-core";
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

#[derive(Debug, Clone, Default)]
enum PartyChatState {
    #[default]
    Checking,
    Offline,
    Running {
        online_id: Option<String>,
        emulator_attached: bool,
    },
}

#[derive(Debug, Deserialize)]
struct PartyChatResponse {
    status: Option<String>,
    service: Option<String>,
    #[serde(rename = "onlineId")]
    online_id: Option<String>,
    #[serde(rename = "emulatorAttached", default)]
    emulator_attached: bool,
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
    run_status: String,
    error: Option<String>,
}

impl LauncherApp {
    fn new(context: &eframe::CreationContext<'_>) -> Self {
        configure_style(&context.egui_ctx);
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

        Self {
            config,
            config_path,
            bundled_eboot,
            bundled_addons,
            partychat: PartyChatMonitor::new(context.egui_ctx.clone()),
            child: None,
            run_status: "Эмулятор не запущен".to_owned(),
            error: None,
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
                self.run_status = match status.code() {
                    Some(0) => "Эмулятор завершил работу".to_owned(),
                    Some(code) => format!("Эмулятор завершился с кодом {code}"),
                    None => "Эмулятор аварийно завершился".to_owned(),
                };
                self.child = None;
            }
            Ok(None) => self.run_status = "Игра запущена".to_owned(),
            Err(error) => {
                self.run_status = "Не удалось проверить процесс".to_owned();
                self.error = Some(error.to_string());
                self.child = None;
            }
        }
    }

    fn launch(&mut self) {
        self.error = None;
        if self.child.is_some() {
            return;
        }

        let Some(eboot) = self.selected_eboot() else {
            self.error = Some("Укажи существующий eboot.bin".to_owned());
            return;
        };
        if !is_eboot(&eboot) {
            self.error = Some(format!("eboot.bin не найден: {}", eboot.display()));
            return;
        }
        let addon_root = self.addon_root();
        if let Some(path) = &addon_root
            && !path.is_dir()
        {
            self.error = Some(format!("Папка DLC не найдена: {}", path.display()));
            return;
        }

        let core = core_executable_path();
        if !core.is_file() {
            self.error = Some(format!("Внутри .app не найден {CORE_EXECUTABLE}"));
            return;
        }

        if let Err(error) = save_config(&self.config_path, &self.config) {
            self.error = Some(format!("Не удалось сохранить настройки: {error}"));
            return;
        }

        let mut command = Command::new(&core);
        command.args(build_core_args(&self.config, &eboot, addon_root.as_deref()));
        if let Some(parent) = core.parent() {
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
                self.run_status = "Игра запускается…".to_owned();
            }
            Err(error) => {
                self.error = Some(format!("Не удалось запустить эмулятор: {error}"));
            }
        }
    }

    fn draw_game_section(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        ui.heading("Игра");
        if let Some(path) = &self.bundled_eboot {
            changed |= ui
                .radio_value(
                    &mut self.config.prefer_bundled_game,
                    true,
                    "Игра внутри .app",
                )
                .changed();
            ui.label(RichText::new(path.display().to_string()).small().weak());
        } else {
            ui.label(RichText::new("Встроенный eboot.bin не найден").weak());
        }
        changed |= ui
            .radio_value(
                &mut self.config.prefer_bundled_game,
                false,
                "Внешний eboot.bin",
            )
            .changed();
        ui.horizontal(|ui| {
            let field = ui.add_sized(
                [ui.available_width() - 92.0, 26.0],
                egui::TextEdit::singleline(&mut self.config.external_eboot)
                    .hint_text("/путь/к/CUSA00063/eboot.bin"),
            );
            changed |= field.changed();
            if ui.button("Выбрать…").clicked()
                && let Some(path) = rfd::FileDialog::new()
                    .add_filter("PlayStation executable", &["bin"])
                    .set_file_name("eboot.bin")
                    .pick_file()
            {
                self.config.external_eboot = path.to_string_lossy().into_owned();
                self.config.prefer_bundled_game = false;
                changed = true;
            }
        });

        ui.add_space(10.0);
        ui.label(RichText::new("DLC / Add-ons").strong());
        ui.horizontal(|ui| {
            let field = ui.add_sized(
                [ui.available_width() - 92.0, 26.0],
                egui::TextEdit::singleline(&mut self.config.addon_root)
                    .hint_text("Необязательно: корень addcont"),
            );
            changed |= field.changed();
            if ui.button("Выбрать…").clicked()
                && let Some(path) = rfd::FileDialog::new().pick_folder()
            {
                self.config.addon_root = path.to_string_lossy().into_owned();
                changed = true;
            }
        });
        ui.label(
            RichText::new("Ожидаемая структура: <корень>/CUSA00063/<DLC>/sce_sys/param.sfo")
                .small()
                .weak(),
        );
        if let Some(path) = &self.bundled_addons
            && ui.small_button("Использовать DLC из .app").clicked()
        {
            self.config.addon_root = path.to_string_lossy().into_owned();
            changed = true;
        }
        changed
    }

    fn draw_video_section(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        ui.heading("Видео");
        ui.horizontal(|ui| {
            ui.label("Разрешение рендера и окна:");
            egui::ComboBox::from_id_salt("resolution")
                .selected_text(&self.config.resolution)
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
        });
        changed |= ui
            .checkbox(&mut self.config.fullscreen, "Fullscreen")
            .changed();
        changed |= ui
            .checkbox(&mut self.config.show_fps, "Показывать FPS")
            .changed();
        changed
    }

    fn draw_online_section(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        ui.heading("LBP3 Online");
        changed |= ui
            .checkbox(&mut self.config.lbp3_online, "Включить LBP3 Online")
            .changed();
        match self.partychat.state() {
            PartyChatState::Checking => {
                ui.colored_label(Color32::from_rgb(235, 185, 70), "● Проверяю PartyChat…");
            }
            PartyChatState::Offline => {
                ui.colored_label(Color32::from_rgb(230, 95, 95), "● PartyChat не запущен");
            }
            PartyChatState::Running {
                online_id,
                emulator_attached,
            } => {
                let suffix = online_id.map(|id| format!(" — {id}")).unwrap_or_default();
                ui.colored_label(
                    Color32::from_rgb(95, 215, 135),
                    format!("● PartyChat запущен{suffix}"),
                );
                if emulator_attached {
                    ui.label(RichText::new("Эмулятор подключён к bridge").small().weak());
                }
            }
        }
        ui.label(
            RichText::new(
                "Галочку можно оставить включённой без PartyChat: игра автоматически уйдёт в offline.",
            )
            .small()
            .weak(),
        );
        changed
    }

    fn draw_patches_section(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        ui.heading("Патчи совместимости LBP3 v1.26");
        changed |= ui
            .checkbox(
                &mut self.config.patch_prize_bubbles,
                "Патч пузырьков-наград (рекомендуется)",
            )
            .changed();
        changed |= ui
            .checkbox(
                &mut self.config.disable_sprite_lights,
                "Отключить Sprite Lights",
            )
            .changed();
        changed |= ui
            .checkbox(
                &mut self.config.disable_tone_map,
                "Отключить ToneMap Sprite Lights",
            )
            .changed();
        ui.label(
            RichText::new(
                "Отключения света нужны только как fallback: текущий нативный фикс рассчитан на включённые эффекты.",
            )
            .small()
            .weak(),
        );
        changed
    }
}

impl eframe::App for LauncherApp {
    fn update(&mut self, context: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_child();
        context.request_repaint_after(Duration::from_millis(500));

        egui::TopBottomPanel::top("header").show(context, |ui| {
            ui.add_space(12.0);
            ui.horizontal(|ui| {
                ui.heading(RichText::new(APP_NAME).size(24.0));
                ui.separator();
                ui.label(RichText::new(&self.run_status).strong());
            });
            ui.add_space(10.0);
        });

        egui::CentralPanel::default().show(context, |ui| {
            let mut changed = false;
            egui::ScrollArea::vertical().show(ui, |ui| {
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    changed |= self.draw_game_section(ui);
                });
                ui.add_space(10.0);
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    changed |= self.draw_video_section(ui);
                });
                ui.add_space(10.0);
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    changed |= self.draw_online_section(ui);
                });
                ui.add_space(10.0);
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    changed |= self.draw_patches_section(ui);
                });
                ui.add_space(12.0);

                if let Some(error) = &self.error {
                    ui.colored_label(Color32::from_rgb(240, 105, 105), error);
                    ui.add_space(8.0);
                }

                let eboot_valid = self.selected_eboot().is_some_and(|path| is_eboot(&path));
                let addon_valid = self.addon_root().is_none_or(|path| path.is_dir());
                let core_valid = core_executable_path().is_file();
                let can_launch = self.child.is_none() && eboot_valid && addon_valid && core_valid;
                if ui
                    .add_enabled(
                        can_launch,
                        egui::Button::new(RichText::new("Запустить LittleBigPlanet 3").size(18.0))
                            .min_size([ui.available_width(), 42.0].into()),
                    )
                    .clicked()
                {
                    self.launch();
                }
                if !eboot_valid {
                    ui.label(
                        RichText::new("Для запуска выбери валидный eboot.bin")
                            .small()
                            .weak(),
                    );
                } else if !core_valid {
                    ui.label(
                        RichText::new(format!("В bundle отсутствует {CORE_EXECUTABLE}"))
                            .small()
                            .weak(),
                    );
                }
                ui.add_space(6.0);
            });

            if changed && let Err(error) = save_config(&self.config_path, &self.config) {
                self.error = Some(format!("Не удалось сохранить настройки: {error}"));
            }
        });
    }
}

impl Drop for LauncherApp {
    fn drop(&mut self) {
        let _ = save_config(&self.config_path, &self.config);
    }
}

fn configure_style(context: &egui::Context) {
    let mut visuals = egui::Visuals::dark();
    visuals.panel_fill = Color32::from_rgb(20, 22, 28);
    visuals.window_fill = Color32::from_rgb(25, 28, 35);
    visuals.selection.bg_fill = Color32::from_rgb(72, 105, 185);
    context.set_visuals(visuals);

    let mut style = (*context.style()).clone();
    style.spacing.item_spacing = egui::vec2(8.0, 7.0);
    style.spacing.button_padding = egui::vec2(12.0, 7.0);
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
    Some(PartyChatState::Running {
        online_id: status.online_id,
        emulator_attached: status.emulator_attached,
    })
}

fn forward_to_core(arguments: &[OsString]) -> i32 {
    let core = core_executable_path();
    if !core.is_file() {
        eprintln!("{APP_NAME}: missing {}", core.display());
        return 1;
    }
    let mut command = Command::new(core);
    command.args(arguments);
    match command.status() {
        Ok(status) => status.code().unwrap_or(1),
        Err(error) => {
            eprintln!("{APP_NAME}: failed to start core: {error}");
            1
        }
    }
}

fn run_gui() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([760.0, 790.0])
            .with_min_inner_size([650.0, 640.0]),
        ..Default::default()
    };
    eframe::run_native(
        APP_NAME,
        options,
        Box::new(|context| Ok(Box::new(LauncherApp::new(context)))),
    )
}

fn main() {
    let arguments = std::env::args_os()
        .skip(1)
        .filter(|argument| !argument.to_string_lossy().starts_with("-psn_"))
        .collect::<Vec<_>>();
    if !arguments.is_empty() {
        std::process::exit(forward_to_core(&arguments));
    }
    if let Err(error) = run_gui() {
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
            Some(PartyChatState::Running {
                emulator_attached: true,
                ..
            })
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
}
