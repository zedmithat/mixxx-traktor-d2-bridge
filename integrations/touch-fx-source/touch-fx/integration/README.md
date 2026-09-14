# ZED Touch FX entegrasyonu

## Pad üzerinde efekt seçimi

ZED modunda Filter, Echo, Reverb, Flanger, Phaser, Bitcrusher, Autopan ve
Tremolo menüsü bulunur. Yalnız Unit 3/Slot 1 değiştirilir; Unit 3'ün diğer
slotları seçimde bypass edilir. D2 Unit 1/2 değiştirilmez. X metaknob,
Y Dry/Wet kontrolüdür; X'in tam işlevi seçilen efektin varsayılan parametre
bağlarına bağlıdır. Seçimde latch sesi kesilir, X merkeze ve Y sıfıra döner;
sonraki dokunuşa kadar efekt açılmaz.

CC13 (kanal 0/1) Mixxx `loaded_effect` indeksini ister; CC14 (kanal 0)
gerçek yüklü indeksi her 250 ms geri bildirir. Seçim onayı gelmeden pad
kilitlidir. Sayısal indeksler sabit varsayılmaz: adlar Mixxx'in son kaydettiği
`~/.mixxx/effects.xml` içindeki `VisibleEffects` sırasından okunur.
Bu özel profil yalnız Built-In efekt kataloğunu destekler; LV2/eksik katalogda
menü devre dışıdır. Yeni temiz public kurulumları `effect_factory.py` ile
24 yerleşik efekt kataloğunu ve yedi makro parametre preset'ini ilk açılıştan
önce oluşturur. Mevcut kataloglar ve private profiller değiştirilmez.
Eski imajda dosyalar yoksa yeniden başlatma yeterli olmayabilir; eksik katalog
doğrulanıp yedekli bakım yapılmalıdır. Mixxx tercihlerinden efekt görünürlüğü/sırası değiştirilirse
Mixxx ve Touch FX birlikte yeniden başlatılmalıdır; açık oturumda listeyi
yeniden sıralamayın. Böylece kaydedilmiş katalog ile çalışan Mixxx eşleşir.

Seçici eski `.img`/`.img.xz` dosyalarına sonradan eklenmiş sayılmaz.
Canlı özel USB güncellemesi kullanıcıya ait runtime dizini ve systemd
drop-in'i kullanır; root/sudo, NVMe veya parola politikası değişikliği gerekmez.

**Güncel genel kaynak paketi: `touchfx-source-20260910-04.tar.gz`.** Aşağıdaki
ikinci/ilk paket kayıtları tarihsel kanıttır. Üçüncü paket gerçek Raspberry Pi
OS imajındaki `etc/os-release -> ../usr/lib/os-release` (ve standart mutlak
eşdeğeri) bağlantısını kabul eder; yalnız rootfs içindeki sabit hedef okunur,
diğer symlink hedefleri reddedilir.

ARM64 Debian'da Qt JavaScript testleri için `python3-pyqt5.qtquick` ayrıca
gerekir. Bu uygulamanın runtime gereksinimi değil, test gereksinimidir;
QtQml yoksa ilgili testler açıkça atlanır. Özel test imajına testleri gerçekten
çalıştırmak için bu paket de eklenir.

Kullanıcının seçtiği ayrı **private-owner-test** imaj akışı genel pi-gen
kurulumundan ayrıdır: onaylı r11'in yeni normal dosya kopyası büyütülür,
paketler yalnız kopyaya kurulur, D2 binary/pinleri korunur. Özgün imaj/Pi/USB
değişmez. Bu özel kopyada boş Unit 3'e Filter preset'i ve yalnız onun ilk
slotunu etkinleştiren ek mapping kodu konur; ünite açılışta bypass kalır.
İlk iki boş üniteye efekt yüklenmez. Özel imaj kişisel yapılandırmayı miras
aldığı için yayınlanmamalı; public kaynak/lisans kapılarını tamamlamaz.

## Ne entegre edildi?

- pi-gen `stage-zed/20-touch-fx`: önceki D2 profilinin ardından çalışır.
- APT bağımlılıkları: `python3-mido`, `python3-pyqt5`, `python3-rtmidi`, `qtwayland5`.
- `/usr/local/lib/zed-touch-fx/`: gereken üç Python modülü:
  `touchfx.py`, `touchfx_core.py`, `zed_session.py`.
- `~/.config/systemd/user/zed-touch-fx.service`: grafik oturumuna bağlı,
  root olmayan kullanıcı servisi. Python ve MIDI portları hazır olunca
  `READY=1` bildirir; başlangıçta pad gizlidir.
