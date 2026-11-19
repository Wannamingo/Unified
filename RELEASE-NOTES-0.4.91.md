# Unified 0.4.91 Preview

Android ARM64, API 31 or newer. This preview is the Vulkan 1.3 Lab build. The gaming comparison used Mali-G615 on ANBERNIC RG477M. The previous stable downloads remain available in Releases.

## Local-read optimization

Unified can decode six packed color constants during draw preparation. A separate experiment skips a texture-size read and three reciprocal instructions in the measured NFS Most Wanted fragment shader when the application supplies verified coordinate factors. Draws without those factors keep the original query.

The exact-shader experiment requires an application that supplies the matching push data, such as the test HakuX build used for validation. Both driver flags default to off. The preview build does not require an APK update for ordinary driver import.

- Color constants. Enable with `UNIFIED_PUSH_COLORS=1` or `debug.unified.push_colors=1`.
- Local-read coordinate factors. Enable with `UNIFIED_LOCAL_READ_RCP=1` or `debug.unified.local_read_rcp=1`, with color predecoding enabled and a compatible application.
- Compact FXAA and attachment feedback loop were disabled for the reported tests. Ordinary FXAA was enabled in the game.

### RG477M results

At 2× in NFS Most Wanted, the median of the measured GPU pass fell from **1.167 ms to 1.043 ms (−10.6%)**, with 341 passes per mode. The exact captured shader produced **9,830,400 bit-identical pixels** across eight native test inputs. In one short game pair, the reported FPS was **30.04 / 29.98** and p95/p99 presentation intervals were in the **49–51 ms** bucket for both modes. The end of the two runs followed different trajectories. A sustained FPS or frame-pacing benefit remains unverified.

### RG476H and RG Rotate

The same binary passed 156 native vertex-divisor draw and readback cases on each device (Mali-G57 and Mali-G52), with matching output checksums. The local-read coordinate experiment is restricted to the Mali-G615 path tested above.

NFS Most Wanted loaded slot 1 and rendered a stationary scene at 2× on both devices. In a short comparison with Unified 0.4.53, RG476H reported **30.00 / 30.00 FPS**, and RG Rotate reported **20.66 / 20.43 FPS**. Each observed window was approximately ten seconds. These checks cover loading and rendering and give an indicative FPS comparison.

This binary has zero reported Vulkan conformance version.

## Русский

Unified 0.4.91 — предварительный выпуск для Android ARM64 с API 31 и новее. Используется сборка Vulkan 1.3 Lab. Игровая проверка этого бинарника прошла на RG477M с Mali-G615.

Драйвер может распаковать шесть цветовых констант при подготовке отрисовки. Отдельный режим пропускает запрос размера текстуры и три обратные величины, если совместимое приложение передаёт проверенные множители. Оба режима выключены по умолчанию.

В NFS MW при 2× медиана целевого GPU-прохода сократилась с **1,167 до 1,043 мс (−10,6%)**. В нативном тесте совпали все **9 830 400 пикселей**. В короткой игровой паре получено **30,04 / 29,98 FPS**, p95 и p99 попали в один диапазон **49–51 мс**. Повторяемый прирост FPS и плавности не подтверждён.

Тот же бинарник прошёл по 156 нативных сценариев отрисовки с делителями вершинных атрибутов и проверкой результата на RG476H (Mali-G57) и RG Rotate (Mali-G52). Эксперимент с множителями local read на этих GPU выключен.

NFS MW загрузила слот 1 и отрисовала неподвижную сцену при 2× на обеих консолях. В коротком сравнении 0.4.53 и 0.4.91 получено **30,00 / 30,00 FPS** на RG476H и **20,66 / 20,43 FPS** на RG Rotate. Окна составляли около десяти секунд на режим. Эта проверка подтверждает запуск и отрисовку, а показатели FPS дают ориентир для данной сцены.

## Assets

- `Unified-0.4.91-Local-Read-RCP-lab-Android-API31.zip` is the tested driver package for manual import.
- `SHA256SUMS.txt` records the public asset checksum. The tag contains the driver sources and the patch for the pinned Mesa revision.
