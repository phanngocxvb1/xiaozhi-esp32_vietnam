# ESP32-S3 N16R8 – Offline Media board

| Thành phần | GPIO |
|---|---|
| ST7789 DC / RES | 1 / 2 |
| ST7789 SDA(MOSI) / SCL(CLK) | 41 / 42 |
| ST7789 BLK | 17 |
| INMP441 WS / SCK / SD | 4 / 5 / 6 |
| MAX98357A DIN / BCLK / LRC | 7 / 15 / 16 |
| SD MISO / MOSI / CLK | 12 / 13 / 14 |
| SD CS | 10 |
| Touch | 47 |
| Vol+ / Vol- | 39 / 40 |

## Thẻ nhớ
```
/sdcard/*.mp3 (hoặc thư mục con)  -> nhạc
/sdcard/videos/*.mp4              -> video (H.264/MJPEG + AAC/MP3)
/sdcard/photos/*.jpg              -> ảnh (JPEG baseline, nên <= 1600x1200)
```

## Điều khiển
| Thao tác | Online | Offline |
|---|---|---|
| Giữ Vol+ (1.5 s) | vào offline | thoát offline |
| Giữ Vol+ lúc cấp nguồn | khởi động thẳng offline (không WiFi) | |
| Touch chạm 1 lần | bật/tắt chat AI | phát / tạm dừng (ảnh: bật/tắt slideshow) |
| Touch chạm đúp | – | đổi nguồn: Nhạc → Video → Ảnh |
| Touch giữ | – | đổi âm lượng 30/50/70/90 |
| Vol+ | tăng âm lượng | bài / video / ảnh kế tiếp |
| Vol- | giảm âm lượng | bài / video / ảnh trước |

Build: `idf.py set-target esp32s3 && idf.py menuconfig` → *Board Type* = "ESP32-S3 N16R8 Offline Media", rồi `idf.py build flash monitor`.
