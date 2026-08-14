#pragma once

#include <Arduino.h>
#include <M5GFX.h>

// iKun M5 Monitor stream v1
// Browser -> device: IKUNMIRROR ON\n / IKUNMIRROR OFF\n
class iKunM5Mirror {
 public:
  // 4 FPS 足够直播像素 UI，同时不给按键、声音和动画主循环制造串口积压。
  explicit iKunM5Mirror(uint16_t frameIntervalMs = 250,
                         uint32_t maxPayloadBytes = 12000)
      : frameIntervalMs_(frameIntervalMs), maxPayloadBytes_(maxPayloadBytes) {}

  void begin(Stream& serial) { serial_ = &serial; }

  bool consumeEnabledEvent() {
    const bool received = enabledEvent_;
    enabledEvent_ = false;
    return received;
  }

  void poll() {
    if (!serial_) return;
    while (serial_->available()) {
      char incoming = static_cast<char>(serial_->read());
      if (incoming == '\r') continue;
      if (incoming == '\n') {
        handleCommand();
        command_ = "";
      } else if (command_.length() < 40) {
        command_ += incoming;
      }
    }
  }

  void capture(M5Canvas& canvas) {
    if (!enabled_ || !serial_) return;
    const uint32_t now = millis();
    if (now - lastFrameMs_ < frameIntervalMs_) return;

    const int width = canvas.width();
    const int height = canvas.height();
    if (width <= 0 || height <= 0 || width > 255 || height > 255) return;

    const FrameInfo frame = inspectFrame(canvas, width, height);
    if (frame.payloadBytes == 0 || frame.payloadBytes > maxPayloadBytes_) {
      // 复杂图像直接丢帧，不排队，避免影响设备交互。
      lastFrameMs_ = now;
      return;
    }
    if (hasLastFrame_ && frame.hash == lastFrameHash_) {
      lastFrameMs_ = now;
      return;
    }

    static const uint8_t magic[] = {'I', 'K', 'M', 'F'};
    serial_->write(magic, sizeof(magic));
    serial_->write(static_cast<uint8_t>(1));
    writeU16(static_cast<uint16_t>(width));
    writeU16(static_cast<uint16_t>(height));
    serial_->write(static_cast<uint8_t>(frame.payloadBytes & 0xFF));
    serial_->write(static_cast<uint8_t>((frame.payloadBytes >> 8) & 0xFF));
    serial_->write(static_cast<uint8_t>((frame.payloadBytes >> 16) & 0xFF));
    writePayload(canvas, width, height);
    lastFrameHash_ = frame.hash;
    hasLastFrame_ = true;
    lastFrameMs_ = now;
  }

 private:
  struct FrameInfo { uint32_t payloadBytes; uint32_t hash; };

  Stream* serial_ = nullptr;
  String command_;
  bool enabled_ = false;
  bool enabledEvent_ = false;
  uint16_t frameIntervalMs_;
  uint32_t maxPayloadBytes_;
  uint32_t lastFrameMs_ = 0;
  uint32_t lastFrameHash_ = 0;
  bool hasLastFrame_ = false;

  void handleCommand() {
    if (command_ == "IKUNMIRROR ON") {
      enabled_ = true;
      enabledEvent_ = true;
      // 文本握手只用于排障；随后仍是二进制 IKMF 帧。
      serial_->print("IKUNREADY\n");
    }
    if (command_ == "IKUNMIRROR OFF") enabled_ = false;
  }

  void writeU16(uint16_t value) {
    serial_->write(static_cast<uint8_t>(value & 0xFF));
    serial_->write(static_cast<uint8_t>(value >> 8));
  }

  FrameInfo inspectFrame(M5Canvas& canvas, int width, int height) {
    uint32_t runs = 0;
    uint32_t hash = 2166136261UL;
    for (int y = 0; y < height; y++) {
      int x = 0;
      while (x < width) {
        const uint16_t color = canvas.readPixel(x, y);
        uint8_t count = 1;
        while (x + count < width && count < 255 && canvas.readPixel(x + count, y) == color) count++;
        hash = (hash ^ count) * 16777619UL;
        hash = (hash ^ static_cast<uint8_t>(color & 0xFF)) * 16777619UL;
        hash = (hash ^ static_cast<uint8_t>(color >> 8)) * 16777619UL;
        runs++;
        x += count;
      }
    }
    return {runs * 3, hash};
  }

  void writePayload(M5Canvas& canvas, int width, int height) {
    for (int y = 0; y < height; y++) {
      int x = 0;
      while (x < width) {
        const uint16_t color = canvas.readPixel(x, y);
        uint8_t count = 1;
        while (x + count < width && count < 255 && canvas.readPixel(x + count, y) == color) count++;
        serial_->write(count);
        writeU16(color);
        x += count;
      }
    }
  }
};
