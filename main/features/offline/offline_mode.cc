#include "offline_mode.h"

#ifdef CONFIG_OFFLINE_MODE_ENABLE

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

extern "C" {
#include "esp_jpeg_dec.h"
}

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "display/lcd_display.h"
#include "features/mp4/mp4_player.h"
#include "features/music/esp32_sd_music.h"
#include "sd_card.h"

#define TAG "OfflineMode"

namespace {
constexpr const char* kPhotoDir = "/sdcard/photos";
constexpr TickType_t kSlideshowPeriod = pdMS_TO_TICKS(5000);
constexpr size_t kMaxPhotoFileBytes = 6 * 1024 * 1024;
constexpr int kMaxDecodedBytes = 5 * 1024 * 1024;  // RGB565 buffer limit (PSRAM is 8 MB)
constexpr int kVolumeSteps[] = {30, 50, 70, 90};

bool IsJpegName(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == "jpg" || ext == "jpeg";
}
}  // namespace

OfflineMode& OfflineMode::GetInstance() {
    static OfflineMode instance;
    return instance;
}

bool OfflineMode::BootRequested() const {
    if (boot_gpio_ == GPIO_NUM_NC) return false;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << boot_gpio_;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(20));
    return gpio_get_level(boot_gpio_) == 0;  // buttons are active-low
}

/* ------------------------------------------------------------------ */
/*  Command queue                                                      */
/* ------------------------------------------------------------------ */

void OfflineMode::EnsureTask() {
    if (task_) return;
    queue_ = xQueueCreate(8, sizeof(Cmd));
    xTaskCreate(&OfflineMode::TaskEntry, "offline_ctl", 6 * 1024, this, 4, &task_);
}

void OfflineMode::Post(Cmd cmd) {
    EnsureTask();
    if (queue_) xQueueSend(queue_, &cmd, 0);
}

void OfflineMode::TaskEntry(void* arg) {
    static_cast<OfflineMode*>(arg)->TaskLoop();
}

void OfflineMode::TaskLoop() {
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (active_ && source_ == Source::Photo && slideshow_) wait = kSlideshowPeriod;
        Cmd cmd;
        if (xQueueReceive(queue_, &cmd, wait) == pdTRUE) {
            Handle(cmd);
        } else {
            Handle(Cmd::AutoNext);  // slideshow tick
        }
    }
}

void OfflineMode::Toggle() {
    // Vol+ is still physically held when we boot straight into offline mode;
    // its long-press event must not toggle us right back out.
    if (standalone_ && esp_timer_get_time() - standalone_start_us_ < 3000000) return;
    Post(Cmd::Toggle);
}
void OfflineMode::OnTouchClick()       { if (active_) Post(Cmd::PlayPause); }
void OfflineMode::OnTouchDoubleClick() { if (active_) Post(Cmd::SwitchSource); }
void OfflineMode::OnTouchLongPress()   { if (active_) Post(Cmd::CycleVolume); }
void OfflineMode::OnNext()             { if (active_) Post(Cmd::Next); }
void OfflineMode::OnPrev()             { if (active_) Post(Cmd::Prev); }

