#ifndef OFFLINE_MODE_H
#define OFFLINE_MODE_H

/*
 * Offline media mode
 * ------------------
 * Plays MP3 (/sdcard/...), MP4 (/sdcard/videos) and shows JPEG photos
 * (/sdcard/photos) without network and without the AI chat.
 *
 * Input mapping (wired up by the board):
 *   Vol+ hold      -> enter / leave offline mode   (Toggle)
 *   Touch click    -> play / pause (photo: slideshow on/off)  (OnTouchClick)
 *   Touch double   -> switch source Music -> Video -> Photo    (OnTouchDoubleClick)
 *   Touch hold     -> cycle volume                             (OnTouchLongPress)
 *   Vol+ click     -> next                                     (OnNext)
 *   Vol- click     -> previous                                 (OnPrev)
 */

#ifdef CONFIG_OFFLINE_MODE_ENABLE

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class OfflineMode {
public:
    enum class Source { Music = 0, Video, Photo };

    static OfflineMode& GetInstance();

    bool IsActive() const { return active_.load(); }
    Source GetSource() const { return source_; }

    // GPIO that, when held low at boot, starts the device directly in offline
    // mode (no WiFi / no AI). Set by the board. GPIO_NUM_NC disables it.
    void SetBootGpio(gpio_num_t gpio) { boot_gpio_ = gpio; }
    bool BootRequested() const;

    // SD bookkeeping so the card is mounted exactly once.
    bool SdMounted() const { return sd_mounted_; }
    void SetSdMounted(bool mounted) { sd_mounted_ = mounted; }

    // Called by Application::Start() when BootRequested(): mounts SD, inits
    // players and enters offline mode. Never starts the network.
    void StartStandalone();

    // Button entry points (safe to call from any task; work is queued).
    void Toggle();
    void OnTouchClick();
    void OnTouchDoubleClick();
    void OnTouchLongPress();
    void OnNext();
    void OnPrev();

private:
    enum class Cmd : uint8_t {
        Toggle, EnterOnly, PlayPause, SwitchSource, CycleVolume, Next, Prev,
        AutoNext,      // end of track / video / slideshow tick
    };

    OfflineMode() = default;
    void EnsureTask();
    void Post(Cmd cmd);
    static void TaskEntry(void* arg);
    void TaskLoop();
    void Handle(Cmd cmd);

    bool EnsureMedia();
    void Enter();
    void Exit();
    void StartSource(Source src);
    void StopSource();

    // Music / video helpers
    void PlayPauseMusic();
    void PlayPauseVideo();
    void StepVideo(int dir);

    // Photo viewer
    bool ScanPhotos();
    bool ShowPhoto(int index);
    void StepPhoto(int dir);
    void EnterPhotoView();
    void LeavePhotoView();
    bool DecodeJpegToCanvas(const std::string& path);

    void Notify(const std::string& text);

    std::atomic<bool> active_{false};
    bool standalone_ = false;
    bool sd_mounted_ = false;
    bool media_ready_ = false;
    gpio_num_t boot_gpio_ = GPIO_NUM_NC;
    Source source_ = Source::Music;

    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;

    // Photo state
    std::vector<std::string> photos_;
    int photo_index_ = -1;
    bool slideshow_ = true;
    void* canvas_ = nullptr;          // lv_obj_t*
    uint16_t* canvas_buf_ = nullptr;  // LCD-sized RGB565 (LVGL little-endian)
    int lcd_w_ = 0;
    int lcd_h_ = 0;

    int volume_step_ = 0;
    int64_t standalone_start_us_ = 0;  // ignore the long-press that is still held at boot
};

#endif  // CONFIG_OFFLINE_MODE_ENABLE
#endif  // OFFLINE_MODE_H
