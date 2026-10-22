// Nova V2 — donanım ve ayar sabitleri (nova_live: modele doğrudan bağlanan firmware)
// Pin haritası 10 Ekim 2026'da lehimlenen haliyle.
#pragma once

#define FW_VERSION "2.8.3"

// ---------- Pinler ----------
// Sağ amfi (MAX98357A)
#define PIN_AMP_R_BCLK 42
#define PIN_AMP_R_LRC   2
#define PIN_AMP_R_DIN  41
// Sol amfi (MAX98357A) — aynı I2S sinyali bu pinlere de kopyalanır
#define PIN_AMP_L_BCLK 10
#define PIN_AMP_L_LRC  11
#define PIN_AMP_L_DIN   9
// Mikrofonlar (2x INMP441, ortak hat, biri sol biri sağ kanal)
// Lehim listesinde WS=12, SCK=13 yazıyordu; kartta ters çıktı. Firmware'in
// öz-testi buldu, ses kaydıyla doğrulandı (10 Ekim 2026).
#define PIN_MIC_SCK    12
#define PIN_MIC_WS     13
#define PIN_MIC_SD     14
// Ekran (1.8" ST7735, SPI). Modülde SDA/SCL yazar ama SPI'dır.
#define PIN_TFT_BLK     4
#define PIN_TFT_CS      5
#define PIN_TFT_DC      6
#define PIN_TFT_RST     7
#define PIN_TFT_MOSI   15   // modülde "SDA"
#define PIN_TFT_SCK    16   // modülde "SCL"
// Pil ölçümü (47k/47k bölücü, pil voltajının yarısı)
#define PIN_BAT_ADC     3
// Hiçbir şeye bağlı olmayan bir pin: mikrofon hattı testlerinde karşılaştırma için ölçülür.
#define PIN_FREE_REF   17
// Kartın üstündeki BOOT butonu: basınca uyandırır / konuşmayı keser
#define PIN_BUTTON      0
// Kartın üstündeki RGB LED. Çoğu kartta 48, bazılarında 38. Kapatmak için -1.
#define PIN_RGB_LED    48

// ---------- Ekran ----------
// Renkler ters ya da kenarda çizgi varsa sırayla dene:
// INITR_BLACKTAB, INITR_GREENTAB, INITR_REDTAB
#define TFT_TAB        INITR_BLACKTAB
// Görüntü ters duruyorsa 1 yerine 3 yap (yatay kullanım).
#define TFT_ROTATION   1
#define TFT_SPI_HZ     16000000   // elle çekilmiş tellerde güvenli hız; sorun yoksa 27000000 yapılabilir
#define UI_FPS         20

// ---------- Ses ----------
#define MIC_RATE           16000
#define MIC_FRAME_SAMPLES  1280     // 80 ms: modele bu boyda parçalar gider
// 24 bit mikrofon verisinin 16 bite indirilmesi. Küçük sayı = daha yüksek ses.
// V1'deki değer 14'tü.
#define MIC_SHIFT          14
// 0: iki mikrofonun ortalaması (biri bozulursa sağlam olanla devam eder), 1: sadece sol, 2: sadece sağ
#define MIC_MODE           0
// Mikrofon saat hatlarının (SCK, WS) sürüş gücü: 0 en zayıf, 3 en güçlü. 2 ESP'nin varsayılanıdır.
// Serial'de 'd' karşılaştırma testi 2 ile 3'ü karşılaştırır.
#define MIC_DRIVE          2
// 1: mikrofonlardan veri gelmezse öz-test tellerin başka sırada lehimlenmiş olma ihtimalini de dener
// (5 sn sürer). Tel sırası doğrulandı (config.h'deki pinler doğru), bu yüzden kapalı.
#define MIC_TRY_PIN_ORDERS 0
// 1: bir mikrofon susunca saniyede bir kez veri hattı zayıfça yukarı çekilip "çip ayakta mı" diye
// bakılır (2.2.5'te açıktı; sonuç: her susuşta iki çip de ayakta ve hattı 0'da tutuyordu). 0: çalışırken
// mikrofon hatlarına hiç dokunulmaz, yalnızca gelen veri izlenir.
#define MIC_PROBE_SILENCE  0
// Bunun altındaki frekanslar atılır (uğultu, 50 Hz vınlaması). Konuşma 100 Hz'den başlar.
#define MIC_HPF_HZ         120

