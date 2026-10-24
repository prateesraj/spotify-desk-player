#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>


String timeText(uint32_t seconds);
uint32_t currentElapsedMilliseconds();
uint32_t currentElapsedSeconds();
uint32_t crc32Byte(uint32_t crc, uint8_t value);
void drawPlaceholderArt();
void drawStaticUi();
void drawScrollingLine(const String &text, int y, uint16_t color, uint16_t phase);
void drawTextPanel();
void drawProgress(bool force);
bool splitNow(const String &command, String fields[7]);
void beginAlbumArt(uint32_t checksum);
void finishAlbumArt();
void processCommand(String command);

constexpr uint32_t SERIAL_BAUD = 460800;
constexpr int TFT_SCK = 18;
constexpr int TFT_MOSI = 23;
constexpr int TFT_CS = 4;
constexpr int TFT_DC = 27;
constexpr int TFT_RST = 26;

constexpr int SCREEN_W = 160;
constexpr int SCREEN_H = 128;
constexpr int ART_X = 5;
constexpr int ART_Y = 6;
constexpr uint16_t ART_W = 68;
constexpr uint16_t ART_H = 68;
constexpr size_t ART_BYTES = ART_W * ART_H * 2;
constexpr int INFO_X = 80;
constexpr int INFO_W = 75;
constexpr int INFO_H = 13;
constexpr int ALBUM_Y = 7;
constexpr int TITLE_Y = 29;
constexpr int ARTIST_Y = 51;

const uint16_t BG = 0x0861;
const uint16_t GREEN = 0x1FE6;
const uint16_t TEXT = ST77XX_WHITE;
const uint16_t MUTED = 0x9CD3;
const uint16_t BAR_BG = 0x3186;
const uint16_t PANEL = 0x10A3;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
GFXcanvas16 infoCanvas(INFO_W, INFO_H);

String trackTitle = "Waiting for music";
String artistName = "Open Spotify on the laptop";
String albumName = "No track received";
String serialLine;
uint32_t packetElapsedSeconds = 0;
uint32_t durationSeconds = 1;
uint32_t packetReceivedMs = 0;
uint32_t lastProgressDrawMs = 0;
uint32_t lastTextDrawMs = 0;
bool playing = false;

uint16_t *albumArt = nullptr;
size_t artByteIndex = 0;
bool receivingArt = false;
bool pendingArt = false;
uint16_t scrollOffset = 0;
uint32_t expectedArtCrc = 0;
uint32_t receivedArtCrc = 0xFFFFFFFFUL;
int lastBarPixel = -1;
uint32_t lastShownSecond = UINT32_MAX;

String timeText(uint32_t seconds) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%lu:%02lu", seconds / 60, seconds % 60);
  return String(buffer);
}

uint32_t currentElapsedMilliseconds() {
  uint64_t elapsed = static_cast<uint64_t>(packetElapsedSeconds) * 1000ULL;
  if (playing) elapsed += millis() - packetReceivedMs;
  const uint64_t maximum = static_cast<uint64_t>(durationSeconds) * 1000ULL;
  return static_cast<uint32_t>(min(elapsed, maximum));
}

uint32_t currentElapsedSeconds() {
  return currentElapsedMilliseconds() / 1000UL;
}

uint32_t crc32Byte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
  }
  return crc;
}

void drawPlaceholderArt() {
  tft.fillRoundRect(ART_X, ART_Y, ART_W, ART_H, 6, 0x4015);
  tft.fillCircle(ART_X + 34, ART_Y + 34, 25, 0x6A7D);
  tft.fillCircle(ART_X + 34, ART_Y + 34, 17, 0x1022);
  tft.fillCircle(ART_X + 34, ART_Y + 34, 5, GREEN);
  tft.drawRoundRect(ART_X - 1, ART_Y - 1, ART_W + 2, ART_H + 2, 6, PANEL);
}

void drawStaticUi() {
  tft.fillScreen(BG);
  drawPlaceholderArt();
  tft.drawRoundRect(5, 96, 150, 26, 5, PANEL);
}

void drawScrollingLine(const String &text, int y, uint16_t color, uint16_t phase) {
  infoCanvas.fillScreen(BG);
  infoCanvas.setTextWrap(false);
  infoCanvas.setTextSize(1);
  infoCanvas.setTextColor(color);
  const int textPixels = text.length() * 6;
  if (textPixels <= INFO_W) {
    infoCanvas.setCursor(0, 2);
    infoCanvas.print(text);
  } else {
    const int loopPixels = textPixels + 18;
    const int offset = phase % loopPixels;
    infoCanvas.setCursor(-offset, 2);
    infoCanvas.print(text);
    infoCanvas.setCursor(loopPixels - offset, 2);
    infoCanvas.print(text);
  }
  tft.drawRGBBitmap(INFO_X, y, infoCanvas.getBuffer(), INFO_W, INFO_H);
}

void drawTextPanel() {
  drawScrollingLine(albumName, ALBUM_Y, GREEN, scrollOffset / 2);
  drawScrollingLine(trackTitle, TITLE_Y, TEXT, scrollOffset);
  drawScrollingLine(artistName, ARTIST_Y, MUTED, (scrollOffset * 3) / 4);
}

