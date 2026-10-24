import asyncio
import io
import time
import zlib

import serial
from PIL import Image

from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus,
)
from winrt.windows.storage.streams import DataReader


# Change COM3 only if Windows gives your ESP32 another COM port.
ESP32_PORT = "COM3"
BAUD_RATE = 460800

# Must match the ESP32 code.
ART_W = 68
ART_H = 68
ART_BYTES = ART_W * ART_H * 2

UPDATE_INTERVAL_SECONDS = 1.0
ART_READY_TIMEOUT_SECONDS = 5.0
ART_SHOWN_TIMEOUT_SECONDS = 8.0


def clean_text(value):
    """Keep text safe inside NOW|title|artist|album|... messages."""
    if value is None:
        return ""

    return (
        str(value)
        .replace("|", " ")
        .replace("\r", " ")
        .replace("\n", " ")
        .strip()
    )


def seconds_from_timespan(value):
    """Turn a Windows time value into seconds."""
    if value is None:
        return 0

    if hasattr(value, "total_seconds"):
        return max(0, int(value.total_seconds()))

    try:
        return max(0, int(value.duration / 10_000_000))
    except Exception:
        return 0


def rgb565_big_endian(image):
    """
    Convert an album image to exactly 68 x 68 pixels of RGB565 data.

    The ESP32 expects: high byte, then low byte, for every pixel.
    """
    image = image.convert("RGB").resize(
        (ART_W, ART_H),
        Image.Resampling.LANCZOS,
    )

    output = bytearray(ART_BYTES)
    index = 0

    pixels = (
        image.get_flattened_data()
        if hasattr(image, "get_flattened_data")
        else image.getdata()
    )

    for red, green, blue in pixels:
        rgb565 = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)

        output[index] = (rgb565 >> 8) & 0xFF
        output[index + 1] = rgb565 & 0xFF
        index += 2

    return bytes(output)


async def thumbnail_to_rgb565(thumbnail):
    """
    Read Spotify's thumbnail through Windows media controls.

    The writable bytearray is important: it fixes the earlier
    'a bytes-like object is required, not int' DataReader issue.
    """
    if thumbnail is None:
        return None

    stream = await thumbnail.open_read_async()
    reader = DataReader(stream)

    try:
        await reader.load_async(stream.size)

        raw_buffer = bytearray(reader.unconsumed_buffer_length)
        reader.read_bytes(raw_buffer)

        image = Image.open(io.BytesIO(bytes(raw_buffer)))
        image.load()  # Fully decode before closing the Windows stream.

        return rgb565_big_endian(image)

    finally:
        reader.close()
        stream.close()


def drain_serial_lines(esp32):
    """
    Read every complete reply currently available from the ESP32.

    The ESP32 replies with:
      READY|460800|ART_CRC
      OK|ART_READY
      OK|ART_SHOWN
      ERR|ART_CRC|...
    """
    lines = []

    while esp32.in_waiting:
        raw = esp32.readline()

        if not raw:
            break

        line = raw.decode("utf-8", errors="replace").strip()

        if line:
            lines.append(line)

    return lines


def wait_for_reply(esp32, wanted_reply, timeout_seconds):
    """Wait for one exact reply from the ESP32 without blocking forever."""
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        for line in drain_serial_lines(esp32):
            print(f"ESP32: {line}")

            if line == wanted_reply:
                return True

            if line.startswith("ERR|"):
                return False

        time.sleep(0.01)

    print(f"Timed out waiting for ESP32 reply: {wanted_reply}")
    return False


def send_now(esp32, title, artist, album, elapsed, duration, is_playing):
    """Send timing and metadata. The ESP32 uses this to update the bar."""
    state = "PLAYING" if is_playing else "PAUSED"

    message = (
        f"NOW|{clean_text(title)}|{clean_text(artist)}|{clean_text(album)}|"
        f"{elapsed}|{duration}|{state}\n"
    )

    esp32.write(message.encode("utf-8", errors="replace"))
    esp32.flush()


def send_cover_with_handshake(esp32, cover_bytes):
    """
    Send one verified cover.

    Order matters:
    1. Send header including CRC32.
    2. Wait for OK|ART_READY.
    3. Send exactly 9,248 raw image bytes.
    4. Wait for OK|ART_SHOWN.

    This prevents cover bytes being confused with text commands and prevents
    corrupt striped artwork from being drawn.
    """
    if cover_bytes is None:
        print("Cover skipped: Spotify did not provide cover data.")
        return False

    if len(cover_bytes) != ART_BYTES:
        print(
            f"Cover skipped: expected {ART_BYTES} bytes, "
            f"received {len(cover_bytes)} bytes."
        )
        return False

    checksum = zlib.crc32(cover_bytes) & 0xFFFFFFFF
    header = f"ART|{ART_W}|{ART_H}|{checksum:08X}\n"

    # Remove old replies before this new transfer begins.
    drain_serial_lines(esp32)

    esp32.write(header.encode("ascii"))
    esp32.flush()

    if not wait_for_reply(esp32, "OK|ART_READY", ART_READY_TIMEOUT_SECONDS):
        print("Cover was not sent because the ESP32 was not ready.")
        return False

    esp32.write(cover_bytes)
    esp32.flush()

    if not wait_for_reply(esp32, "OK|ART_SHOWN", ART_SHOWN_TIMEOUT_SECONDS):
        print("Cover transfer failed or the ESP32 rejected its checksum.")
        return False

    return True