#define SPK_RATE           24000
#define SPK_BLOCK          240      // 10 ms
#define SPK_PREBUF_MS      120      // çalmaya başlamadan önce biriken ses
// Sunucu sesi gerçek zamandan hızlı yollayabilir: uzun bir cevabın tamamı tamponda bekleyebilir.
#ifndef SPK_RING_MS
#define SPK_RING_MS        60000    // çalma tamponu (PSRAM'de 2,9 MB)
#endif
#define SPK_RING_MS_NOPSRAM 2500
#define DEFAULT_VOLUME     80       // 0-100. %100'de amfi bozuyor (Aşama 1 ölçümü); temiz tavan %80-90
// Gürlük artırma (loudness.h): modelin konuşması bu kadar yükseltilir, tavanı aşacak tepeler
// sınırlayıcıyla kısılır. Ses ayarı en sonda uygulanır: tepe seviyesi eskisiyle aynı kalır,
// ortalama yükselir. 0 yapılırsa eski davranışa yakın olur. Açılışta ve her cevaptan sonra
// "[ses]" satırı gelen sesin tepe ve ortalamasını yazar; bu sayı ona göre ayarlanır.
#define SPK_BOOST_DB       10.0f
// GPT-Live'ın sesi kısık geliyor (11 Ekim ölçümü, ortalama -25 dB; Gemini -20 dB'di). Oturum sırasında
// kazanç bu değere çıkar; açılış sesi ve testler SPK_BOOST_DB ile çalar.
#define SPK_BOOST_DB_GPTLIVE 14.0f
#define SPK_LIMIT_REL_MS   100.0f   // sınırlayıcının kısılan sesi geri açma süresi
// Sınırlayıcının tavanı (tam ölçeğe göre). Amfi tam ölçeğe yakın sürülünce bozuyor (10 Ekim: %100'de
// bozulma; 17 Ekim: %70 üstünde yankı 11 dB artıyor, bastırma düşüyor). Bozulmuş ses yankı silmeyle de
// silinemiyor. 18 Ekim: kullanıcı tavanın 3 dB aşağı çekilmesini seçti (%100 artık amfiyi bozmasın).
#define SPK_CEILING_DB     -3.0f
#define SPK_HPF_HZ         150.0f   // hoparlörün veremediği alt frekanslar atılır
#define DEFAULT_BRIGHTNESS 80       // 0-100

// ---------- Pil ----------
#define BAT_DIVIDER        2.0f     // 47k/47k
// Multimetreyle ölçtüğün pil voltajı / ekranda görünen voltaj. Önce 1.00 bırak.
#define BAT_CAL            1.00f
#define BAT_LOW_V          3.20f    // bunun altında 20 sn kalırsa uykuya geçer
#define BAT_VALID_MIN_V    2.50f    // bunun altı "ölçüm yok" sayılır

// ---------- Ağ ----------
#define HOSTNAME           "nova"   // ağdaki adı: http://nova.local, Arduino IDE'de "nova at ..."
// Kart art arda bu kadar çökerse güvenli kipte açılır (yalnızca WiFi + kayıt sayfası + kablosuz yükleme)
#define SAFE_MODE_CRASHES  3
#define STATUS_INTERVAL_MS 5000

