from flask import Flask, request, send_file, jsonify
import requests
import io
import whisper
import traceback
import numpy as np
import soundfile as sf
from gtts import gTTS
from pydub import AudioSegment

app = Flask(__name__)

GROQ_API_URL = "https://api.groq.com/openai/v1/chat/completions"
GROQ_API_KEY = "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"
GROQ_MODEL = "meta-llama/llama-4-scout-17b-16e-instruct"

whisper_model = whisper.load_model("medium")

@app.route("/upload_audio", methods=["POST"])
def upload_audio():
    try:
        raw_audio = request.data
        input_pcm = io.BytesIO(raw_audio)

        audio_np, _ = sf.read(
            input_pcm,
            dtype='int16',
            channels=1,
            samplerate=16000,
            format='RAW',
            subtype='PCM_16'
        )

        sf.write("input_audio.wav", audio_np, 16000)
        print("[INFO] input_audio.wav kaydedildi")

        result = whisper_model.transcribe("input_audio.wav", language="tr")
        recognized_text = result["text"]
        print(f"[TEXT] Tanınan metin: {recognized_text}")

        headers = {
            "Authorization": f"Bearer {GROQ_API_KEY}",
            "Content-Type": "application/json"
        }
        payload = {
            "model": GROQ_MODEL,
            "messages": [
                {"role": "system", "content": "Kısa ve net Türkçe cevap ver. Gereksiz detay verme."},
                {"role": "user", "content": recognized_text}
            ]
        }
        response = requests.post(GROQ_API_URL, headers=headers, json=payload)
        if response.status_code != 200:
            return jsonify({"error": "LLM hatası", "details": response.text}), 500

        answer = response.json()["choices"][0]["message"]["content"]
        print(f"[LLM] LLM cevabı: {answer}")

        # gTTS ile TTS (Google)
        tts = gTTS(answer, lang='tr')
        tts.save('output_response.mp3')
        print("[INFO] output_response.mp3 oluşturuldu")

        # MP3'ü WAV'a çevir
        audio = AudioSegment.from_mp3("output_response.mp3")
        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
        audio.export("output_response.wav", format="wav")
        print("[INFO] output_response.wav oluşturuldu")

        # WAV -> PCM (ESP32 için)
        audio = AudioSegment.from_wav("output_response.wav")
        audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
        pcm_buf = io.BytesIO()
        audio.export(pcm_buf, format="raw")
        pcm_buf.seek(0)

        return send_file(pcm_buf, mimetype="application/octet-stream")

    except Exception as e:
        print("[ERROR] Hata:", traceback.format_exc())
        return jsonify({"error": str(e)}), 500

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
