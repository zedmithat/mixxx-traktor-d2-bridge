# Touch FX — Raspberry Pi + Mixxx 2.5

Özgün kod **GPL-3.0-or-later** lisanslıdır; [kapsam ve istisnalar](LICENSING.md),
[tam lisans metni](LICENSE). Üçüncü taraf lisansları değişmez.

**Güncel lisanslı kaynak paketi: `touchfx-source-20260910-12.tar.gz`.** Paket 12
lisans belgelerini ekler; G uygulama/mapping kodu paket 11 ile aynıdır. Altıncı paket
11 makro, Amount, gerçek ON geri bildirimiyle yanıp sönen düğme ve Reset FX ekler.
Beşinci paket gecikmeli efekt yükleme/etkinleştirme sorununu giderdi.
Yedinci paket yeniden açılışta eski makro slotlarını kapalı tutar; menüden yeniden seçim yapılır.
Sekizinci paket iki JavaScript dosyasının init sırasını ve slot kapatmayı birlikte test eder.
Dokuzuncu paket Sway tam ekran kuralını yalnız ana pencereye sınırlar; makro
menüsü normal yüzen pencere olur ve kapanınca pad tam ekrana döner.
Dokuzuncu düzeltme gerçek Pi'de yeterli olmadı. Onuncu paket modal pencereyi ve
yerel açılır listeyi kaldırır: makro ve tek-efekt seçimleri aynı ana pencerede
sayfa değiştirerek açılır. Gerçek Wayland testinde 20 menü döngüsü ve tek-efekt
menüsü aynı tam ekran pencereyi korudu; ayrı MIDI portu açılmadı.
On birinci paket ZED arayüzünde kullanılmayan Deck 3/4 düğmelerini kaldırır;
yalnız Deck 1/2 gösterilir. Bağımsız dört-deck örneği ve MIDI mapping değişmez.
Önceki kayıtlar tarihseldir.
İlk paket
yerel Mixxx'te bulunmayan eski ünite-sayısı kontrolüne dayanıyordu; kullanmayın.
İkinci pakette Unit 3 slot kontrolü, skin bağlantı yaşam döngüsü ve Wayland
başlangıcı düzeltildi. Windows/Linux birlikte 108 farklı test doğruladı.
Üçüncü paket, gerçek Raspberry Pi OS rootfs'indeki standart `os-release`
symlink'ini güvenli okumayı ve Debian'da ayrı paketlenen QtQml test modülünün
doğru tespitini ekler. Önceki test sayıları tarihsel sonuçlardır.
Özel test imajı ayrıca Unit 3 için Filter preset'i içerir; bu preset genel
kurucunun varsayılanı değildir. İmaj başkalarına dağıtılmamalıdır.

PyQt5 dokunmatik X/Y pad → mido/python-rtmidi → ALSA sanal MIDI → Mixxx XML/JS.
Tam kodlar bu klasördedir; mevcut Mixxx imajını veya D2 mapping'ini değiştirmez.

**ZED entegrasyonu:** Yeni `stage-zed/20-touch-fx` aşaması, ayrı `zed-touchfx`
skin'i ve Unit 3 mapping'i kurar. D2'nin Unit 1/2 kullanımı ve iki deck düzeni
korunur. TOUCH FX düğmesi pad'i açar; “Mixxx'e dön” efekti kapatıp pad'i gizler,
MIDI portunu açık tutar. Ayrıntı: [ZED entegrasyonu](integration/README.md).
Eski imaj arşivleri otomatik güncellenmez; canlı kurulum/test durumu releases raporundadır.

**Bağımsız kullanım:** Aşağıdaki orijinal mapping etkinleştirildiğinde Effect Unit 1 Touch FX'e ayrılır.
Açılışta ünite bypass edilir; Deck 2–4, master, headphone ve mevcut sampler
atamaları kapatılır, Deck 1 atanır. Unit 1'i aynı anda D2 veya başka bir
controller ile yönetmeyin. Makro seçimi ayrılmış ünitenin slotlarını değiştirir; eski
yönlendirme/parametre ayarları çıkışta geri yüklenmez. Önce düşük sesle deneyin.

## Dosyalar

Makro tablosu, Amount/Reset davranışı ve uyumluluk: [Makrolar](MACROS.md).