void OfflineMode::Handle(Cmd cmd) {
    if (cmd == Cmd::Toggle) {
        if (active_) Exit(); else Enter();
        return;
    }
    if (!active_) return;

    switch (cmd) {
        case Cmd::PlayPause:
            if (source_ == Source::Music) PlayPauseMusic();
            else if (source_ == Source::Video) PlayPauseVideo();
            else {
                slideshow_ = !slideshow_;
                Notify(slideshow_ ? "Slideshow: ON" : "Slideshow: OFF");
            }
            break;
        case Cmd::SwitchSource: {
            Source next = static_cast<Source>((static_cast<int>(source_) + 1) % 3);
            StopSource();
            StartSource(next);
            break;
        }
        case Cmd::CycleVolume: {
            volume_step_ = (volume_step_ + 1) % (sizeof(kVolumeSteps) / sizeof(kVolumeSteps[0]));
            int v = kVolumeSteps[volume_step_];
            Board::GetInstance().GetAudioCodec()->SetOutputVolume(v);
            Notify("Volume: " + std::to_string(v));
            break;
        }
        case Cmd::Next:
            if (source_ == Source::Music) {
                if (auto m = Application::GetInstance().GetSdMusic()) m->Next();
            } else if (source_ == Source::Video) {
                StepVideo(+1);
            } else {
                StepPhoto(+1);
            }
            break;
        case Cmd::Prev:
            if (source_ == Source::Music) {
                if (auto m = Application::GetInstance().GetSdMusic()) m->Prev();
            } else if (source_ == Source::Video) {
                StepVideo(-1);
            } else {
                StepPhoto(-1);
            }
            break;
        case Cmd::AutoNext:
            if (source_ == Source::Video) StepVideo(+1);
            else if (source_ == Source::Photo && slideshow_) StepPhoto(+1);
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Enter / leave                                                      */
/* ------------------------------------------------------------------ */

bool OfflineMode::EnsureMedia() {
    if (media_ready_) return true;

    auto& app = Application::GetInstance();
    auto sd = Board::GetInstance().GetSdCard();
    if (!sd) return false;

    if (!sd_mounted_) {
        if (sd->Initialize() != ESP_OK) {
            ESP_LOGW(TAG, "SD card mount failed");
            return false;
        }
        sd_mounted_ = true;
    }
    if (!app.GetSdMusic()) app.InitSdMusic();
    if (!app.GetMp4Video()) app.InitMp4Video();

    if (auto mp4 = app.GetMp4Video()) {
        // Auto-advance when a video finishes (return false = do not repeat).
        mp4->SetEndCallback([this](const std::string&) {
            if (active_ && source_ == Source::Video) Post(Cmd::AutoNext);
            return false;
        });
    }
    if (auto m = app.GetSdMusic()) {
        m->SetRepeatMode(Esp32SdMusic::RepeatMode::RepeatAll);
    }
    media_ready_ = true;
    return true;
}

void OfflineMode::StartStandalone() {
    standalone_ = true;
    standalone_start_us_ = esp_timer_get_time();
    ESP_LOGI(TAG, "Boot request: starting offline mode without network");
    Post(Cmd::Toggle);
}

void OfflineMode::Enter() {
    if (!EnsureMedia()) {
        Notify("No SD card");
        return;
    }
    auto& app = Application::GetInstance();
    app.StopAllMedia();
    app.GetAudioService().EnableWakeWordDetection(false);
    active_ = true;
    ESP_LOGI(TAG, "Offline mode ON");
    source_ = Source::Music;
    StartSource(source_);
}

void OfflineMode::Exit() {
    StopSource();
    active_ = false;
    ESP_LOGI(TAG, "Offline mode OFF");
    if (standalone_) {
        // The network was never started: a reboot returns to the normal flow.
        esp_restart();
        return;
    }
    Notify("Online mode");
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    });
}

void OfflineMode::StartSource(Source src) {
    source_ = src;
    auto& app = Application::GetInstance();
    switch (src) {
        case Source::Music: {
            Notify("Offline: Music");
            auto m = app.GetSdMusic();
            if (m && m->GetTotalTracks() == 0) m->LoadPlaylist();
            if (!m || !m->Play()) Notify("No MP3 on SD");
            break;
        }
        case Source::Video: {
            Notify("Offline: Video");
            auto v = app.GetMp4Video();
            if (!v || v->ScanDirectory() == 0) { Notify("No MP4 in /videos"); break; }
            if (!app.PlayMp4Video(v->GetPlaylist().front().path)) Notify("MP4 play failed");
            break;
        }
        case Source::Photo:
            Notify("Offline: Photos");
            EnterPhotoView();
            if (ScanPhotos()) ShowPhoto(0);
            else Notify("No JPG in /photos");
            break;
    }
}

void OfflineMode::StopSource() {
    auto& app = Application::GetInstance();
    app.StopAllMedia();
    if (source_ == Source::Photo) LeavePhotoView();
}

