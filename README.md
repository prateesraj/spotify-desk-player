# Spotify Desk Player
```html
<p align="center">
  <img src="1000076952.jpg"
       alt="Spotify Desk Player project demo"
       width="650">
</p>
```

I enjoy listening to music on Spotify, and I wanted to display the album cover, song name, and artist name on a small screen on my desk. So I built a Spotify Desk Player using an ESP32 and a TFT display.

The project connects a Python script running on my laptop to an ESP32, which displays the current music information and album artwork on the screen.

## Features

- Displays the currently playing song and artist name
- Shows album artwork on a TFT display
- Uses Python to retrieve music information from Windows media controls
- Transfers music information and album artwork to the ESP32 through serial communication
- Displays the information on a compact 1.8-inch screen

## Hardware

- ESP32 development board
- 1.8-inch TFT display (160 × 128)
- Jumper wires
- USB cable for serial communication

## TFT Display Connections

| TFT Pin | ESP32 Pin |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SCK / CLK | GPIO 18 |
| MOSI / SDA | GPIO 23 |
| CS | GPIO 4 |
| DC / A0 | GPIO 27 |
| RST / RES | GPIO 26 |
| BL / LED | 3.3V |

These pin assignments are based on the ESP32 code. Check that your physical wiring matches them.

## Software and Technologies

- C++
- Python
- ESP32
- Arduino IDE
- Adafruit GFX Library
- Adafruit ST7735 Library
- PySerial
- Pillow
- Windows media controls

## How It Works

1. The Python script reads the current music information from Windows media controls.
2. It retrieves the song title, artist name, album name, playback status, and timing information.
3. The album artwork is converted into RGB565 pixel data suitable for the TFT display.
4. The Python script sends the music information and artwork to the ESP32 through serial communication.
5. The ESP32 processes the received data and displays the information on the TFT screen.

## Setup

1. Install the Arduino IDE and ESP32 board support.
2. Install the required Arduino libraries.
3. Install Python and the dependencies required by the Python script.
4. Connect the TFT display to the ESP32 using the pin table above.
5. Upload the ESP32 sketch using the Arduino IDE.
6. Connect the ESP32 to the laptop through USB.
7. Configure the correct COM port in the Python script.
8. Start Spotify and run the Python script.

## Project Files

- `spotify_desk_player.ino` — ESP32 code written in C++.
- `spotify_controller.py` — Python script that reads music information and communicates with the ESP32.

## Author

Prateesraj K

Built as a personal project to explore embedded systems, Python, and hardware–software integration.
