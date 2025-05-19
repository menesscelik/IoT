from flask import Flask, request, send_from_directory, make_response, jsonify
from gtts import gTTS
from pydub import AudioSegment
import wave, os, uuid, requests
import pandas as pd
from datetime import datetime
import json
import io

app = Flask(__name__)
UPLOAD_FOLDER = "audios"
LOG_FILE = "access_logs.csv"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# Server Configuration
SERVER_IP = "192.168.137.44"
SERVER_PORT = 5000

# API Configuration
GROQ_API_KEY = "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"
WHISPER_MODEL = "whisper-large-v3"
CHAT_MODEL = "llama3-8b-8192"

# Aktif kayıtları tutmak için sözlük
active_recordings = {}

def get_log_context():
    try:
        df = pd.read_csv(LOG_FILE)
        # Son 10 kaydı al ve string formatına çevir
        recent_logs = df.tail(10).to_string()
        return f"""Bu bir giriş-çıkış kayıt sistemidir. Aşağıdaki kayıtlar son giriş-çıkışları göstermektedir:\n\n{recent_logs}\n\nLütfen yukarıdaki kayıtları kullanarak soruyu yanıtla. Yanıtını Türkçe ve doğal bir dille ver."""
    except Exception as e:
        print(f"Log dosyası okuma hatası: {str(e)}")
        return "Kayıt dosyası okunamadı."

def get_user_info_by_id(user_id):
    # Bu fonksiyon artık kullanılmıyor, basit bir hata mesajı döndürelim
    return "ID sorgusu desteklenmiyor. Lütfen isim ile sorgulama yapın."

def is_log_related_query(text):
    # Log ile ilgili anahtar kelime kombinasyonlarını kontrol et
    text_lower = text.lower()
    
    # Anahtar kelime grupları
    primary_keywords = ["giriş", "çıkış", "kayıt"]
    secondary_keywords = ["kim", "ne zaman", "saat", "kişi", "kaç", "son"]
    
    # En az bir primary keyword ve bir secondary keyword olmalı
    has_primary = any(keyword in text_lower for keyword in primary_keywords)
    has_secondary = any(keyword in text_lower for keyword in secondary_keywords)
    
    # Özel durumlar için tam kalıplar
    exact_patterns = [
        "kim geldi",
        "kim gitti",
        "gelen kim",
        "giden kim",
        "son giriş",
        "son kayıt"
    ]
    
    has_exact_match = any(pattern in text_lower for pattern in exact_patterns)
    
    return (has_primary and has_secondary) or has_exact_match, None

