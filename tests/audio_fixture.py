import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def wav(root, filename, *samples):
    path = root / filename
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
        output.writeframes(struct.pack(f"<{len(samples)}h", *samples))


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as fixture:
        root = Path(fixture)
        wav(root, "parts/welcome.before.wav", 1000)
        wav(root, "stations/sample.wav", 2000)
        wav(root, "parts/welcome.after.wav", 3000, 3000)
        wav(root, "parts/next.before.wav", 1000, 1000)
        wav(root, "parts/finish.before.wav", 1000)
        wav(root, "stations/long.wav", *([3000] * (15 * 22050 + 1)))
        (root / "stations/broken.wav").write_bytes(b"RIFF")
        wav(root, "welcome.wav", 4000)
        wav(root, "finish.wav", 4000)
        wav(root, "parts/terminal_soon.before.wav", 1200)
        wav(root, "parts/terminal_soon.after.wav", 3200)
        wav(root, "terminal_soon.wav", 3600)
        wav(root, "welcome.custom.wav", 5000)
        wav(root, "parts/approaching.before.wav", 1000)
        wav(root, "parts/approaching.between.wav", 3000)
        wav(root, "parts/approaching.after.wav", 4000)
        wav(root, "chime.wav", 1000)
        wav(root, "de/next.wav", 2000)
        wav(root, "en/next.wav", 1000, 3000)
        wav(root, "zh/next.wav", 4000)
        wav(root, "ja/next.wav", 5000)
        wav(root, "de/terminal_soon.wav", 2000)
        wav(root, "en/terminal_soon.wav", 3000)
        wav(root, "zh/terminal_soon.wav", 4000)
        wav(root, "ja/approaching.wav", 6000)
        subprocess.run([sys.argv[1], fixture, sys.argv[2]], check=True)