/* ------------------------------------------------------------------ */
/*  Music / video                                                      */
/* ------------------------------------------------------------------ */

void OfflineMode::PlayPauseMusic() {
    auto m = Application::GetInstance().GetSdMusic();
    if (!m) return;
    if (m->GetState() == Esp32SdMusic::PlayerState::Playing) {
        m->Pause();
        Notify("Paused");
    } else {
        m->Play();  // resumes when paused, otherwise starts current track
        Notify("Play");
    }
}

void OfflineMode::PlayPauseVideo() {
    auto v = Application::GetInstance().GetMp4Video();
    if (!v) return;
    switch (v->GetState()) {
        case Mp4PlayerState::Playing: v->Pause(); Notify("Paused"); break;
        case Mp4PlayerState::Paused:  v->Resume(); Notify("Play"); break;
        default: StepVideo(0); break;
    }
}

// dir: +1 next, -1 previous, 0 = (re)play current / first
void OfflineMode::StepVideo(int dir) {
    auto& app = Application::GetInstance();
    auto v = app.GetMp4Video();
    if (!v) return;
    if (v->GetPlaylist().empty() && v->ScanDirectory() == 0) { Notify("No MP4 in /videos"); return; }
    const auto& list = v->GetPlaylist();
    int n = static_cast<int>(list.size());
    int idx = v->GetCurrentIndex();
    if (idx < 0) idx = 0;
    else idx = ((idx + dir) % n + n) % n;
    app.PlayMp4Video(list[idx].path);
}

/* ------------------------------------------------------------------ */
/*  Photo viewer (JPEG -> LVGL canvas, nearest-neighbour fit)          */
/* ------------------------------------------------------------------ */

bool OfflineMode::ScanPhotos() {
    photos_.clear();
    photo_index_ = -1;
    DIR* dir = opendir(kPhotoDir);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", kPhotoDir);
        return false;
    }
    while (auto* e = readdir(dir)) {
        if (e->d_type != DT_REG) continue;
        std::string name(e->d_name);
        if (IsJpegName(name)) photos_.push_back(std::string(kPhotoDir) + "/" + name);
    }
    closedir(dir);
    std::sort(photos_.begin(), photos_.end());
    ESP_LOGI(TAG, "Found %u photos", static_cast<unsigned>(photos_.size()));
    return !photos_.empty();
}

void OfflineMode::EnterPhotoView() {
    auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (!lcd || canvas_) return;
    lcd_w_ = lcd->width();
    lcd_h_ = lcd->height();
    size_t bytes = static_cast<size_t>(lcd_w_) * lcd_h_ * sizeof(uint16_t);
    canvas_buf_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!canvas_buf_) {
        ESP_LOGE(TAG, "Canvas buffer alloc failed");
        return;
    }
    memset(canvas_buf_, 0, bytes);
    {
        DisplayLockGuard lock(lcd);
        lv_obj_t* c = lv_canvas_create(lv_scr_act());
        lv_canvas_set_buffer(c, canvas_buf_, lcd_w_, lcd_h_, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_size(c, lcd_w_, lcd_h_);
        lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_COVER);
        lv_obj_move_foreground(c);
        canvas_ = c;
    }
    lcd->SetMediaOverlayActive(true);
}

void OfflineMode::LeavePhotoView() {
    auto* lcd = dynamic_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
    if (canvas_ && lcd) {
        DisplayLockGuard lock(lcd);
        lv_obj_del(static_cast<lv_obj_t*>(canvas_));
    }
    canvas_ = nullptr;
    if (canvas_buf_) {
        heap_caps_free(canvas_buf_);
        canvas_buf_ = nullptr;
    }
    if (lcd) lcd->SetMediaOverlayActive(false);
}

void OfflineMode::StepPhoto(int dir) {
    if (photos_.empty() && !ScanPhotos()) { Notify("No JPG in /photos"); return; }
    int n = static_cast<int>(photos_.size());
    int idx = ((photo_index_ + dir) % n + n) % n;
    ShowPhoto(idx);
}