@app.route("/upload", methods=["POST"])
def upload():
    try:
        print(f"[IN] Upload request - Content-Length: {request.content_length} bytes")
        print(f"[IN] Headers: {dict(request.headers)}")
        
        # İstek başlıklarını kontrol et
        is_first_chunk = request.headers.get('X-First-Chunk', 'false').lower() == 'true'
        is_last_chunk = request.headers.get('X-Last-Chunk', 'false').lower() == 'true'
        is_wake_check = request.headers.get('X-Wake-Check', 'false').lower() == 'true'
        session_id = request.headers.get('X-Session-ID', str(uuid.uuid4()))
        
        print(f"[IN] Session: {session_id} | First: {is_first_chunk} | Last: {is_last_chunk} | Wake: {is_wake_check}")
        
        # Eğer JSON olarak sadece text geldiyse, TTS-only mod
        if request.is_json:
            data = request.get_json()
            text = data.get("text", "").strip()
            lang = data.get("lang", "tr")  # Varsayılan olarak Türkçe
            if text:
                print(f"[IN] TTS request - Text: {text} | Lang: {lang}")
                file_id = str(uuid.uuid4())
                mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
                wav_reply_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
                try:
                    gTTS(text, lang=lang).save(mp3_path)
                    audio = AudioSegment.from_mp3(mp3_path)
                    audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
                    raw_pcm = audio.raw_data
                    with wave.open(wav_reply_path, "wb") as wf:
                        wf.setnchannels(1)
                        wf.setsampwidth(2)
                        wf.setframerate(16000)
                        wf.writeframes(raw_pcm)
                    host = request.host_url.rstrip('/')
                    url = f"{host}/audios/{os.path.basename(wav_reply_path)}"
                    print(f"[OUT] TTS response - URL: {url}")
                    resp = make_response(url, 200)
                    resp.headers["Content-Type"] = "text/plain"
                    return resp
                except Exception as e:
                    print(f"[ERROR] TTS generation failed: {str(e)}")
                    return jsonify({"error": str(e)}), 500
        
        # Yeni kayıt oturumu başlat
        if is_first_chunk:
            print(f"[IN] New recording session started: {session_id}")
            active_recordings[session_id] = io.BytesIO()
        
        # Veriyi geçici bellekte biriktir
        if session_id in active_recordings:
            active_recordings[session_id].write(request.data)
            current_size = active_recordings[session_id].tell()
            print(f"[IN] Chunk received - Size: {current_size} bytes")
            
            # Son chunk ise ses işlemeyi başlat
            if is_last_chunk:
                print(f"[IN] Last chunk received - Processing audio...")
                wav_data = active_recordings[session_id].getvalue()
                
                # Geçici dosyaya kaydet
                file_id = str(uuid.uuid4())
                wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.wav")
                
                with open(wav_path, "wb") as f:
                    f.write(wav_data)
                
                print(f"[IN] WAV saved - Size: {os.path.getsize(wav_path)} bytes")
                
                # Whisper API'ye gönder
                headers = {"Authorization": f"Bearer {GROQ_API_KEY}"}
                
                with open(wav_path, "rb") as af:
                    try:
                        asr_res = requests.post(
                            "https://api.groq.com/openai/v1/audio/transcriptions",
                            headers=headers,
                            files={"file": (os.path.basename(wav_path), af, "audio/wav")},
                            data={
                                "model": WHISPER_MODEL,
                                "language": "tr",
                                "response_format": "json"
                            }
                        )
                        asr_res.raise_for_status()
                        text = asr_res.json().get("text", "").strip()
                        print(f"[IN] Whisper transcription: {text}")
                        
                        # Wake word kontrolü ise sadece metni döndür
                        if is_wake_check:
                            resp = make_response(text, 200)
                            resp.headers["Content-Type"] = "text/plain"
                            return resp
                            
                        # Normal asistan işlemine devam et
                        is_log_query, id_match = is_log_related_query(text)
                        
                        if is_log_query:
                            if id_match:
                                user_info = get_user_info_by_id(id_match)
                                reply = user_info
                                print(f"[OUT] User info response: {reply}")
                            else:
                                context = get_log_context()
                                system_prompt = """Senin görevin, sana sunulan giriş ve çıkış verilerini kullanarak soruları yanıtlamak.
Yanıtlarını Türkçe, sade ve herkesin kolayca anlayabileceği bir şekilde vermelisin.
Teknik ayrıntılara fazla girme; doğal, akıcı ve günlük dile yakın bir anlatım tercih et.
Eğer bir soruyu anlamazsan ya da cevaplamak için yeterli bilgiye sahip değilsen, bunu kibarca belirtmen yeterli."""
                                messages = [
                                    {"role": "system", "content": system_prompt},
                                    {"role": "user", "content": context},
                                    {"role": "user", "content": text}
                                ]
                                
                                try:
                                    chat_res = requests.post(
                                        "https://api.groq.com/openai/v1/chat/completions",
                                        headers=headers,
                                        json={
                                            "model": CHAT_MODEL,
                                            "messages": messages,
                                            "temperature": 0.7
                                        }
                                    )
                                    chat_res.raise_for_status()
                                    reply = chat_res.json()["choices"][0]["message"]["content"].strip()
                                    print(f"[OUT] LLM response: {reply}")
                                except Exception as e:
                                    print(f"[ERROR] LLM API error: {str(e)}")
                                    raise
                        else:
                            system_prompt = """Sen, kullanıcılara destek olmak için tasarlanmış bir yardımcı asistansın.
Sana yöneltilen her türlü soruya elinden geldiğince yanıt vermeye çalışacaksın.
Cevaplarını her zaman Türkçe, anlaşılır ve doğal bir dille vereceksin.
Eğer bir konuda bilgin yoksa, bunu açıkça ve dürüstçe söylemen önemli.
Güncel gelişmelerden hava durumuna, genel bilgiden günlük yaşama kadar pek çok konuda yardımcı olmaya çalışacaksın."""
                            messages = [
                                {"role": "system", "content": system_prompt},
                                {"role": "user", "content": text}
                            ]
                            
                            try:
                                chat_res = requests.post(
                                    "https://api.groq.com/openai/v1/chat/completions",
                                    headers=headers,
                                    json={
                                        "model": CHAT_MODEL,
                                        "messages": messages,
                                        "temperature": 0.7
                                    }
                                )
                                chat_res.raise_for_status()
                                reply = chat_res.json()["choices"][0]["message"]["content"].strip()
                                print(f"[OUT] LLM response: {reply}")
                            except Exception as e:
                                print(f"[ERROR] LLM API error: {str(e)}")
                                raise

                    except Exception as e:
                        print(f"[ERROR] Whisper API error: {str(e)}")
                        raise

                # gTTS → MP3 → kesin PCM WAV
                mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
                wav_reply_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
                
                try:
                    gTTS(reply, lang="tr").save(mp3_path)
                    audio = AudioSegment.from_mp3(mp3_path)
                    audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
                    raw_pcm = audio.raw_data

                    with wave.open(wav_reply_path, "wb") as wf:
                        wf.setnchannels(1)
                        wf.setsampwidth(2)
                        wf.setframerate(16000)
                        wf.writeframes(raw_pcm)

                    print(f"[OUT] Response WAV saved - Size: {os.path.getsize(wav_reply_path)} bytes")
                except Exception as e:
                    print(f"[ERROR] Audio synthesis error: {str(e)}")
                    raise

                # Oturum verilerini temizle
                del active_recordings[session_id]
                
                # URL'i dön
                host = request.host_url.rstrip('/')
                url = f"{host}/audios/{os.path.basename(wav_reply_path)}"
                print(f"[OUT] Response URL: {url}")
                resp = make_response(url, 200)
                resp.headers["Content-Type"] = "text/plain"
                return resp
            
            # Ara chunk'lar için OK yanıtı
            return make_response("OK", 200)
            
        else:
            error_msg = "Invalid session ID or no active recording"
            print(f"[ERROR] {error_msg}")
            return jsonify({
                "error": error_msg,
                "details": "No active recording found for this session ID"
            }), 400

    except Exception as e:
        print(f"[ERROR] General error: {str(e)}")
        import traceback
        print(f"[ERROR] Stack trace:\n{traceback.format_exc()}")
        
        # Hata durumunda oturum verilerini temizle
        if 'session_id' in locals() and session_id in active_recordings:
            del active_recordings[session_id]
            
        return jsonify({
            "error": "An error occurred during processing",
            "details": str(e)
        }), 500