- `touchfx.py`: PyQt5 arayüzü ve gerçek ALSA MIDI çıkışı.
- `touchfx_core.py`: dokunma, Latch, deck seçimi ve MIDI durum mantığı.
- `TouchFX_Virtual.midi.xml`: dört kanalın eksen, gate ve makro MIDI binding'leri.
- `TouchFX-scripts.js`: Effect Unit 1 yönlendirmesi, eksenler, gate ve watchdog.
- `TouchFX-macros.js`: iki slotlu parametre dağıtıcısı, güvenli yükleme ve LFO.
- `requirements.txt`: pip bağımlılıkları; PyQt5 Pi'de dağıtım paketinden alınır.
- `integration/`: ZED skin/mapping oluşturucusu, user service ve başlatma bağı.
- `tests/`: Python/XML ve JavaScript mapping regresyonları.

## 1. Raspberry Pi bağımlılıkları

Raspberry Pi OS / Debian, çalışan X11 veya Wayland masaüstü oturumu ve
Mixxx 2.5 hedeflenir. Uygulama ses üretmez; Mixxx'in mevcut ALSA ses ayarını
değiştirmek gerekmez. JACK, `snd-virmidi`, ALSA loopback veya `.asoundrc`
değişikliği gerekmez. Sistemde Mixxx zaten kuruluysa yeniden kurmayın.

Bu klasörü Pi'de örneğin `~/touch-fx` dizinine kopyalayın. Aşağıdaki kurulum
komutlarını Pi üzerinde çalıştırın; Windows terminali üzerinde değil.
APT eksik paketler için yönetici yetkisi ister. Yetki yoksa yönetici kurmalı;
parola sıfırlama veya yetki kontrollerini gevşetme bu projenin parçası değildir.

```bash
sudo apt update
sudo apt install python3 python3-venv python3-pip python3-pyqt5 alsa-utils
cd ~/touch-fx
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python -c "import PyQt5.QtWidgets, mido, rtmidi; print('Bağımlılıklar hazır')"
```