// ---------- Konuşma modeli: OpenAI GPT-Live ----------
// ChatGPT uygulamasındaki sesli sohbetin modeli (API'de 10 Eylül 2026'dan beri). Aynı anda dinleyip
// konuşur. Anahtar secrets.h'deki OPENAI_API_KEY. 2.4.3'e kadar Gemini Live ve GPT Realtime da vardı
// ('p' ile seçiliyordu); 2.5.0'da firmware/arsiv/saglayicilar klasörüne taşındılar.
#ifndef OPENAI_HOST
#define OPENAI_HOST        "api.openai.com"
#endif
#ifndef OPENAI_PORT
#define OPENAI_PORT        443
#endif
// Ücret: bağlı kalınan süre (dakikası GPTLIVE_USD_PER_MIN dolar) + arka modelin jetonları.
#ifndef GPTLIVE_MODEL
#define GPTLIVE_MODEL      "gpt-live-1"
#endif
// Sesler: marin (varsayılan), cedar, alloy, ash, ballad, coral, echo, sage, shimmer, verse ve bu modelle
// gelenler: quartz, ripple, vesper, willow, stone, gleam, meridian, bossa, tempo, beacon, delta, cinder,
// brise, flitz, harema, juni, nira, noeul, nuri, shida, sillage. Hangisinin Türkçesi iyi, belgede yazmıyor.
#ifndef GPTLIVE_VOICE
#define GPTLIVE_VOICE      "marin"
#endif
// 'o' / 'O' (ya da sayfadaki "Ses tonu") bu listede ileri / geri gider; seçim kartta saklanır.
#define GPTLIVE_VOICES "marin", "cedar", "alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse", \
  "quartz", "ripple", "vesper", "willow", "stone", "gleam", "meridian", "bossa", "tempo", "beacon", "delta", \
  "cinder", "brise", "flitz", "harema", "juni", "nira", "noeul", "nuri", "shida", "sillage"
