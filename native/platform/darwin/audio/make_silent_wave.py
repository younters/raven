import sys
import wave

with wave.open(sys.argv[1], "wb") as sound:
    sound.setnchannels(2)
    sound.setsampwidth(2)
    sound.setframerate(24000)
    sound.writeframes(bytes(2400 * 4))