`--system-site-packages`, APT ile kurulan PyQt5'i sanal ortamdan görünür yapar.
Pi'de Qt'yi pip ile kaynak koddan derlemek yerine dağıtımın ARM paketini
kullanıyoruz. [Debian PyQt5 paketi](https://packages.debian.org/trixie/python3-pyqt5).
Başka bir desteklenen platformda uygun wheel varsa alternatif komut
`python -m pip install PyQt5` olabilir; Pi için yukarıdaki APT yolu tercih edilir.
[PyQt indirme bilgisi](https://riverbankcomputing.com/software/pyqt/download).

`python-rtmidi` için uygun wheel bulunmaz ve derleyici/ALSA header hatası olursa:

```bash
sudo apt install build-essential python3-dev libasound2-dev pkg-config
python -m pip install -r requirements.txt
```

`sudo pip`, `--break-system-packages` veya uygulamayı root olarak çalıştırma
gerekmez. Bu geliştirme sırasında Pi'ye paket kurulmadı.

## 2. ALSA sequencer kontrolü

```bash
ls -l /dev/snd/seq
aconnect -l
```

`/dev/snd/seq` yoksa yönetici geçici olarak sequencer modülünü yükleyebilir:

```bash
sudo modprobe snd_seq
```

Modül bulunamıyorsa çalışan kernel'in ALSA sequencer desteği kontrol edilmeli;
`chmod 666`, root GUI veya rastgele ses yapılandırması değişikliği kullanmayın.
Permission denied durumunda `id` ve `getfacl /dev/snd/seq` ile oturum/aygıt
izinlerini inceleyin; cihazın gerçekten `audio` grubuyla yönetildiği sistemde
üyelik gerekiyorsa yönetici yalnız ilgili kullanıcı için düzenlesin.

Kod backend'i açıkça `mido.backends.rtmidi/LINUX_ALSA` seçer. Oluşturulan port
adı **TouchFX_Virtual**, ALSA client adı **TouchFX** olur. Listede
`TouchFX:TouchFX_Virtual 128:0` benzeri görünür; sayı oturumdan oturuma değişir.
Python çıkışı, Mixxx açısından MIDI giriş kaynağıdır.
[Mido RtMidi sanal portları](https://mido.readthedocs.io/en/stable/backends/rtmidi.html).

## 3. Mapping dosyalarını yerleştirin

Mixxx'i normal biçimde kapatın. Standart kullanıcı mapping klasörü Linux'ta
`~/.mixxx/controllers/` dizinidir. Mixxx özel ayar diziniyle başlatılıyorsa
Preferences → Controllers içindeki **Open User Mapping Folder** ile gerçek
klasörü açın. `/usr/share/mixxx/controllers/` veya
`/usr/local/share/mixxx/controllers/` sistem dosyalarını değiştirmeyin.
[Resmî dosya konumları](https://github.com/mixxxdj/mixxx/wiki/Controller-Mapping-File-Locations).

```bash
cd ~/touch-fx
mkdir -p ~/.mixxx/controllers
cp -i TouchFX_Virtual.midi.xml TouchFX-scripts.js TouchFX-macros.js ~/.mixxx/controllers/
```

Üç dosya aynı klasörde olmalıdır. `cp -i` mevcut aynı adlı dosyayı sessizce
ezmez. Özel dizin kullanıyorsanız komuttaki hedefi gerçek klasörle değiştirin.

## 4. Çalıştırın ve Mixxx'e bağlayın

Önce Pi'nin grafik masaüstündeki terminalde Python'u başlatın:

```bash
cd ~/touch-fx
source .venv/bin/activate
python touchfx.py --windowed
```

Ardından Mixxx'i başlatın; sanal portun önceden oluşturulmuş olması gerekir.
Preferences → Controllers altında **TouchFX_Virtual** içeren cihazı seçin,
mapping listesinden **TouchFX Virtual XY Pad** seçip **Enabled** işaretleyin
ve Apply yapın. XML'deki controller kimliği işletim sistemi portunu kendi
başına bağlamaz; Mixxx'te bu seçim gereklidir. Cihaz görünmüyorsa Python açık
kalırken Mixxx'i yeniden başlatın. Yinelenen Touch FX örnekleri çalıştırmayın.

İlk bağlantıdan veya mapping yeniden yüklenmesinden sonra pad'e yeniden
dokunun. MIDI tek yönlüdür; Python'daki “FX ON (gönderildi)” etiketi Mixxx'ten
alınmış durum onayı değildir. Watchdog sonrası otomatik yeniden etkinleştirme
yerine yeni bir dokunma gerekir.

Diğer görünüm seçenekleri:

```bash
python touchfx.py
python touchfx.py --frameless
python touchfx.py --windowed --dry-run
```

İlk komut tam ekran, ikinci çerçevesiz maksimize, üçüncü MIDI açmadan konsol
denemesidir. Gerçek uygulamalardan yalnız birini aynı anda çalıştırın.
Çıkış butonu/Escape kapatır; FX OFF efekti kapatır ve pencereyi açık tutar.
Wayland'da Qt platform eklentisi eksikse dağıtımın `qtwayland5` paketi
gerekebilir. X11 oturumunda gerekirse `QT_QPA_PLATFORM=xcb python touchfx.py`
kullanın; Wayland/X11 ortam değişkenlerini gelişigüzel root oturumuna taşımayın.

## 5. Efekt hazırlığı ve kullanım

Mixxx'te dört deck gösteren bir skin seçin. Effect Unit 1'e bir efekt
(örneğin Filter) yükleyin; kullanacağınız efekt slotunu etkinleştirin ve
slotun metaknob'unu Super1/superknob'a bağlayın. Kod efekt seçimini değiştirmez.
Slot boşsa, slot kapalıysa veya parametreler superknob'a bağlı değilse X
hareketi duyulur sonuç üretmeyebilir. Dry/Wet alt kenarda 0 olduğundan orada
efekt ON olsa bile kuru ses duyulur. Normal Dry/Wet mix modu kullanın.

- X: sol 0 → sağ 127. Y: alt 0 → üst 127.
- Momentary: dokununca önce X/Y, sonra ON; bırakınca OFF.
- Latch: bırakınca aynı X/Y ve ON korunur. Latch'i kapatmak, parmak pad'de
  değilse hemen OFF gönderir; parmak pad'deyse bırakılmasını bekler.
- Deck seçimi: eski gate kapanır; yeni deck atanır, aynı X/Y gönderilir.
  Yeni deck'te efekt için yeniden dokunun. Dört ayrı eşzamanlı latch yoktur;
  bütün deck'ler tek bir Effect Unit 1'i paylaşır.
- İlk dokunan parmak pad'i yönetir; ek parmaklar X/Y'yi ele geçirmez.
  Aktif dokunma iptal edilirse veya dokunurken pencere odağı kaybolursa
  güvenlik için OFF gönderilir. Fareyle de denenebilir.
- FX OFF, kapanış, SIGINT ve SIGTERM OFF gönderir. `kill -9`, kopmuş MIDI
  veya kilitlenmiş Python OFF gönderemez; mapping watchdog'u devreye girer.
- Python 250 ms'de bir heartbeat gönderir. Mixxx yaklaşık 1,5–1,75 saniye
  mesaj alamazsa üniteyi bypass eder; latch bunu engellemez. Bu gerçek zamanlı
  güvenlik garantisi değildir: Mixxx kendisi kilitlenirse JS de çalışamaz.

### MIDI protokolü

Python kanalları **0–3**, MIDI uygulamalarının bazı ekranlarında **1–4** olarak
görünür; bunlar aynı dört kanaldır. Deck = Python MIDI channel + 1.

| İşlev | Mesaj | Değer |
| --- | --- | --- |
| X | CC 10 / 0x0A | 0–127 |
| Y | CC 11 / 0x0B | 0–127 |
| Deck seçimini bildir | CC 12 / 0x0C | 127; deck MIDI kanalından |
| Efekt aç | Note On, nota 60 / 0x3C | velocity 127 |
| Efekt kapat | Note Off, nota 60 | velocity 0 |
| Heartbeat | CC 119 / 0x77 | gate açık 127, kapalı 0 |

XML her kanal için CC, Note On ve Note Off status'larını ayrı ayrı içerir.
JS, velocity 0 Note On'u da OFF kabul eder; eski deck'ten gelen gecikmiş
Note Off yeni deck'in efektini kapatmaz. XML `script-binding`, mesajı
`TouchFX.axis`, `TouchFX.gate` gibi fonksiyonlara yönlendirir.
[Mixxx MIDI scripting API](https://github.com/mixxxdj/mixxx/wiki/midi-scripting).

### Kullanılan Mixxx kontrolleri

Grup: `[EffectRack1_EffectUnit1]`.

- X → `super1`; Y → `mix`, `engine.setParameter` ile 0.0–1.0.
- Gate → `enabled`, `engine.setValue` ile 0/1.
- Deck N → `group_[ChannelN]_enable`, yalnız seçili deck 1.

Bunlar efekt **ünitesi** kontrolleridir; `[EffectRack1_EffectUnit1_Effect1]`
altındaki slot `enabled` ile aynı değildir. Spesifik parametre isterseniz JS
`axis` içindeki X atamasını ilgili efekt slotunun `parameter1` kontrolüne
uyarlayın; slot ve yüklenen efektin parametre sırasını önce doğrulayın.
[Mixxx 2.5 kontrol referansı](https://manual.mixxx.org/2.5/en/chapters/appendix/mixxx_controls).

## 6. Bağlantı teşhisi ve kabul kontrolü

Python açıkken başka terminalde:

```bash
aconnect -l
aseqdump -l
```

Listeden TouchFX portunun o anki sayısal adresini bulun. Örneğin gerçekten
`128:0` ise `aseqdump -p 128:0` ile pad mesajlarını gözleyin. CC 10/11,
nota 60 ve seçili kanal görünmelidir. Sayıyı sabitlemeyin.

Mixxx normalde controller etkinleştirilince aboneliği kendisi oluşturur.
Oluşmadıysa, yalnız listede doğruladığınız kaynak/hedef adresleriyle
`aconnect KAYNAK_CLIENT:PORT MIXXX_CLIENT:PORT` bağlanabilir; doğru Mixxx
input portunun mapping'le açılmış olması gerekir. Tüm ALSA bağlantılarını
kesen `aconnect -x` kullanmayın. [ALSA aconnect belgesi](https://raw.githubusercontent.com/alsa-project/alsa-utils/master/seq/aconnect/aconnect.1).

Kabul sırası:

1. Dört köşede X/Y uç değerleri ve Mixxx Super1/Mix hareketi.
2. Her deck'te yalnız o deck'in Unit 1 ataması; momentary bas/bırak.
3. Latch bas/bırak; parmak yokken Latch kapatınca OFF.
4. Latch açık deck değiştirme: eski efekt kapalı, yeni dokunmaya kadar OFF.
5. FX OFF ve uygulama çıkışında bypass.
6. Düşük sesle bağlantı kesilmesi/uygulama durması durumunda watchdog bypass.
7. D2 ile çakışmayan ünite kullanımı, gerçek ekran dokunma/çoklu dokunma,
   ALSA gecikmesi ve uzun kullanım testi.

## Geliştirici testleri

```bash
python -B -m unittest discover -s tests -p 'test_*.py' -v
node --test tests/test_mapping.cjs
node --check TouchFX-scripts.js
```

Node yalnız JS testleri içindir; Pi'de çalıştırmak için gerekmez. İlk teslimde
19 Python/XML ve 13 mock-engine JS testi geçmişti. Sonraki doğrulamada Windows
üzerinde yalnız proje içindeki `.test-venv` ortamına PyQt5 5.15.11 ve Mido 1.3.3
kuruldu. Gerçek Qt widget'ları `offscreen` platformunda çalıştırıldı.

**İlk Qt doğrulaması: 43 Python testi + 13 Node testi = 56 test geçti, atlanan yok.**
Python kümesi 19 çekirdek/XML, 17 GUI/transport ve 7 protokol entegrasyon
testinden oluşur. PyQt5 veya Mido bulunmayan ortamda ilgili testler açıkça
`skipped` görünür; bu GUI başarısı sayılmaz.

Gerçek pencerenin 800 pikseli aşması tespit edilip durum metnine satır sarma
eklendi. Son 800×480 görüntülerin ikisi de görsel olarak incelendi:
`validation/qt-20260909-03/`. İlk iki denemenin hatalı/font eksik görüntüleri
karşılaştırma için korunur; kabul edilen görüntüler değildir.

Windows geliştirme ortamını yeniden kurmak için (Pi'de Qt için APT yolunu kullanın):

```powershell
python -m venv .test-venv
.test-venv/Scripts/python.exe -m pip install PyQt5==5.15.11 mido==1.3.3
.test-venv/Scripts/python.exe -B -m unittest discover -s tests -p 'test_*.py' -v
.test-venv/Scripts/python.exe -B tests/render_gui.py --output validation/yeni-deneme
```

Çıktı dizini yeni olmalıdır. Render aracı MIDI açmaz. Windows offscreen Qt
font bulamazsa yalnız test için mevcut sistem Segoe UI fontunu yükler;
font dosyası kopyalanmaz veya dağıtılmaz. Uygulamanın Pi kodunda Windows yolu yoktur.

**Sınır:** Widget/fare olayları gerçek PyQt5 ile; çoklu dokunma verileri
sentetik olarak test edildi. Mido mesaj kodlaması, gerçek XML binding'leri ve
gerçek Qt JavaScript motoru birlikte çalıştı; Mixxx `engine` API'si ve MIDI
portu test doubles kullanır. Bu, gerçek ALSA bağlantısı, Mixxx ses işleme,
Linux/Wayland dokunmatik sürücüsü veya Pi donanım kabulü değildir.
Ayrıntı: `validation/VALIDATION-2026-09-09.md`.

**Entegrasyon sonrası:** Windows test ortamında 61 Python + 20 Node testi
geçti (81, atlanan yok). Linux'ta bağımlılıksız 32 test geçti; PyQt5/Mido
bulunmayan WSL ortamında 29 GUI/protokol testi açıkça atlandı. Ayrıca mevcut
19 public-build integration ve 10 config testi Linux'ta geçti. 61 gerçek
kaynak varlığı hash ile doğrulanarak geçici rootfs'te 71 kurulum dosyası
oluşturuldu ve doğrulandı; gerçek cihaz/imaj kullanılmadı.

**10 Eylül ek kontrolü:** Sıkı Linux dizin izinleri düzeltildi; doğrulanabilir,
yalnız seçilmiş kaynakları içeren tar.gz paketleyici eklendi. Windows'ta 67
Python + 20 Node testi geçti; yalnız Linux'a özel izin testi orada atlandı,
WSL'de 12 kurucu testinin tamamıyla birlikte geçti. Paket kapsamı ve kullanım
sınırları için `integration/README.md` içindeki 10 Eylül bölümüne bakın.