// Arama ve ağır düşünme işini yapan arka model (OpenAI çalıştırır, jetonla ayrıca ücretlenir).
// "gpt-6-luna" en ucuzu; daha akıllısı "gpt-6.1-sol". "" yapılırsa arka model istenmez.
#ifndef GPTLIVE_BACKEND_MODEL
#define GPTLIVE_BACKEND_MODEL "gpt-6-luna"
#endif
#define GPTLIVE_BACKEND_NOTE "Cevabın sesli okunacak: en fazla iki kısa cümle, düz metin, kullanıcının konuştuğu dilde (genelde Türkçe). Liste, madde işareti ya da bağlantı kullanma. Arama gerekiyorsa tek arama yap, sonucu hemen ver."
// Arka modeli hızlandıran ayarlar (11 Ekim'de web araması 10-20 sn sürdü). Hızlı kip jeton ücretini
// iki katına çıkarır (günlük kullanımda kuruşlar). Sunucu kabul etmezse cihaz bunlarsız bağlanır ve
// Serial'e yazar. "" yapılırsa gönderilmez.
#ifndef GPTLIVE_BACKEND_TIER
#define GPTLIVE_BACKEND_TIER   "priority"
#endif
#ifndef GPTLIVE_BACKEND_EFFORT
#define GPTLIVE_BACKEND_EFFORT "low"
#endif
// 1: arka modele web araması aracı verilir (güncel bilgi sorulabilir)
#ifndef GPTLIVE_WEB_SEARCH
#define GPTLIVE_WEB_SEARCH 1
#endif
// Model "cevabım bitti" demez. Bu kadar süre sesli parça gelmezse cevap bitmiş sayılır ve mikrofon
// açılır. Küçültmek mikrofonu erken açar ama cümle arasındaki duraklamada da açabilir (zararı yok:
// Nova devam ederse yeniden kapanır).
#ifndef GPTLIVE_END_GAP_MS
#define GPTLIVE_END_GAP_MS 450
#endif
// GPT-Live sesi büyük olasılıkla gerçek zamanlı yolluyor (Gemini ve GPT Realtime gibi önden
// yığmıyor); ağ gecikince hoparlör boşta kalır ve ses takılır. Bu yüzden çalmaya başlamadan önce
// SPK_PREBUF_MS'ten fazla ses biriktirilir. Büyütmek takılmayı azaltır, cevabı o kadar geciktirir.
// "[nova] cevap bitti: ... akışta N duraklama" satırındaki N yüksekse büyüt.
#ifndef GPTLIVE_PREBUF_MS
#define GPTLIVE_PREBUF_MS  200
#endif
// Cevabın ortasında ses ağdan geç gelirse (hoparlör tamponu boşalır, takılma duyulur) ön tampon o oturumda
// 150 ms'lik adımlarla en çok buna kadar büyütülür (2.6.2; iki kayıtta ~40 cevabın 2'sinde görüldü).
#define GPTLIVE_PREBUF_MAX_MS 500
// Hoparlör boşaldığında akışta (sessiz parçalar dahil) bundan uzun boşluk varsa ses ağdan geç gelmiş sayılır
// ve ön tampon büyütülür; yoksa model duraksamıştır, ön tampon değişmez (2.8.1).
#ifndef GPTLIVE_NET_GAP_MS
#define GPTLIVE_NET_GAP_MS 250
#endif
// Hoparlör sustuktan (yukarıdaki süre) sonra mikrofon açılır ama cevap hemen "bitti" sayılmaz: model
// cümle aralarında yarım saniyeyi aşan duraklamalar yapıyor. Bu kadar süre ses gelmezse cevap bitmiş
// sayılır (ekran "dinliyor"a döner, satır Serial'e basılır). Kullanıcı arada konuşursa hemen biter.
#ifndef GPTLIVE_ANSWER_END_MS
#define GPTLIVE_ANSWER_END_MS 1200
#endif
// Kullanıcının sözünün yazısı bu kadar süre durunca (ve mikrofonda ses yokken) ekran "düşünüyor"a geçer
#define GPTLIVE_THINK_AFTER_MS 900
// 1: her bağlantıda ilk kez görülen olay türleri ham haliyle Serial'e yazılır (inceleme için)
#ifndef GPTLIVE_LOG_FIRST_EVENTS
#define GPTLIVE_LOG_FIRST_EVENTS 0
#endif
// Bitiş anlaşıldıktan sonra mikrofonu açmadan önceki ek bekleme
#ifndef GPTLIVE_POST_SPEECH_GUARD_MS
#define GPTLIVE_POST_SPEECH_GUARD_MS 100
#endif
// Gelen ses parçasının ortalama seviyesi (RMS) bunun altındaysa sessizlik sayılır (32767 üzerinden;
// 100 = -50 dB). Model susarken de ses akıtıyorsa ve bu ses tam sessizlik değilse eşik çalışırken
// kendiliğinden yükseltilir: GPTLIVE_NOISE_CHECK_MS boyunca yazısı gelmeyen ve seviyesi
// GPTLIVE_NOISE_MAX_RMS'in altında kalan ses konuşma sayılmaz.
#ifndef GPTLIVE_OUT_SILENCE_RMS
#define GPTLIVE_OUT_SILENCE_RMS 100
#endif
#define GPTLIVE_NOISE_CHECK_MS  3000
#define GPTLIVE_NOISE_MAX_RMS   800
// Oturum kapanınca konuşma sunucuda kalmaz. Son satırlar cihazda tutulur ve bu süre içinde yeniden
// uyandırılırsa yeni oturuma yazı olarak verilir.
#define GPTLIVE_HISTORY_LINES 16
#define GPTLIVE_HISTORY_TEXT  360
#ifndef GPTLIVE_HISTORY_MAX_AGE_MS
#define GPTLIVE_HISTORY_MAX_AGE_MS (30UL * 60UL * 1000UL)
#endif
#define GPTLIVE_USD_PER_MIN 0.05f   // ses katmanının dakika ücreti (11 Ekim 2026 fiyat sayfası)

// 'o' ile yeni ses seçilince Nova bu yazılı soruyla bağlanıp kendini tanıtır
#define VOICE_SAMPLE_PROMPT "Yeni sesinle kendini iki kısa cümleyle tanıt."

