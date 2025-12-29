# PatriciaLearn - Maksimum Performans Build Yönergesi

## Adım 1: MSYS2 MinGW Terminal'ini Aç

1. Windows'ta **MSYS2 MinGW 64-bit** terminal'ini açın
   - Başlat menüsünden: `MSYS2 MinGW 64-bit`
   - Veya: `C:\msys64\mingw64.exe`

## Adım 2: Proje Dizinine Git

```bash
cd /c/Github/Chess\ Dev/PatriciaLearn/engine
```

Veya Windows path kullanarak:
```bash
cd /c/Github/Chess\ Dev/PatriciaLearn/engine
```

## Adım 3: Maksimum Performans Build Komutu

**CPU özelliklerini otomatik algılayan versiyon (ÖNERİLEN):**

```bash
g++ -O3 -std=c++20 -march=native -mtune=native -ffast-math -funroll-loops -fomit-frame-pointer -DNDEBUG -Isrc src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

**Manuel AVX512 desteği (Eğer CPU'nuz destekliyorsa):**

```bash
g++ -O3 -std=c++20 -march=native -mtune=native -mavx2 -mavx512f -mavx512bw -mavx512dq -mavx512vl -mbmi2 -mpopcnt -ffast-math -funroll-loops -fomit-frame-pointer -DNDEBUG -Isrc src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

**AVX2 desteği (AVX512 desteklemiyorsa):**

```bash
g++ -O3 -std=c++20 -march=native -mtune=native -mavx2 -mbmi2 -mpopcnt -ffast-math -funroll-loops -fomit-frame-pointer -DNDEBUG -Isrc src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

## Adım 4: DLL'leri Kopyala (Eğer Gerekirse)

Build başarılı olduktan sonra, eğer `patricia.exe` çalıştırırken DLL hatası alırsanız:

```bash
cp /c/msys64/mingw64/bin/libwinpthread-1.dll .
```

## Adım 5: Test Et

```bash
./patricia.exe bench
```

## Flag Açıklamaları

- `-O3`: Maksimum optimizasyon
- `-std=c++20`: C++20 standardı
- `-march=native`: CPU özelliklerini otomatik algıla (AVX512, AVX2, BMI2, POPCNT vb.)
- `-mtune=native`: CPU'ya göre tuning
- `-ffast-math`: Hızlı matematik işlemleri
- `-funroll-loops`: Döngü açma optimizasyonu
- `-fomit-frame-pointer`: Frame pointer'ı kaldır (küçük hız artışı)
- `-DNDEBUG`: Debug kodlarını devre dışı bırak
- `-Isrc`: Header dosyaları için include path
- `-static-libgcc -static-libstdc++`: libgcc ve libstdc++'yi statik bağla

## Sorun Giderme

**"g++: command not found" hatası:**
- MSYS2 MinGW 64-bit terminal'ini kullandığınızdan emin olun
- `which g++` komutu ile g++ yolunu kontrol edin

**DLL hatası:**
- Adım 4'ü uygulayın
- Veya `patricia.exe` ile aynı dizinde `libwinpthread-1.dll` olduğundan emin olun

**Build hatası:**
- Kaynak dosyaların varlığını kontrol edin: `ls src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c`
- Compiler versiyonunu kontrol edin: `g++ --version`

