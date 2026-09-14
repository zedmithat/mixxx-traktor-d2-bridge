# Touch FX makroları

ZED: yalnız Unit 3, Deck 1–2. Bağımsız XML: Unit 1, Deck 1–4.
D2 Unit 1/2, ses kartı ve harici mikser çıkışları ZED'de değiştirilmez.
CUE/PFL işleme eklenmez. Bu makrolar başka bir DJ cihazının birebir efekt kopyası değildir.

| Menü | Slotlar | X hareketi | Y hareketi | Alt sürgü |
|---|---|---|---|---|
| Filter Echo | Filter + Echo | Filter LP/HP | Echo süresi | Feedback %15–55 |
| Reverb + Flanger | Reverb + Flanger | Reverb decay | Flanger genişliği | Reverb send %0–65 |
| Filter Reverb | Filter + Reverb | Filter LP/HP | Reverb decay | Reverb send %0–65 |
| Filter Roll · Glitch | Filter + Glitch | Filter LP/HP | Tekrar süresi | Rezonans Q 0.707–2.5 |
| LFO Echo | Echo | LFO hızı, 0.2–3 Hz | Echo send %0–65 | Feedback %15–55 |
| Filter Dub Echo | Filter + Echo | Filter LP/HP | Echo süresi | Feedback %35–75 |
| Filter Gate · Tremolo | Filter + Tremolo | Filter LP/HP | Gate periyodu | Gate derinliği |
| Noise Gate · Tremolo | White Noise + Tremolo | Gate periyodu | Gate derinliği | Gürültü katkısı %0–8 |
| Flanger | Flanger | LFO periyodu | Genişlik | Regeneration %0–50 |
| LFO Filter | Filter | Filter merkez | LFO hızı, 0.2–3 Hz | Modülasyon derinliği %0–40 |
| Filter | Filter | Filter LP/HP | Rezonans Q 0.707–2.5 | — |

12 Eylül parametre adayı: Y artık bütün makrolarda ortak Dry/Wet değildir.
Pad üzerinde eksenlerin gerçek görevleri ve seçili ritim kademesi yazılır. Alt
sürgü pad'in altında ve kontrol ettiği parametrenin adıyla gösterilir. Yüzdeler
efekt parametre değeridir, ses seviyesi veya işitme güvenliği ölçüsü değildir.
Makro seçilirken ünite OFF / mix=0; pad hareketiyle mix=1 olur, ancak ON yalnız
dokunma kapısından gelir. Filtreli zincirde paralel kuru sinyal karışımı yerine
tam işlenmiş zincir kullanılır. Echo send Filter Echo'da0.35, Dub'da0.5 sabittir;
Reverb ve LFO Echo katkısı kendi send parametrelerinden yönetilir.
Tek-efekt menüsünde eski X=Super1 / Y=Dry-Wet davranışı korunur.

Echo/Glitch süre kademeleri: 2, 1, 3/4, 1/2, 1/4, 1/8 beat. Mixxx 2.5.6 Echo
DSP'si çeyrek vuruşa yuvarladığından 1/8 için kontrol değeri0 gönderilir; DSP'nin
minimum periyot sınırı bunu1/8 yapar. Gate periyotları4,2,1,1/2,1/4,1/8;
gerçek rate kontrolüne bunun tersi (vuruş başına çevrim) yazılır. Flanger
periyotları16,8,4,2,1,1/2 beat. Triplet kapalı, Echo/Glitch/Tremolo quantize açık.
Bu değerler BPM/beatgrid mevcutsa ritmiktir; yoksa Mixxx'in zaman tabanlı
davranışı geçerlidir. Özel LFO Echo/Filter modülasyonu ise Hz tabanlıdır ve
beatgrid fazına kilitli olduğu iddia edilmez. Filter meta ve Flanger genişliği
normalize parametrelerdir; doğrusal Hz/ms ölçeği vaat edilmez.
Roll Glitch tabanlı, Gate Tremolo tabanlı bir yaklaşımdır.
Ritim kademeli eksenlerde pad üzerinde etiketli şeritler görünür; seçili kademe
vurgulanır. Şerit sınırları MIDI'nin 0–127 kuantizasyonuyla eşleşir. Y ekseninde
yukarı çıkıldıkça kısa süreler seçilir. Bu görsel vurgu engine geri bildirimi
değildir; pad'in mevcut konumunu ve gönderilen dokunma durumunu gösterir.
Gürültü katkısı en fazla 0.08, Dub feedback en fazla 0.75 ile sınırlandırılır;
bu bir çıkış limiter'i veya işitme güvenliği garantisi değildir. İlk ses testi düşük
harici mikser seviyesinde yapılmalıdır. Noise Gate otomatik canlı ON testine alınmaz.