// Modele giden mikrofon sesinin yükseltilmesi (256 = 1x, 512 = 2x)
#define MIC_UPLINK_GAIN_Q8 512
// Yükseltilmiş seste bu tepeyi aşan çerçeve yerel olarak "ses var" sayılır. Yalnızca
// gecikme ölçümünde ve sunucu konuşma işareti yollamıyorsa kullanılır (GPT-Live yollamıyor).
#define MIC_ACTIVITY_PEAK  2500
// Dinlerken bu kadar süre konuşma olmazsa oturum kapanır (ücret ve pil için).
// Sayaç yazıya dökülen söze ve Nova'nın sesine bakar; odadaki tıkırtı sayacı sıfırlamaz.
#ifndef LINK_IDLE_SLEEP_MS
#define LINK_IDLE_SLEEP_MS 25000
#endif
// Kısa kapanış (2.7.1, kullanıcının isteği): Nova'nın cevabı soruyla bitmediyse ve ardından bu kadar süre kimse
// konuşmazsa oturum kapanır ("teşekkürler" deyip susunca beklemesin). Cevap soruyla bittiyse normal süre geçerli.
// Kapandıktan sonra "Hey Nova" ile açılan oturum son konuşmayı bilir. 0 yapılırsa kapalı.
#ifndef LINK_SHORT_CLOSE_MS
#define LINK_SHORT_CLOSE_MS 12000
#endif
// Sunucudan hiçbir işaret gelmeden en çok bu kadar dinlenir
#ifndef LINK_MAX_QUIET_MS
#define LINK_MAX_QUIET_MS  90000
#endif
// Söz bittikten sonra cevap gelmezse "düşünüyor" en çok bu kadar gösterilir
#ifndef LINK_THINK_MAX_MS
#define LINK_THINK_MAX_MS  8000
#endif
// Nova sustuktan sonra mikrofonu açmadan önceki bekleme
#ifndef LINK_POST_SPEECH_GUARD_MS
#define LINK_POST_SPEECH_GUARD_MS 350
#endif
// ---------- Uyandırma sözü ("Hey Nova") ----------
// Espressif'in hazır wn10_heynova modeli (heynova_model.c). Serial'de / sayfada 'n' ile kapalı / normal /
// hassas / çok hassas; seçim kartta saklanır, bu yalnızca ilk açılışın varsayılanı.
// Hassas kipler daha çok yakalar, daha sık yanlış uyanır (yanlış uyanma ~25 sn'lik boş oturum demek).
// 17 Ekim: kullanıcı "çok hassas"ta karar kıldı.
#define WAKE_DEFAULT_MODE  3      // 0 kapalı, 1 normal, 2 hassas, 3 çok hassas ('n' ile sırayla)
// Oturum kapandıktan sonra motor bu kadar bekleyip kurulur (Nova'nın son sözünün yankısı sönsün)
#ifndef WAKE_HOLDOFF_MS
#define WAKE_HOLDOFF_MS    1500
#endif

// ---------- Yankı silme ve araya girme (Aşama 3) ----------
// 'a': yankı ölçümü (buluta bağlanmadan; 3 sn hışırtı çalar). Hoparlörden mikrofona gecikmeyi ve yankı
// silmenin ne kadar bastırdığını ölçer, kartta saklar. 'e': araya girme açık / kapalı.
// Açıkken oturumda mikrofon sesi ESP-SR yankı silmesinden geçer ve Nova konuşurken de modele gider;
// yalnızca ölçülen artık yankının belirgin üstündeki ses geçer (kapı), gerisi sessizlik.
#define ECHO_AEC_MODE       5      // 5 = AEC_MODE_FD_LOW_COST (Aşama 1: %25 işlemci), 6 = FD_HIGH_PERF
// Doğrusal silmeden sonra kalan yankıyı bastıran ek katman (NLP). 0 normal, 1 güçlü (ESP-SR'nin FD varsayılanı),
// 3 kapalı (yalnızca doğrusal). 17 Ekim: güçlüyken Nova konuşurken kullanıcının sesi modele ulaşmadı (yazıya hiç
// dökülmedi); NLP iki kişi aynı anda konuşurken yakındaki sesi de bastırıyor olabilir. Varsayılan normal.
// 'E' ile sırayla değişir (sonraki oturumda geçerli), kartta saklanır.
#define ECHO_NLP_DEFAULT    0
#define ECHO_FILTER_LEN     4      // ESP-SR'nin önerdiği
// Yankı silmenin girişi bu kadar kısılır, çıkışı aynı kadar yükseltilir (modele giden seviye değişmez).
// 17 Ekim: ses %100'deyken Nova konuşurken mikrofon örneklerinin %6'ya kadarı kırpıldı; kırpılan yankı
// silinemiyor (canlı bastırma 4-18 dB, ölçümde 35 dB). 12 dB pay kırpılmayı önler.
#define ECHO_IN_HEADROOM_DB 12
// Yankı silmenin çalışma belleği: 1 = PSRAM, 0 = iç RAM (daha hızlı). İç RAM'de toplam boş yer çok ama parçalı
// (en büyük blok 30 KB); ESP-SR büyük bir blok alamazsa çökebiliyor. 'a' ölçümü işlem süresini yazar.
#define ECHO_AEC_PSRAM      1
#define ECHO_ADVANCE        32     // referans, ölçülen gecikmeden 2 ms önce verilir (süzgeç penceresinin içinde kalsın)
#define ECHO_DELAY_MAX      4800   // ölçümde aranan en büyük gecikme (300 ms)
#define ECHO_GATE_MARGIN_DB 8.0f   // Nova konuşurken kapı, ölçülen artık yankının bu kadar üstünde açılır
#define ECHO_GATE_HANG_MS   250    // kapı açıldıktan sonra açık kalma (hece aralarında kesilmesin)
#define ECHO_DEFAULT_ON     0      // ilk açılışta araya girme (sonra 'e' ile değişir, kartta saklanır)
// Nova'nın cevabı başladıktan sonra bu süre içinde gelen kullanıcı yazısı araya girme sayılmaz: o, kullanıcının
// bir önceki sözünün geç gelen parçasıdır (model sözün bitiminden ~200 ms önce konuşmaya başlayabiliyor, yazı
// sesten geç geliyor). 17 Ekim'de bu yüzden Nova'nın cevabının başı kesildi.
#define GPTLIVE_BARGE_GUARD_MS 1500

