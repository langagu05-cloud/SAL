import asyncio
from bleak import BleakClient, BleakScanner
from google import genai

# Configura a tua chave da API do Gemini
GEMINI_API_KEY = "AQ.Ab8RN6IHnIPnvblf0lPSnEJBgpH8C9wOsELarpF_niyO060a6g"
client_ai = genai.Client(api_key=GEMINI_API_KEY)

# UUIDs definidos no ESP32
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_AUDIO_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
CHAR_TEXT_UUID = "8b365922-358b-42ea-92b0-8f61536b1d4c"

audio_buffer = bytearray()
is_receiving = False
EXPECTED_AUDIO_SIZE = 48044  # Tamanho exato do buffer enviado pelo ESP32 (WAV + PCM)

async def process_with_gemini(client):
    global audio_buffer, is_receiving
    print("\n[AI] Áudio completo recebido! A enviar para o Gemini...")
    
    prompt = (
        "És o Sálvio, um assistente de voz inteligente, carinhoso, paciente e prestativo, "
        "feito para conversar com um senhor de idade. Moras em São Bento do Sul, SC, Brasil. "
        "Responde de forma curta, clara e afetuosa (como um neto atencioso)."
    )

    try:
        response = client_ai.models.generate_content(
            model="gemini-2.5-flash",
            contents=[
                prompt,
                {"mime_type": "audio/wav", "data": bytes(audio_buffer)}
            ]
        )
        resposta_texto = response.text.strip()
        print(f"\n👴💬 SÁLVIO DIZ: {resposta_texto}\n")
        
        # Envia a resposta de volta para o ESP32 via BLE
        await client.write_gatt_char(CHAR_TEXT_UUID, resposta_texto.encode('utf-8'))
        print("[BLE] Resposta enviada de volta para o ESP32 com sucesso!")
        
    except Exception as e:
        print(f"[ERRO AI] Falha ao consultar o Gemini: {e}")
    
    audio_buffer.clear()
    is_receiving = False
    print("[BLE] Pronto para a próxima fala. Podes falar perto do Sálvio...")

def notification_handler(sender, data):
    global audio_buffer, is_receiving
    if not is_receiving:
        is_receiving = True
        audio_buffer.clear()
        print("\n[BLE] A receber áudio do Sálvio...")
    
    audio_buffer.extend(data)
    print(f"[BLE] Chunk recebido: {len(data)} bytes (Total: {len(audio_buffer)})")

async def main():
    print("[BLE] A procurar o Sálvio-Assistente...")
    device = await BleakScanner.find_device_by_name("Salvio-Assistente")
    
    if not device:
        print("[ERRO] Dispositivo 'Salvio-Assistente' não encontrado no Bluetooth!")
        return

    print(f"[BLE] Encontrado! A conectar a {device.address}...")
    async with BleakClient(device) as client:
        print("[BLE] Conectado com sucesso ao ESP32!")
        
        # Função interna para lidar com o envio assim que o buffer estiver cheio
        def wrapped_notification_handler(sender, data):
            notification_handler(sender, data)
            if len(audio_buffer) >= EXPECTED_AUDIO_SIZE:
                # Dispara o processamento da IA de forma segura
                asyncio.create_task(process_with_gemini(client))

        await client.start_notify(CHAR_AUDIO_UUID, wrapped_notification_handler)
        print("[BLE] A escutar áudio do microfone. Podes falar perto do Sálvio...")

        # Mantém o script Python ativo à escuta
        while True:
            await asyncio.sleep(1)

if __name__ == "__main__":
    asyncio.run(main())