void drawProgress(bool force = false) {
  const uint32_t elapsedMs = currentElapsedMilliseconds();
  const uint32_t durationMs = max(1UL, durationSeconds * 1000UL);
  const int barPixels = (148UL * elapsedMs) / durationMs;
  const uint32_t shownSecond = currentElapsedSeconds();

  // This bar is 148 physical pixels wide. On a 7-minute song one new pixel is
  // correctly reached about every 2.9 seconds; the time labels show the exact
  // second between those unavoidable physical-pixel steps.
  if (force || barPixels != lastBarPixel) {
    tft.fillRoundRect(6, 101, 148, 5, 2, BAR_BG);
    if (barPixels > 0) tft.fillRoundRect(6, 101, min(barPixels, 148), 5, 2, GREEN);
    // A bright playhead makes the exact current edge easy to see.
    if (barPixels < 148) tft.drawFastVLine(6 + barPixels, 100, 7, ST77XX_WHITE);
    lastBarPixel = barPixels;
  }

  if (force || shownSecond != lastShownSecond) {
    tft.fillRect(7, 109, 146, 11, PANEL);
    tft.setTextSize(1);
    tft.setTextColor(playing ? GREEN : MUTED);
    tft.setCursor(8, 110);
    tft.print(playing ? ">" : "||");
    tft.setTextColor(MUTED);
    tft.setCursor(28, 110);
    tft.print(timeText(shownSecond));
    tft.setCursor(116, 110);
    tft.print(timeText(durationSeconds));
    lastShownSecond = shownSecond;
  }
}

bool splitNow(const String &command, String fields[7]) {
  int field = 0;
  int start = 0;
  for (int i = 0; i <= command.length() && field < 7; ++i) {
    if (i == command.length() || command.charAt(i) == '|') {
      fields[field++] = command.substring(start, i);
      start = i + 1;
    }
  }
  return field == 7;
}

void beginAlbumArt(uint32_t checksum) {
  if (albumArt == nullptr) albumArt = static_cast<uint16_t *>(malloc(ART_BYTES));
  if (albumArt == nullptr) {
    Serial.println("ERR|ART_MEMORY");
    return;
  }
  artByteIndex = 0;
  expectedArtCrc = checksum;
  receivedArtCrc = 0xFFFFFFFFUL;
  receivingArt = true;
  // The helper waits for this before it sends raw bytes. It prevents command
  // text and image bytes from ever being mixed together.
  Serial.println("OK|ART_READY");
}

void finishAlbumArt() {
  receivingArt = false;
  const uint32_t actualCrc = receivedArtCrc ^ 0xFFFFFFFFUL;
  if (expectedArtCrc != 0 && actualCrc != expectedArtCrc) {
    Serial.printf("ERR|ART_CRC|%08lX\n", actualCrc);
    return; // Preserve the old good cover instead of drawing corrupt stripes.
  }
  pendingArt = true;
  tft.drawRGBBitmap(ART_X, ART_Y, albumArt, ART_W, ART_H);
  tft.drawRoundRect(ART_X - 1, ART_Y - 1, ART_W + 2, ART_H + 2, 6, PANEL);
  pendingArt = false;
  Serial.println("OK|ART_SHOWN");
}

void processCommand(String command) {
  command.trim();
  if (command.startsWith("ART|68|68")) {
    const int separator = command.lastIndexOf('|');
    const uint32_t checksum = separator > 8 ? strtoul(command.substring(separator + 1).c_str(), nullptr, 16) : 0;
    beginAlbumArt(checksum);
    return;
  }
  if (!command.startsWith("NOW|")) return;

  String fields[7];
  if (!splitNow(command, fields)) return;
  const bool hasRealTrack = fields[1].length() > 0 && !fields[1].equalsIgnoreCase("Nothing playing") && !fields[1].equalsIgnoreCase("Unknown song");
  const bool metadataChanged = hasRealTrack && (fields[1] != trackTitle || fields[2] != artistName || fields[3] != albumName);

  if (hasRealTrack) {
    trackTitle = fields[1];
    artistName = fields[2].length() ? fields[2] : "Unknown artist";
    albumName = fields[3].length() ? fields[3] : "Unknown album";
  }
  packetElapsedSeconds = max(0L, fields[4].toInt());
  durationSeconds = max(1L, fields[5].toInt());
  packetReceivedMs = millis();
  playing = fields[6].equalsIgnoreCase("PLAYING") || fields[6] == "1";

  if (metadataChanged) {
    scrollOffset = 0;
    if (!pendingArt) drawPlaceholderArt();
    drawTextPanel();
  }
  drawProgress(true);
}

void setup() {
  Serial.setRxBufferSize(16384);
  Serial.begin(SERIAL_BAUD);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  drawStaticUi();
  drawTextPanel();
  drawProgress(true);
  Serial.println("READY|460800|ART_CRC");
}

void loop() {
  while (Serial.available()) {
    const uint8_t incoming = static_cast<uint8_t>(Serial.read());
    if (receivingArt) {
      albumArt[artByteIndex / 2] = (artByteIndex % 2 == 0)
          ? static_cast<uint16_t>(incoming) << 8
          : albumArt[artByteIndex / 2] | incoming;
      receivedArtCrc = crc32Byte(receivedArtCrc, incoming);
      ++artByteIndex;
      if (artByteIndex == ART_BYTES) finishAlbumArt();
      continue;
    }
    if (incoming == '\n' || incoming == '\r') {
      if (serialLine.length()) {
        processCommand(serialLine);
        serialLine = "";
      }
    } else if (serialLine.length() < 240) {
      serialLine += static_cast<char>(incoming);
    }
  }

  if (receivingArt) return;

  const uint32_t now = millis();
  if (now - lastProgressDrawMs >= 50) {
    lastProgressDrawMs = now;
    drawProgress();
  }
  if (now - lastTextDrawMs >= 55) {
    lastTextDrawMs = now;
    ++scrollOffset;
    drawTextPanel();
  }
}