## Kullanım

Makro ve tek-efekt menüleri pad'in içinde ayrı sayfalardır; modal/popup pencere
açılmaz. Pad’e dön veya Escape seçim yapmadan geri döner. Menü açmak efekti kapatır.

Her açılışta TOUCH FX · Efektler menüsünden seçim yapın; eski zincir güvenlik için
etkin bırakılmaz. İki slotun yüklenip etkinleştiği
Mixxx tarafından doğrulanana kadar pad kapalıdır. Seçim her zaman FX OFF / Y=0 ile
başlar. Sonra pad'e dokunun; LATCH kapalıyken bırakmak OFF gönderir.
LATCH açıkken bırakmak konumu korur. Deck değiştirmek, menüyü açmak, FX OFF ve
Mixxx'e dönmek efekti kapatır. Yeni deck üzerinde yeniden dokunun.

Alt sürgü desteklenen makrolarda etkinleşir. Aktif düğme gerçek ünite ON geri
bildirimiyle yanıp söner; yalnız gönderilmiş Note On'a güvenmez. MIDI heartbeat
kesilirse Mixxx 1.5 saniyede OFF yapar. LFO sadece aktif efekt sırasında çalışır.

Reset FX: OFF, LATCH kapalı, X=64/Y=0, Amount=64; seçili makro slotlarını temizler,
Mixxx'in kayıtlı varsayılan presetlerini yeniden yükler ve tablodaki profil başlangıç
değerlerini uygular. D2, diğer üniteler ve Mixxx'in genel ayarları sıfırlanmaz.
Yükleme başarısızsa efekt kapalı kalır; menüden yeniden seçim yapılabilir.

## Katalog ve uyumluluk

`~/.mixxx/effects.xml` VisibleEffects sırasından gerçek 1-tabanlı MIDI indeksleri
okunur. Sabit Filter=14 gibi varsayım yoktur. `effects/defaults/` altındaki kayıtlı
Built-In presetlerin parametre sırası/görünürlüğü de kontrol edilir; uyumsuz veya
eksik efekt gerektiren makro düğmesi pasiftir. Gerekirse ilgili Built-In efekti
Mixxx'te bir kez yükleyip varsayılan presetini kaydedin, sonra Mixxx ve Touch FX'i
birlikte normal yeniden başlatın. Bu menü LV2 kataloğunu desteklemez.
Çalışma sırasında VisibleEffects sırasını veya varsayılan parametre düzenini
değiştirmeyin; değişiklikten sonra iki uygulamayı yeniden başlatın.

## MIDI

Tüm girişler seçili deck'in kanalında (0–3, ZED 0–1):
CC10=X, CC11=Y, CC12=deck seçimi, Note60=ON/OFF, CC119=heartbeat.
Makro işlemi: CC16=id → CC20=ilk efekt indeksi → CC21=ikinci indeks (tek slotta 0)
→ CC22=id (commit). Eksik/yanlış kanallı işlem etkinleşmez.
CC24=Amount (0–127); CC26=127 Reset. ZED CC13 tek efekt seçimidir.
Geri bildirim kanal 0: CC14=ilk hazır indeks, CC15=ON/slot/deck bayrakları,
CC18=makro id (127 yükleniyor/hata), CC19=ikinci hazır indeks, CC23=hazır slot maskesi.

Mixxx kaynak davranışı: `loaded_effect` kayıtlı varsayılanları yükler; önce `clear`
ile boşalma doğrulanması aynı efektin Reset'inde eski değerin yanlış ACK sayılmasını
önler. Kaynak: https://github.com/mixxxdj/mixxx/blob/2.5/src/effects/effectslot.cpp

Testler: `python -m unittest discover -s tests -p 'test_*.py'` ve
`node --test tests/test_mapping.cjs tests/test_macros.cjs tests/test_private_mapping.cjs`.
