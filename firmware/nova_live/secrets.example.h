// Bu dosyayı "secrets.h" adıyla kopyala ve doldur.
// secrets.h kimseyle paylaşılmaz, sohbete yapıştırılmaz, GitHub'a yüklenmez (.gitignore dışarıda tutar).
#pragma once

#define WIFI_SSID      "wifi-adi"
#define WIFI_PASS      "wifi-sifresi"

// OpenAI anahtarı (GPT-Live): https://platform.openai.com/api-keys
#define OPENAI_API_KEY "BURAYA-ANAHTAR"

// Kablosuz yükleme (Arduino IDE ağ portu) ve http://nova.local kayıt/komut sayfasının şifresi.
// Kendin seç, en az 8 karakter. Sayfada kullanıcı adı "nova". Bu satır yoksa kablosuz erişim kapalı kalır.
#define OTA_PASS       "BURAYA-SIFRE"