// ---------- Sesle oturum kapatma ----------
// Kullanıcının sözünün yazısında bunlardan biri geçerse cihaz, Nova'nın cevabı bitince oturumu kendisi
// kapatır. Modelin "kapatıyorum" demesine güvenilmez: GPT-Live'ın ses modeli araç
// kullanamıyor. Karşılaştırma küçük harfe çevrilip Türkçe harfler sadeleştirilerek yapılır; listeye
// eklerken de öyle yaz (ı->i, ş->s, ğ->g, ü->u, ö->o, ç->c).
// Komutlar: sözün içinde geçmesi yeter ("oturumu bitirir misin" de tutar).
#define HANGUP_COMMANDS  "oturumu kapat", "oturumu bitir", "oturumu sonlandir", "konusmayi bitir", "konusmayi kapat", \
                         "konusmayi sonlandir", "gorusmeyi bitir", "gorusmeyi kapat", "gorusmeyi sonlandir", "uyuyabilirsin", "uykuya gec", \
                         "kapat kendini", "kendini kapat", "kapan artik"
// Vedalar: kısa bir sözün sonunda olmalı ("tamam görüşürüz"). Uzun cümlenin ortasındaki "görüşürüz" sayılmaz.
#define HANGUP_FAREWELLS "gorusuruz", "gorusmek uzere", "hosca kal", "hoscakal", "iyi geceler", "bay bay", "bye bye", "kendine iyi bak"
// Nova'nın kendi vedası: kısa cevabı (en çok NOVA_BYE_MAX_WORDS kelime) bunlardan birini içeriyorsa cevap
// bitince oturum kapanır. Kullanıcının listede olmayan bir sözle bitirmek istediği durumu yakalar
// ("kapatır mısın sesyonu"); prompt Nova'ya bu kelimeyi yalnızca konuşmayı bitirirken kullanmasını söyler.
// "kendine iyi bak" burada yok: Nova onu birine selam iletirken de söylüyor.
#define NOVA_FAREWELLS     "gorusuruz"
#define NOVA_BYE_MAX_WORDS 5
// Veda algılandıktan sonra Nova hiç cevap vermezse bu kadar beklenip kapatılır
#ifndef LINK_BYE_WAIT_MS
#define LINK_BYE_WAIT_MS   4000
#endif
// Arka model bir işi sürdürürken oturum sessizlik yüzünden kapatılmaz; en çok bu kadar beklenir
#ifndef LINK_BACKEND_WAIT_MS
#define LINK_BACKEND_WAIT_MS 60000
#endif
// Sunucudan gelen tek bir mesajın sığacağı tampon (PSRAM) ve gönderme tamponu
#define WS_RX_BYTES        (256 * 1024)
#define WS_TX_BYTES        (48 * 1024)   // GPT-Live'ın açılış mesajı kişilik + hafıza + konuşma geçmişi + araçları taşır