async def get_spotify_session(manager):
    """
    Return Spotify's current Windows media-control session.

    If another app becomes Windows' active media app, this returns None and
    keeps the last good track shown on the ESP32.
    """
    session = manager.get_current_session()

    if session is None:
        return None

    app_id = (session.source_app_user_model_id or "").lower()

    if "spotify" not in app_id:
        return None

    return session


async def main():
    print(f"Opening {ESP32_PORT} at {BAUD_RATE} baud...")

    with serial.Serial(
        ESP32_PORT,
        BAUD_RATE,
        timeout=0.05,
        write_timeout=10,
    ) as esp32:
        # Opening the port may reset the ESP32.
        time.sleep(2.0)
        esp32.reset_input_buffer()
        esp32.reset_output_buffer()

        print("Connected. Waiting for the ESP32 startup message...")

        startup_deadline = time.monotonic() + 5.0
        while time.monotonic() < startup_deadline:
            for line in drain_serial_lines(esp32):
                print(f"ESP32: {line}")

                if line == "READY|460800|ART_CRC":
                    print("ESP32 display protocol is ready.")

            time.sleep(0.01)

        print("Start or resume a Spotify track.")

        manager = await (
            GlobalSystemMediaTransportControlsSessionManager.request_async()
        )

        # Only set after the display confirms that the cover was drawn.
        last_successful_cover_track_key = None

        while True:
            try:
                # Print unexpected board messages, such as a CRC failure.
                for line in drain_serial_lines(esp32):
                    print(f"ESP32: {line}")

                session = await get_spotify_session(manager)

                if session is None:
                    await asyncio.sleep(UPDATE_INTERVAL_SECONDS)
                    continue

                properties = await session.try_get_media_properties_async()
                timeline = session.get_timeline_properties()
                playback = session.get_playback_info()

                title = clean_text(properties.title)
                artist = clean_text(properties.artist)
                album = clean_text(properties.album_title)

                # Do not overwrite the display with Spotify's temporary blank
                # values during a song change.
                if not title or title.lower() in (
                    "nothing playing",
                    "unknown song",
                ):
                    await asyncio.sleep(UPDATE_INTERVAL_SECONDS)
                    continue

                if not artist:
                    artist = "Unknown artist"

                if not album:
                    album = "Unknown album"

                elapsed = seconds_from_timespan(timeline.position)
                duration = seconds_from_timespan(timeline.end_time)

                if duration <= 0:
                    duration = max(1, elapsed)

                is_playing = (
                    playback.playback_status
                    == GlobalSystemMediaTransportControlsSessionPlaybackStatus.PLAYING
                )

                # Metadata goes first. It keeps the displayed time and fill
                # position aligned with Spotify once per second.
                send_now(
                    esp32,
                    title,
                    artist,
                    album,
                    elapsed,
                    duration,
                    is_playing,
                )

                state_text = "PLAYING" if is_playing else "PAUSED"
                print(
                    f"{title} — {artist} "
                    f"[{state_text}] {elapsed}/{duration}"
                )

                # Only transmit the 9,248-byte image after a song/album change.
                track_key = f"{title}\x1f{artist}\x1f{album}"

                if track_key != last_successful_cover_track_key:
                    try:
                        cover_bytes = await thumbnail_to_rgb565(
                            properties.thumbnail
                        )

                        if send_cover_with_handshake(esp32, cover_bytes):
                            last_successful_cover_track_key = track_key
                            print(f"Sent verified cover: {album}")
                        else:
                            print(
                                "Keeping the previous cover; "
                                "the helper will retry in one second."
                            )

                    except Exception as error:
                        print(f"Cover skipped: {error}")

                await asyncio.sleep(UPDATE_INTERVAL_SECONDS)

            except serial.SerialException as error:
                print(f"Serial error: {error}")
                print(
                    "Close Schematik or another program using "
                    f"{ESP32_PORT}, then start this helper again."
                )
                return

            except KeyboardInterrupt:
                raise

            except Exception as error:
                # Preserve the last valid display data and retry.
                print(f"Temporary media read error: {error}")
                await asyncio.sleep(UPDATE_INTERVAL_SECONDS)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nSpotify helper stopped.")