- `mixxx-d2.service.d/50-touch-fx.conf`: Mixxx başlatılırken servisi ister ve
  hazır olmasını bekler. `Wants` kullanılır; Touch FX hatası Mixxx sesini
  durduracak bir `Requires/BindsTo` bağı oluşturmaz. Başlangıç beklemesi 15 sn ile sınırlı.
- `~/.mixxx/controllers/TouchFX-ZED.*`: Unit 3 için oluşturulmuş XML/JS.
- `/usr/share/mixxx/skins/zed-touchfx`: orijinal ZED'den ayrı skin kopyası;
  üst çubukta **TOUCH FX** düğmesi vardır. Özgün `zed` dizinine dokunulmaz.
- Sway'de yalnız `org.zed.TouchFX` için fullscreen kuralı eklenir.

Systemd'nin `Type=notify` hazır bildirimi
[resmî servis belgesindeki](https://github.com/systemd/systemd/blob/main/man/systemd.service.xml)
mekanizmayı kullanır. Qt 5'in Wayland eklentisi
[qtwayland5 paketinden](https://packages.debian.org/trixie/qtwayland5) gelir.

## D2 ile çakışma önlemi

Doğrulanmış D2 mapping'inin `deckNumber()` işlevi fiziksel yüzeye göre 1/2
döndürür; efekt erişimi bu ünitelere bağlıdır. Touch FX **Unit 3** kullanır.
Bağımsız orijinal Touch FX mapping'i Unit 1 olarak kalır; ZED'de onu değil,
üretilen **ZED Touch FX — Unit 3** mapping'ini kullanın. Aynı MIDI portuna
iki mapping/iki Python örneği bağlamayın.

Kurucu kabul edilmiş D2 JS dosyasının hash'ini doğrular:
`b82e96670f6b59de60cf837265e1e048dfcaf1c69de8f3447331d35197734ee3`.
Farklı bir D2 sürümünde inceleme olmadan kurulumu sürdürmez; bu hash'i
kontrolü geçmek için değiştirmeyin. Kabul edilmiş pin dosyaları ve binary'ler
bu entegrasyonda değiştirilmedi.

ZED iki-deck profilini korur. Pad'de Deck 3/4 görünür ama devre dışıdır;
JS de bu kanalları reddeder. Bağımsız uygulamanın dört deck desteği değişmedi.
Mixxx Unit 3 sağlamıyorsa entegre mapping efekt kontrollerine yazmaz ve
log'a hata kaydeder; Unit 1/2'ye otomatik geri düşmez.

## Açılış ve geri dönüş

1. Sway oturumu → Mixxx user service → Touch FX service başlatılır.
   Qt oluşturulmadan önce Wayland soketi en fazla 10 sn beklenir. Açıkça
   verilmiş `WAYLAND_DISPLAY` korunur; yoksa tek hazır `wayland-0/1` seçilir.
   İki soket hazırsa tahmin edilmez; eksik/kararsız ekran ortamında servis
   hata ile çıkar. MIDI portları bu kontrolden sonra açılır.
2. Python aynı client/port adlarıyla iki ALSA yönünü açar:
   **TouchFX_Virtual**. Mixxx'e X/Y/gate çıkışı ve Mixxx'ten yalnız pad açma girişi.
3. Portlar açılınca systemd'ye hazır bildirimi gider; Mixxx açılabilir.
4. Skin düğmesi `[Skin],touch_fx` değerini değiştirir. JS, yalnız kendi MIDI
   portuna `B0 78 7F` gönderir; callback Qt ana döngüsünde pad açılmasını ister.
   D2 mesajları okunmaz veya başka porta aktarılmaz. Başlangıçta açma mesajı yoktur.
5. **Mixxx'e dön / Escape**: gate kapanır, pencere gizlenir. Süreç/portlar
   yaşamaya devam eder; tekrar açışta portu yeniden keşfetmek gerekmez.
6. Oturum bittiğinde servis kapanır. Anormal kesilmede mapping watchdog'u
   Unit 3'ü bypass eder; Unit 1/2 değişmez.

MIDI tek yönlü gate geri bildirimi içermez: ekrandaki ON, gönderilen komuttur,
Mixxx'ten doğrulanmış efekt/ses durumu değildir. Cihaz çökmesi/servis yeniden
başlaması sonrasında otomatik port yeniden keşfi gerçek Pi kabul testidir;
gerekirse controller'ı yeniden etkinleştirin. Oynatma sırasında Mixxx'i
kendiliğinden yeniden başlatan bir kurtarma komutu eklenmedi.

## Yeni imajda kullanım

Mevcut pi-gen `stage-zed` akışı yeni alt aşamayı da çalıştırır; kaynak kontrolüne
alınırken `20-touch-fx/00-run.sh` çalıştırma biti korunmalıdır. Public build
girişinin mevcut kaynak/lisans/hardware kapıları kaldırılmadı. Bu çalışma
ne tam imaj derledi ne de mevcut USB/NVMe/Pi'ye yazdı.

Kurucu yalnız ayrı, symlink olmayan ve adı `rootfs` olan hazırlanmış dizini
kabul eder. `--rootfs /` reddedilir; bunu canlı kurulum aracı olarak kullanmayın.
Yalnız kontrollü yeni build rootfs'i için otomatik çağrı:

```bash
python3 -B touch-fx/integration/install_rootfs.py --rootfs /build/zed/stage-zed/rootfs
```

Girdi olarak önceki stage'in orijinal `zed` skin'i, config'i, Sway dosyaları,
Mixxx service'i ve kabul edilmiş D2 JS'i gerekir. Var olan Touch FX hedefleri,
symlink'ler ve beklenmeyen config/skin yapısı reddedilir. Girdi doğrulaması
yazmadan önce yapılır. I/O sırasında kesilmiş kısmi kurulumda yeni temiz
staging kökü hazırlayın; kurulum atomik rollback iddiasında bulunmaz.

Kurulum makbuzu `/opt/zed/touch-fx-integration.json`, başlangıç config/Sway
kopyaları `/opt/zed/touch-fx-backup/` altındadır. Makbuz hem yeni dosyaların
hem korunan skin/D2 dosyalarının hash'lerini taşır. Aynı kaynaklarla tekrar
çağrı yalnız doğrular; değişmiş kaynaklarda eski kurulumu sessizce bırakmaz,
yeni staging kökü ister. `.test-venv`, test resimleri veya kişisel medya
runtime'a kopyalanmaz.

## İlk kullanımda hâlâ gerekenler

**Unit 3'e efekt kendiliğinden yüklenmez.** Kullanıcı tercihini/sesini
değiştirmemek için preset ve slotlar korunur. Unit 3'ü gösteren bir Mixxx
skin/efekt arayüzünden örneğin Filter yükleyin, slotu açın ve metaknob'u
Super1'e bağlayın; ardından `zed-touchfx` skin'ine dönün. Boş slot veya
Super1 bağlantısı olmadan pad hareketi duyulur efekt üretmeyebilir.

Gerçek Pi üzerinde, düşük sesle şu kabul sırasını uygulayın:

1. `systemctl --user status zed-touch-fx.service` ve `aconnect -l` ile hazır portlar.
2. Mixxx Preferences → Controllers: `TouchFX_Virtual`, **ZED Touch FX — Unit 3** aktif.
3. TOUCH FX düğmesiyle açılış, Mixxx'e dön ile dönüş ve tekrar açılış.
4. Deck 1/2 momentary ve latch; dönüşte gate kapanması; D2 Unit 1/2'nin değişmemesi.
5. Gerçek dokunma ve aç/kapat sırasında alttaki waveform/transport'a yanlış
   dokunma geçmediğinin kontrolü; Wayland odak/fullscreen davranışı.
6. Unit 3 ses/parametre/Super1 testi, kopma watchdog'u ve uzun süreli kullanım.

Bu işlemler henüz Pi'de yapılmadı. Qt ekransız test Sway/Mixxx kabulü değildir.
Servisi incelemek için `journalctl --user -u zed-touch-fx.service` kullanılır;
parola veya ağ kimlik bilgisi bu loglara eklenmez.

## Geri dönüş

Normal kullanımda yalnız **Mixxx'e dön** yeterlidir. Entegrasyondan tamamen
vazgeçilecekse oynatma durmuşken Mixxx'te TouchFX controller'ını devre dışı
bırakıp orijinal `zed` skin'ini seçin. Yeni imaj tarifinden Touch FX stage'ini
çıkarıp temiz bir rootfs hazırlayın. Eski test/kabul imajları aynen korunur.
Bir süre kullanılmış sistemde başlangıç config yedeğini körlemesine geri
kopyalamayın; bu sonraki kullanıcı ayarlarını kaybettirebilir.

## Doğrulama

### 10 Eylül ikinci paket — önceki paketin yerine

`releases/touchfx-source-20260910-02.tar.gz` güncel kaynak paketidir.
`-01` yalnız tarihsel kanıttır; yeni build için kullanılmamalıdır.

- Yerel Mixxx C++ kaynağında `[EffectRack1],num_effectunits` üretimi yok;
  eski kontrol dump'ında/dokümanda görünen değer mock testleri yanıltıyordu.
  Unit 3 artık kendi `num_effectslots > 0` kontrolüyle doğrulanır.
  Bu slot kontrolü hem yerel `effectchain.cpp` içinde oluşturulup güncellenir
  hem [resmî kontrol listesinde](https://manual.mixxx.org/2.7/en/chapters/appendix/mixxx_controls)
  tanımlanır. Slot sayısının pozitif olması, bir efektin yüklü olduğu anlamına gelmez.
- Mapping yeniden başlatıldıktan sonra eski callback pencere açamaz.
  Kopmuş skin bağlantısı watchdog sırasında yeniden bağlanır; kendiliğinden açılış yoktur.
- Wayland hazır olma kontrolü, gerçek Linux Unix soketi ve Qt/MIDI açılmadan
  önce hata dönüşü test edildi. Bu bir gerçek compositor/dokunmatik kabulü değildir.
- Kurucunun ürettiği XML ve birleşik JS üzerinden gerçek Mido kodlama + Qt
  JavaScript motoru zincirine 10 test eklendi. Mixxx engine hâlâ mock'tur;
  sentetik staging girdileri, gerçek kurucu ile dönüştürülür.
- Windows: 86 Python testinden 84 geçti, iki Linux testi açık skip; 22 Node geçti.
  Linux: iki skip dahil 22 kurulum/oturum testi geçti. Toplam 108 farklı test doğrulandı.
- Güncel staging kanıtı: `../validation/zed-integration-20260910-02/`.

### 10 Eylül ek kontrolü ve kaynak paketi

Sıkı `umask 0077` altında yeni runtime/skin dizinlerinin Pi kullanıcısı için
erişilemez kalması düzeltildi. Yalnız kurucunun yeni açtığı dizinler 0755,
kurulum dosyaları ve makbuz 0644 olur; mevcut kullanıcı dizini izinleri korunur.
Gerçek Linux geçici rootfs testi bunu doğruladı.

- Windows: 68 Python testinden 67 geçti; yalnız POSIX izin testi açıkça atlandı.
- Linux: 12 kurucu testinin tamamı, atlanan POSIX izin testi dahil geçti.
- Node: 20 mapping testi geçti. İki ortam birlikte 88 farklı testi doğruladı.
- Yeni staging kanıtı: `../validation/zed-integration-20260910-01/`.

Kaynak paketi oluşturma (repo kökünden, yeni çıktı adı kullanın):

```bash
python3 -B touch-fx/integration/package_sources.py --output touchfx-source.tar.gz
sha256sum touchfx-source.tar.gz
tar -tzf touchfx-source.tar.gz
```

Paket 26 açıkça seçilmiş kaynak/test/stage dosyası ve SHA256 manifesti içerir.
Sanal ortam, kişisel ayarlar, medya, D2 binary'si, önceki imaj veya test görselleri
pakete girmez. Shell çalıştırma biti arşivde 0755 olarak korunur. Aynı girdiler
aynı arşivi üretir; var olan çıktı dosyası üzerine yazılmaz.

**Bu paket kurulabilir imaj veya canlı Pi güncellemesi değildir.** Yeni boş
bir dizinde açıp `touchfx-source-overlay/touch-fx/README.md` ile başlayın.
Bağımsız pad bu kaynaklarla çalışabilir; ZED entegrasyonu ayrıca mevcut
doğrulanmış pi-gen projesini ve önceki stage varlıklarını gerektirir.
`review_zed_staging.py` de repo içindeki public-candidate girdilerini ister;
bu girdiler pakette yoktur. Overlay'i mevcut projeye körlemesine kopyalamayın,
değişiklikleri karşılaştırarak yalnız temiz bir build çalışma alanına taşıyın.
Kaynak/test paketi hazırlanması yayın lisans kapısını veya donanım kabulünü kapatmaz.

### Önceki 9 Eylül kontrolü

- Windows/PyQt5: **61 Python + 20 Node = 81 test geçti**, atlanan yok.
- WSL: 32 bağımlılıksız Python testi geçti; Qt/Mido olmadığı için 29 açık skip.
- Mevcut public-build integration 19 ve config 10 testi Linux'ta geçti.
- `systemd-analyze --user verify` servis dosyasını başlatmadan doğruladı;
  Windows mount'unun izinleri için uyarı verdi. Kurucu image dosyalarını 0644 yazar.
- Gerçek public-candidate varlıklarının 61 dosyası hash ile doğrulandı;
  geçici rootfs'te 71 kurulum dosyası ve tekrar kurulum doğrulandı.
- Son staged çıktı: `../validation/zed-integration-20260909-03/`.
- Entegre pad görüntüsü: `../validation/zed-ui-20260909-01/`; 800×480 gözle incelendi.

Yeni Qt5/Mido paketlerinin public dağıtım kaynak/lisans kanıtı, tam yeni imaj
ve gerçek donanım testleri ayrı açık işlerdir. `public_release_ready=false`.