// ---------- Hafıza ve zamanlayıcılar (hafiza.cpp) ----------
#define HAFIZA_MAX_NOT     60       // kalıcı not sayısı (her oturumda talimata eklenir)
#define HAFIZA_METIN       180      // bir notun en uzun hali (bayt; Türkçe harfler iki bayt)
#define ZAMANLAYICI_MAX    8
#define YEREL_SAAT_FARKI   10800    // Türkiye UTC+3 (yaz saati yok)

// ---------- Hava durumu (hava.cpp; Open-Meteo, anahtarsız) ----------
// Cihaz boştayken çekilir, oturum açılınca talimata eklenir: "hava nasıl" sorusu aramasız cevaplanır.
// Çekerken uyandırma motoru birkaç saniye kalkar (iç RAM şifreli bağlantıya kalsın).
#define HAVA_ACIK          1
#ifndef HAVA_YER
#define HAVA_YER           "İzmir"
#endif
#ifndef HAVA_ENLEM
#define HAVA_ENLEM         38.42f
#endif
#ifndef HAVA_BOYLAM
#define HAVA_BOYLAM        27.14f
#endif
#define HAVA_YENILE_MS     (60UL * 60UL * 1000UL)   // bu kadar eskiyince yenilenir
#define HAVA_TEKRAR_MS     (10UL * 60UL * 1000UL)   // alınamazsa bu kadar sonra yeniden denenir
#define HAVA_ESKI_MS       (6UL * 60UL * 60UL * 1000UL)   // bundan eskisi modele verilmez

// ---------- Gündem özeti (gundem.cpp; arka model + web araması) ----------
// Günün en çarpıcı genel olayları ve önemli teknoloji gelişmeleri. Aşağıdaki her yerel saatten sonraki ilk
// boş anda bir kez çekilir, oturum açılınca talimata eklenir. İstek arka planda çalışır (background):
// gönderip bırakır, sonucu 10 sn arayla yoklar; uyandırma motoru her seferinde yalnızca 1-2 sn kalkar.
// Ücret (OpenAI fiyat sayfası, 18 Ekim 2026): web araması 0,01 $/arama + arama metni jetonları (gpt-6-luna
// 0,10 $/M giriş). Bir çekiş en çok GUNDEM_MAX_ARAMA arama yapar: çekiş başına ~0,01-0,035 $.
#define GUNDEM_ACIK        1
#define GUNDEM_SAATLER     {8, 13, 19}
#define GUNDEM_MAX_ARAMA   3
#define GUNDEM_ESKI_SAAT   14                              // bundan eskisi modele verilmez
#define GUNDEM_CAP         1600                            // özetin en çok bayt sayısı (NVS'de de saklanır)
#define GUNDEM_TEKRAR_MS   (15UL * 60UL * 1000UL)          // ağ hatasında yeniden deneme
#define GUNDEM_YOKLA_MS    10000                           // sonucu yoklama aralığı
#define GUNDEM_YOKLA_MAX   24                              // ~4 dk sonra vazgeçilir

// ---------- Boşta güç ----------
// Uykudayken (yalnızca "Hey Nova" dinlenirken) bu kadar süre sonra ekran kısılır ve WiFi güç tasarrufuna
// geçer; uyanınca ikisi de geri gelir. 0 yapılırsa kapalı.
#define BOSTA_KIS_MS       30000
#define BOSTA_PARLAKLIK    3        // % (0 = ekran kapalı)
#define BOSTA_WIFI_UYKU    1

// ---------- Sesle ses ayarı (yerel yedek) ----------
// "Sesi kıs / aç / yüzde 50 yap" yazıya dökülünce cihaz modeli beklemeden uygular (model de aracı çağırırsa
// ikinci kez değiştirilmez). Adım: "biraz" denirse yarısı.
#define SESLI_SES_ADIM     20