bool OfflineMode::ShowPhoto(int index) {
    if (index < 0 || index >= static_cast<int>(photos_.size())) return false;
    photo_index_ = index;
    if (!DecodeJpegToCanvas(photos_[index])) {
        Notify("Cannot show photo");
        return false;
    }
    return true;
}

bool OfflineMode::DecodeJpegToCanvas(const std::string& path) {
    if (!canvas_ || !canvas_buf_) return false;

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 ||
        static_cast<size_t>(st.st_size) > kMaxPhotoFileBytes) {
        ESP_LOGW(TAG, "Bad or too large file: %s", path.c_str());
        return false;
    }

    // JPEG input must be 16-byte aligned for the hardware-friendly decoder.
    uint8_t* in = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!in) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { heap_caps_free(in); return false; }
    size_t got = fread(in, 1, st.st_size, f);
    fclose(f);
    if (got != static_cast<size_t>(st.st_size)) { heap_caps_free(in); return false; }

    jpeg_dec_config_t cfg = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .scale = {.width = 0, .height = 0},
        .clipper = {.width = 0, .height = 0},
        .rotate = JPEG_ROTATE_0D,
        .block_enable = false,
    };
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) { heap_caps_free(in); return false; }

    auto* io = static_cast<jpeg_dec_io_t*>(heap_caps_calloc(1, sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* hdr = static_cast<jpeg_dec_header_info_t*>(
        heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* out = nullptr;
    bool ok = false;

    do {
        if (!io || !hdr) break;
        memset(io, 0, sizeof(*io));
        memset(hdr, 0, sizeof(*hdr));
        io->inbuf = in;
        io->inbuf_len = static_cast<int>(got);
        if (jpeg_dec_parse_header(dec, io, hdr) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG header parse failed (progressive JPEG is not supported)");
            break;
        }
        int out_len = 0;
        if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || out_len <= 0 || out_len > kMaxDecodedBytes) {
            ESP_LOGW(TAG, "Photo %dx%d too large to decode (%d bytes) - resize it", hdr->width, hdr->height, out_len);
            break;
        }
        out = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!out) break;
        io->outbuf = out;
        int consumed = io->inbuf_len - io->inbuf_remain;
        io->inbuf = in + consumed;
        io->inbuf_len = io->inbuf_remain;
        if (jpeg_dec_process(dec, io) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG decode failed");
            break;
        }

        // Fit into the LCD, keep aspect ratio, black bars.
        const int sw = hdr->width, sh = hdr->height;
        const float scale = std::min(static_cast<float>(lcd_w_) / sw, static_cast<float>(lcd_h_) / sh);
        const int dw = std::max(1, static_cast<int>(sw * scale));
        const int dh = std::max(1, static_cast<int>(sh * scale));
        const int ox = (lcd_w_ - dw) / 2;
        const int oy = (lcd_h_ - dh) / 2;
        const uint16_t* src = reinterpret_cast<const uint16_t*>(out);

        memset(canvas_buf_, 0, static_cast<size_t>(lcd_w_) * lcd_h_ * sizeof(uint16_t));
        for (int y = 0; y < dh; ++y) {
            const uint16_t* srow = src + static_cast<size_t>(y * sh / dh) * sw;
            uint16_t* drow = canvas_buf_ + static_cast<size_t>(oy + y) * lcd_w_ + ox;
            for (int x = 0; x < dw; ++x) drow[x] = srow[x * sw / dw];
        }
        ok = true;
    } while (false);

    if (out) heap_caps_free(out);
    if (io) heap_caps_free(io);
    if (hdr) heap_caps_free(hdr);
    jpeg_dec_close(dec);
    heap_caps_free(in);

    if (ok) {
        if (auto* lcd = Board::GetInstance().GetDisplay()) {
            DisplayLockGuard lock(lcd);
            lv_obj_invalidate(static_cast<lv_obj_t*>(canvas_));
        }
    }
    return ok;
}

void OfflineMode::Notify(const std::string& text) {
    if (auto* d = Board::GetInstance().GetDisplay()) d->ShowNotification(text, 1500);
}

#endif  // CONFIG_OFFLINE_MODE_ENABLE