@app.route("/audios/<filename>")
def serve_audio(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)



@app.route("/register_user", methods=["POST"])
def register_user():
    try:
        data = request.get_json()
        print(f"[IN] Register request - Data: {data}")

        name = data.get("name", "").strip()
        password = data.get("password", "").strip()

        if not name or not password:
            return jsonify({"status": "error", "message": "Name and password required."}), 400

        users_file = "users.csv"

        # CSV dosyası yoksa oluştur
        if not os.path.exists(users_file):
            df = pd.DataFrame(columns=["Name", "Password"])
            df.to_csv(users_file, index=False)

        # CSV'yi yükle
        df = pd.read_csv(users_file)

        # Kullanıcı zaten varsa → sesli uyarı ver ve ana menüye dön
        if not df[df["Name"] == name].empty:
            print(f"[INFO] User {name} already exists, sending voice warning")
            
            # Sesli uyarı için TTS
            warning_text = f"{name} isimli kullanıcı zaten kayıtlı. Ana menüye dönülüyor."
            file_id = str(uuid.uuid4())
            mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
            wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
            
            try:
                gTTS(warning_text, lang="tr").save(mp3_path)
                audio = AudioSegment.from_mp3(mp3_path)
                audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
                with wave.open(wav_path, "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(16000)
                    wf.writeframes(audio.raw_data)
                host = request.host_url.rstrip('/')
                url = f"{host}/audios/{os.path.basename(wav_path)}"
                print(f"[OUT] Warning audio URL: {url}")
                
                return jsonify({
                    "status": "exists",
                    "message": "User already exists",
                    "name": name,
                    "audio_url": url
                }), 200
                
            except Exception as e:
                print(f"[ERROR] Audio warning generation failed: {str(e)}")
                return jsonify({
                    "status": "exists",
                    "message": "User already exists",
                    "name": name
                }), 200

        # Yeni kullanıcı olarak ekle
        df = pd.concat([df, pd.DataFrame([{"Name": name, "Password": password}])], ignore_index=True)
        df.to_csv(users_file, index=False)
        
        # Başarılı kayıt için sesli onay
        success_text = f"{name} başarıyla kaydedildi."
        file_id = str(uuid.uuid4())
        mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
        wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
        
        try:
            gTTS(success_text, lang="tr").save(mp3_path)
            audio = AudioSegment.from_mp3(mp3_path)
            audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(audio.raw_data)
            host = request.host_url.rstrip('/')
            url = f"{host}/audios/{os.path.basename(wav_path)}"
            print(f"[OUT] Success audio URL: {url}")
            
            return jsonify({
                "status": "success",
                "message": "New user registered",
                "name": name,
                "audio_url": url
            }), 200
            
        except Exception as e:
            print(f"[ERROR] Success audio generation failed: {str(e)}")
            return jsonify({
                "status": "success",
                "message": "New user registered",
                "name": name
            }), 200

    except Exception as e:
        print(f"[ERROR] Register error: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500


@app.route("/check_user", methods=["POST"])
def check_user():
    try:
        data = request.get_json()
        name = data.get("name", "").strip()
        users_file = "users.csv"
        if not name or not os.path.exists(users_file):
            return "NOT_FOUND", 200
        df = pd.read_csv(users_file)
        if not df[df["Name"] == name].empty:
            return "OK", 200
        else:
            return "NOT_FOUND", 200
    except Exception as e:
        return "NOT_FOUND", 200

@app.route("/verify_user", methods=["POST"])
def verify_user():
    try:
        data = request.get_json()
        name = data.get("name", "").strip()
        password = data.get("password", "").strip()
        print(f"[IN] Verify request - User: {name}")
        
        if not name or not password:
            return jsonify({"status": "error", "message": "Name and password required."}), 400
            
        users_file = "users.csv"
        if not os.path.exists(users_file):
            return jsonify({"status": "error", "message": "User not found."}), 400
            
        df = pd.read_csv(users_file)
        df["Password"] = df["Password"].astype(str).str.strip()
        match = df[(df["Name"] == name) & (df["Password"] == password)]
        
        if not match.empty:
            # Log kaydı
            log_file = "access_logs.csv"
            now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log_df = pd.DataFrame([{ "Name": name, "Timestamp": now }])
            if not os.path.exists(log_file):
                log_df.to_csv(log_file, index=False)
            else:
                log_df.to_csv(log_file, mode='a', header=False, index=False)
            print(f"[OUT] Verify response - Status: success | User: {name}")
            return jsonify({"status": "success", "message": "Login successful.", "name": name}), 200
        else:
            print(f"[OUT] Verify response - Status: error | User: {name}")
            return jsonify({"status": "error", "message": "Invalid password or user not found."}), 401
    except Exception as e:
        print(f"[ERROR] Verify error: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route("/last_login", methods=["GET"])
def last_login():
    log_file = "access_logs.csv"
    if not os.path.exists(log_file):
        return jsonify({"message": "No records"}), 200
    try:
        df = pd.read_csv(log_file)
        if df.empty:
            return jsonify({"message": "No records"}), 200

        last_name = df.iloc[-1]["Name"]
        tts_text = f"Son giriş yapan kişi: {last_name}"
        print(f"[IN] Last login request - Last user: {last_name}")

        # Sesli yanıt üret
        file_id = str(uuid.uuid4())
        mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
        wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
        try:
            gTTS(tts_text, lang="tr").save(mp3_path)
            audio = AudioSegment.from_mp3(mp3_path)
            audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(audio.raw_data)
            host = request.host_url.rstrip('/')
            url = f"{host}/audios/{os.path.basename(wav_path)}"
            print(f"[OUT] Last login response - URL: {url}")

            return jsonify({
                "name": last_name,
                "url": url
            }), 200
        except Exception as e:
            print(f"[ERROR] Audio synthesis error: {str(e)}")
            return jsonify({"name": last_name}), 200
    except Exception as e:
        print(f"[ERROR] Last login error: {str(e)}")
        return jsonify({"message": f"Error: {str(e)}"}), 500

@app.route("/confirm_name", methods=["POST"])
def confirm_name():
    try:
        data = request.get_json()
        name = data.get("name", "").strip()
        
        if not name:
            return jsonify({"status": "error", "message": "Name is required."}), 400
            
        # İsim onayı için sesli mesaj oluştur
        confirm_text = f"Algılanan isim: {name}. Bu ismi onaylıyor musunuz?"
        file_id = str(uuid.uuid4())
        mp3_path = os.path.join(UPLOAD_FOLDER, f"{file_id}.mp3")
        wav_path = os.path.join(UPLOAD_FOLDER, f"{file_id}_reply.wav")
        
        try:
            gTTS(confirm_text, lang="tr").save(mp3_path)
            audio = AudioSegment.from_mp3(mp3_path)
            audio = audio.set_frame_rate(16000).set_channels(1).set_sample_width(2)
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(16000)
                wf.writeframes(audio.raw_data)
            host = request.host_url.rstrip('/')
            url = f"{host}/audios/{os.path.basename(wav_path)}"
            
            return jsonify({
                "status": "success",
                "message": "Name confirmation audio generated",
                "audio_url": url
            }), 200
            
        except Exception as e:
            print(f"[ERROR] Name confirmation audio generation failed: {str(e)}")
            return jsonify({"status": "error", "message": str(e)}), 500
            
    except Exception as e:
        print(f"[ERROR] Name confirmation error: {str(e)}")
        return jsonify({"status": "error", "message": str(e)}), 500

    
if __name__ == "__main__":
    print(f"[INFO] Server starting on http://{SERVER_IP}:{SERVER_PORT}")
    app.run(host="0.0.0.0", port=SERVER_PORT, debug=True)