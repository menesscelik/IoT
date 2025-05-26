# 🔐 ESP32-S3 Voice-Controlled Smart Lock System

## 📌 Project Overview

This project presents a fully integrated smart lock system that utilizes voice-based user identification, password verification via a keypad, and physical actuation through a servo motor. Built on the **ESP32-S3 DevKitM-1**, the system leverages **AI-powered voice processing**, **Whisper speech-to-text**, and **Grok Lima 4 (LLM)** integration to enable both secure authentication and intelligent user interaction.

Once a user speaks their name, the system recognizes it using Whisper, then prompts for a password via a **4x4 matrix keypad**. If the input matches, a **servo motor rotates 180°** to simulate door unlocking. In case of failed authentication, a voice message indicates “Access Denied.” The system also includes an assistant function that responds to queries such as “What’s the weather like?” using Grok and plays responses through a **PCM5102A DAC module** and speaker.

---

## 🎯 Project Goals

- Replace traditional keys with voice + password authentication
- Provide visual and mechanical feedback via a servo motor
- Integrate natural language voice interaction using LLMs
- Add dynamic user registration functionality
- Demonstrate the use of AI in embedded security systems

---

## ⚙️ System Architecture

1. User speaks their name into the microphone  
2. Microphone audio sent to backend (Python Flask)  
3. Whisper STT transcribes speech to text  
4. System compares input name with user database  
5. If match found, prompts user for password via keypad  
6. If correct:  
   - Servo rotates 180°
7. If wrong:  
   - Servo does not move, speaker says “Access Denied”  
8. Optional: User asks “What’s the weather like?”  
   - Text query sent to Grok  
   - Response returned, converted to audio  
   - Played through speaker

---

## 🧰 Hardware Components

| Component | Description |
|----------|-------------|
| **ESP32-S3 DevKitM-1** | Main microcontroller |
| **INMP441 Microphone** | Digital I2S mic for voice capture |
| **4x4 Matrix Keypad** | For password input |
| **SG90 Servo Motor** | Simulates door unlocking |
| **PCM5102A DAC Module** | Converts digital TTS output to analog |
| **Speaker** | Plays system responses |
| **Breadboard + Jumper Wires** | Circuit prototyping |

---

## 💻 Software Stack

- **PlatformIO (VS Code)** — Firmware development
- **ESP32Servo** — Controls servo motor
- **Keypad.h** — Reads keypad input
- **Flask (Python)** — Hosts backend logic
- **Whisper** — Transcribes spoken name
- **gTTS + pydub** — Text-to-speech & audio formatting
- **requests** — Sends queries to Grok LLM

---

## 🚀 Setup & Installation

### 1. Clone the Repository

```bash
git clone https://github.com/menesscelik/IoT.git
